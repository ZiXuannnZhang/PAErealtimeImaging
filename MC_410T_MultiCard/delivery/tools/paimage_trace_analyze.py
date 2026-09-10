"""Read schema-1/2 binary traces independently of GUI/source cumulative counters."""
import argparse, hashlib, heapq, json, struct, socket, tempfile, zipfile
from pathlib import Path

RECORD=struct.Struct('<QQQQIIIHHHHHhBB4sH')
TIMING=struct.Struct('<QQQQQIIIHhHHI')

def _target_cards(targets):
    result={}
    for card,item in enumerate(targets or []):
        if isinstance(item,dict):
            ip=item.get('ip') or item.get('address') or item.get('targetIP')
            card=int(item.get('card',item.get('cardId',card)))
        else:ip=item
        if not isinstance(ip,str):
            raise TypeError(f'target entry {card} has no string IP: {item!r}')
        result[int.from_bytes(socket.inet_aton(ip),'little')]=card
    return result

def _round_summaries(groups,rounds,expected,cards,trace_complete):
    summaries=[]
    for spec in rounds or []:
        if spec.get("extraTrigger") is None:
            summaries.append(dict(trialId=spec.get("trialId",""),roundId=str(spec.get("roundId","")),
                classificationStatus="candidate/unknown",reason="extra trigger number not confirmed",perCard=[]))
            continue
        candidates=[g for g in groups.values() if g.get('rawPackets')
            and ('session' not in spec or g['session']==int(spec['session']))
            and ('startNs' not in spec or g['first']>=int(spec['startNs']))
            and ('endNs' not in spec or g['last']<=int(spec['endNs']))]
        by_card={}
        for g in candidates:by_card.setdefault((g['card'],g['trigger']),[]).append(g)
        extra=int(spec['extraTrigger'])&65535;effective=int(spec.get('effectiveCount',4000));basis=spec.get('basis','unknown')
        start=(extra+1)&65535;sequence=[(start+i)&65535 for i in range(effective)]
        per_card=[]
        for card in range(cards):
            complete=partial=0;missing=[];ambiguous=[]
            for trigger in sequence:
                matches=by_card.get((card,trigger),[])
                if len(matches)>1:
                    ambiguous.append(trigger);continue
                g=matches[0] if matches else None
                if g is None:missing.append(trigger)
                elif g['unique']==expected and not g['shortNonTailPayloads'] and not g['shortTailPayloads']:complete+=1
                else:partial+=1
            extra_matches=by_card.get((card,extra),[])
            extra_group=extra_matches[0] if len(extra_matches)==1 else None
            per_card.append(dict(card=card,physicalExpected=effective+1,extraExpected=1,effectiveExpected=effective,
                extraTrigger=extra,extraObserved=bool(extra_matches),extraAssociationAmbiguous=len(extra_matches)>1,
                extraComplete=(extra_group is not None and extra_group['unique']==expected and not extra_group['shortNonTailPayloads'] and not extra_group['shortTailPayloads']),
                effectiveComplete=complete,effectivePartial=partial,effectiveCompletelyUnseen=len(missing),
                effectiveObserved=complete+partial,effectiveAmbiguous=len(ambiguous),ambiguousEffectiveTriggers=ambiguous,missingEffectiveTriggers=missing,
                conservationVerified=bool(trace_complete and not ambiguous and len(extra_matches)<=1 and basis in ('user-confirmed','protocol-confirmed') and complete+partial+len(missing)==effective)))
        summaries.append(dict(trialId=spec.get('trialId',''),roundId=str(spec.get('roundId','')),
            startMarker=spec.get('startMarker'),endMarker=spec.get('endMarker'),extraTrigger=extra,
            basis=basis,operatorMarkerIsPhysicalEdge=False,perCard=per_card,
            classificationStatus='verified from complete application trace and confirmed round start' if all(c['conservationVerified'] for c in per_card) else 'candidate/unknown'))
    return summaries

def _timing(root):
    summary_path=root/'timing-summary.json';summary=json.loads(summary_path.read_text(encoding='utf-8')) if summary_path.exists() else {}
    records=[];bad=0;previous=0;out_of_order=0;identities=bytearray();unique=0;maximum=0
    for path in sorted(root.glob('timing-*.bin'),key=lambda p:int(p.stem.split('-')[-1])):
        data=path.read_bytes();bad+=len(data)%64!=0
        for values in TIMING.iter_unpack(data[:len(data)//64*64]):
            seq,start,end,session,correlation,tid,value0,value1,port,card,kind,flags,reserved=values
            if not seq:bad+=1;continue
            maximum=max(maximum,seq)
            if seq//8>=len(identities):identities.extend(bytes(seq//8-len(identities)+1))
            if identities[seq//8]&(1<<(seq%8)):bad+=1;continue
            identities[seq//8]|=1<<(seq%8);unique+=1
            if previous and seq<previous:out_of_order+=1
            previous=seq
            record=dict(sequence=seq,kind=kind,startNs=start,endNs=end,durationNs=max(0,end-start),session=session,
                correlation=correlation,threadId=tid,card=card,localPort=port,value0=value0,value1=value1,flags=flags)
            item=(record['durationNs'],seq,record)
            if len(records)<1000:heapq.heappush(records,item)
            elif item[:2]>records[0][:2]:heapq.heapreplace(records,item)
    records=[item[2] for item in sorted(records,reverse=True)]
    issued=int(summary.get('recordsIssued',0));missing=max(issued,maximum)-unique
    return dict(status='not-recorded' if not summary and not records else 'ok',summary=summary,
        records=unique,missingRecordCount=missing,malformedOrDuplicate=bad,fileOrderInversions=out_of_order,
        longestIntervals=records[:1000],timingIncomplete=bool(not summary or summary.get('timingIncomplete',True) or missing or bad))

def analyze(root,expected,base_port,samples=None,bits=32,cards=4,rounds=None):
    metadata_path=root/'run-config.json'
    metadata=json.loads(metadata_path.read_text(encoding='utf-8')) if metadata_path.exists() else {}
    if metadata:
        base_port=metadata.get('dataPort',base_port);samples=metadata.get('samples',samples)
        bits=metadata.get('bits',bits);cards=metadata.get('cards',cards)
    target_cards=_target_cards(metadata.get('targets',[]))
    if expected<1 or cards<1 or cards>32 or bits not in (16,32):
        raise ValueError('invalid trace configuration')
    if samples is not None and (samples<1 or (samples*(8 if bits==32 else 4)+1439)//1440!=expected):
        raise ValueError('samples/width disagree with expected packet count')
    summary_path=root/'trace-summary.json'
    summary=json.loads(summary_path.read_text()) if summary_path.exists() else {}
    issued=summary.get('recordsIssued',0)
    identities=bytearray((issued+8)//8)
    groups={};files=[];bad=0;duplicate_records=0;records=0;max_sequence=0;previous_file_sequence=0;file_order_inversions=0
    raw_count=0;feedback_count=0;short_count=0;unmapped_count=0;host_count=0
    current={};occurrences={};ambiguous_keys=set();unmatched_source=0;first_links={};schemas=set();trigger_jumps=[];sync_events=[]
    paths=list(root.glob('trace-*.bin'))
    for path in sorted(paths,key=lambda p:int(p.stem.split('-')[-1])):
        with path.open('rb') as source:digest=hashlib.file_digest(source,'sha256')
        size=path.stat().st_size;tail=size%RECORD.size if path.suffix=='.bin' else 0
        bad+=bool(tail)
        with path.open('rb') as trace_file:
            while chunk:=trace_file.read(64*8192):
                for r in RECORD.iter_unpack(chunk[:len(chunk)//64*64]):
                    seq,tick,session,link,tid,ip,value,local,source,length,trig,pkt,card,stage,reason,header,schema=r
                    records+=1;max_sequence=max(max_sequence,seq)
                    if previous_file_sequence and seq<previous_file_sequence:file_order_inversions+=1
                    previous_file_sequence=seq
                    if schema not in (1,2) or not seq:bad+=1;continue
                    schemas.add(schema)
                    if seq//8>=len(identities):identities.extend(bytes(seq//8-len(identities)+1))
                    if identities[seq//8]&(1<<(seq%8)):duplicate_records+=1
                    identities[seq//8]|=1<<(seq%8)
                    if stage==1:
                        raw_count+=1
                        if not base_port<=local<base_port+cards:
                            feedback_count+=1
                            if length<=64:continue
                            if local!=metadata.get('feedbackPort') or ip not in target_cards:
                                unmapped_count+=1;continue
                            card=target_cards[ip]
                        else:card=local-base_port
                        if length<4:short_count+=1;continue
                        pkt,trig=struct.unpack('<HH',header)
                    elif card<0 or stage not in (2,4,5,7):
                        if stage==2 and reason in (11,12,16):sync_events.append(dict(session=session,trigger=trig,reason=reason,count=value,timestampNs=tick))
                        continue
                    identity=(session,card,trig)
                    exact_object=schema==2 and (stage in (4,5,7) or (stage==2 and reason in (6,7,8,9,13,14,15,18,19,20,21,22,23,24,25,26)))
                    if stage==1:
                        stream=(session,card)
                        if current.get(stream)!=trig:
                            previous=current.get(stream)
                            if previous is not None:
                                delta=(trig-previous)&65535
                                if delta!=1:trigger_jumps.append(dict(session=session,card=card,previous=previous,current=trig,
                                    timestampNs=tick,forwardUnseenEstimate=delta-1 if 1<delta<32768 else None,
                                    classification="forward estimate" if delta<32768 else "backward/reused/reset unknown"))
                            current[stream]=trig
                            occurrences[identity]=occurrences.get(identity,0)+1
                            if occurrences[identity]>1:ambiguous_keys.add(identity)
                    elif exact_object:
                        exact=first_links.get((card,link))
                        if exact is None:unmatched_source+=1;continue
                        identity=exact[:3]
                    elif identity not in occurrences:
                        unmatched_source+=1
                        continue
                    key=identity+(occurrences[identity],)
                    if exact_object:key=exact
                    if key not in groups:
                        groups[key]=dict(session=session,card=card,trigger=trig,rawPackets=0,seen=0,
                            basePacket=None,first=None,last=None,duplicates=0,outsideAnchor=0,sourceClosed=None,
                            occurrence=occurrences[identity],sourceUnique=None,startupFiltered=False,startupBuffered=False,
                            sourceOutput=0,adapterOutput=0,shortNonTailPayloads=0,shortTailPayloads=0,sourceRejects={},outputQueueEvents={},hostDelivery={},terminationEvents={},deliverySessions=[])
                    g=groups[key]
                    if exact_object and session not in g['deliverySessions']:g['deliverySessions'].append(session)
                    if stage==1:
                        if g['basePacket'] is None:
                            g['basePacket']=pkt;g['first']=tick
                            first_links[(card,link)]=key
                        g['last']=tick;g['rawPackets']+=1;offset=(pkt-g['basePacket'])&65535
                        if offset>=expected:g['outsideAnchor']+=1
                        elif g['seen']&(1<<offset):g['duplicates']+=1
                        else:
                            g['seen']|=1<<offset
                            if offset<expected-1 and length<1444:g['shortNonTailPayloads']+=1
                            if offset==expected-1 and samples is not None:
                                needed=samples*(8 if bits==32 else 4)-offset*1440
                                if length<needed+4:g['shortTailPayloads']+=1
                    elif stage==2:
                        if reason in (6,7,8):g['sourceClosed']=reason;g['sourceUnique']=value;g['sourceClosedNs']=tick
                        elif reason in (9,18):g['startupFiltered']=True
                        elif reason==14:g['startupBuffered']=True
                        elif reason==15:g['sourceOutput']+=1
                        elif reason in (13,19,20,21,22,23,24,25,26):
                            g['terminationEvents'][str(reason)]=g['terminationEvents'].get(str(reason),0)+1
                        elif reason in (1,2,3,4,5,17):
                            name=str(reason);g['sourceRejects'][name]=g['sourceRejects'].get(name,0)+value
                    elif stage==4:g['adapterOutput']+=1
                    elif stage==5:
                        name=str(reason);g['outputQueueEvents'][name]=g['outputQueueEvents'].get(name,0)+1
                    elif stage==7:
                        host_count+=1
                        route={0:'saveConsumer',1:'displayReturn',2:'ringReturn',3:'publisherReturn',4:'exception',5:'staleAfterConversion'}.get(reason,'unknown')
                        name=route+':'+str(value);g['hostDelivery'][name]=g['hostDelivery'].get(name,0)+1
        files.append(dict(name=path.name,bytes=size,sha256=digest.hexdigest(),trailingBytes=tail))
    present=sum(int(x).bit_count() for x in identities)
    missing_records=max(issued,max_sequence)-present
    missing_ranges=[];range_start=None
    if missing_records:
        for seq in range(1,max(issued,max_sequence)+1):
            found=seq//8<len(identities) and identities[seq//8]&(1<<(seq%8))
            if not found and range_start is None:range_start=seq
            if found and range_start is not None:missing_ranges.append([range_start,seq-1]);range_start=None
        if range_start is not None:missing_ranges.append([range_start,max(issued,max_sequence)])
    incomplete=(not summary or summary.get('traceIncomplete',True) or missing_records!=0 or bad!=0 or duplicate_records!=0)
    anomalies=[];first={};partial=0;source_filtered=0;raw_complete=0;slot_complete=0;host_totals={}
    for g in groups.values():
        for name,count in g['hostDelivery'].items():host_totals[name]=host_totals.get(name,0)+count
        seen=g.pop('seen');g['unique']=seen.bit_count();g['expected']=expected
        g['missingRelativeSlots']=[i for i in range(expected) if not seen&(1<<i)]
        g['physicalSlotZero']='unknown (source uses first arrival as anchor)'
        g['associationAmbiguous']=1 in schemas and (g['session'],g['card'],g['trigger']) in ambiguous_keys
        g['tailPayloadVerified']=samples is not None
        if g['rawPackets']:
            if g['unique']==expected:slot_complete+=1
            if g['unique']==expected and not g['shortNonTailPayloads'] and not g['shortTailPayloads']:
                if samples is not None:raw_complete+=1
            else:partial+=1
            k=str(g['session'])+':'+str(g['card'])
            if k not in first or g['first']<first[k]['first']:first[k]=g
        if g['startupFiltered']:source_filtered+=1
        delivery_gap=g['sourceOutput']!=g['adapterOutput'] if not host_count else bool(
            g['hostDelivery'].get('saveConsumer:6') or g['hostDelivery'].get('exception:0') or g['hostDelivery'].get('staleAfterConversion:0') or
            any(g['outputQueueEvents'].get(str(r)) for r in (2,5,7,8,9,10,11)))
        if host_count and g['sourceClosed']==6 and not g['hostDelivery'].get('displayReturn:1'):
            g['synchronizedHostDelivery']='not observed; inspect source sync/termination events and queue results'
            delivery_gap=True
        if g['missingRelativeSlots'] or g['outsideAnchor'] or g['duplicates'] or g['shortNonTailPayloads'] or g['shortTailPayloads'] or g['startupFiltered'] or g['associationAmbiguous'] or g['sourceRejects'] or g['terminationEvents'] or delivery_gap:anomalies.append(g)
    timing=_timing(root)
    round_summaries=_round_summaries(groups,rounds,expected,cards,not incomplete)
    return dict(schemaVersion=3,inputSchemas=sorted(schemas),traceIncomplete=bool(incomplete),records=records,recordsIssued=issued or None,
        missingRecordCount=missing_records,missingRecordRanges=missing_ranges,duplicateRecordCount=duplicate_records,malformed=bad,
        fileOrderInversions=file_order_inversions,fileOrderIsNotLossEvidence=True,
        observedTriggerJumps=trigger_jumps,sourceSyncEvents=sync_events,unseenTriggerEstimateIsPhysicalTruth=False,
        rawIngress=raw_count,feedbackPortIngress=feedback_count,shortDataIngress=short_count,
        unmappedFeedbackDataIngress=unmapped_count,unmatchedSourceRecords=unmatched_source,
        rawSlotCompleteOccurrences=slot_complete,rawCompleteCardTriggers=raw_complete if samples is not None else None,
        rawOccurrenceCount=sum(g['rawPackets']>0 for g in groups.values()),
        repeatedTriggerIdentities=len(ambiguous_keys),associationComplete=not (ambiguous_keys and 1 in schemas) and not unmapped_count and not unmatched_source,
        rawPartialCardTriggers=partial,startupFilteredCardTriggers=source_filtered,
        firstObserved=list(first.values()),anomalies=anomalies,files=files,
        physicalTriggerTruth='unknown',productionHostDelivery='observed callback returns, not disk durability or completed reconstruction' if host_count else 'unknown: no stage 7 host records',
        hostDeliveryRecords=host_count,hostDeliveryTotals=host_totals,runIdentity=metadata,timing=timing,rounds=round_summaries,
        caveat='Raw occurrences split on each observed trigger change, including late/reused IDs. They are not physical trigger truth. Delayed source/adapter records for reused IDs are ambiguous; per-occurrence delivery counts must not be treated as verified. Tail payload completeness needs --samples. Feedback-port data mapping is unknown without endpoint metadata.')
def _resolve_trace(path,run_id=None):
    if (path/'run-config.json').exists():return path
    candidates=list(path.glob('paimage/*/run-config.json'))+list(path.glob('*/run-config.json'))
    if run_id:candidates=[p for p in candidates if p.parent.name==run_id]
    if len(candidates)!=1:raise ValueError(f'expected one trace run, found {len(candidates)}; use --run-id')
    return candidates[0].parent

def main():
    p=argparse.ArgumentParser();p.add_argument('trace',type=Path);p.add_argument('--packets',type=int,required=True)
    p.add_argument('--data-port',type=int,default=18001);p.add_argument('--samples',type=int)
    p.add_argument('--bits',type=int,default=32);p.add_argument('--cards',type=int,default=4)
    p.add_argument('--rounds-json',type=Path);p.add_argument('--run-id');p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    rounds=json.loads(a.rounds_json.read_text(encoding='utf-8')).get('rounds',[]) if a.rounds_json else []
    if a.trace.suffix.lower()=='.zip':
        with tempfile.TemporaryDirectory(prefix='paimage-analyze-') as d:
            with zipfile.ZipFile(a.trace) as z:z.extractall(d)
            result=analyze(_resolve_trace(Path(d),a.run_id),a.packets,a.data_port,a.samples,a.bits,a.cards,rounds)
    else:result=analyze(_resolve_trace(a.trace,a.run_id),a.packets,a.data_port,a.samples,a.bits,a.cards,rounds)
    a.output.write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps({k:v for k,v in result.items() if k not in ('firstObserved','anomalies','files','caveat','sourceSyncEvents')},ensure_ascii=False))
if __name__=='__main__':main()
