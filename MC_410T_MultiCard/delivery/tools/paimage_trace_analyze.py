"""Read schema-1/2 binary traces independently of GUI/source cumulative counters."""
import argparse, hashlib, json, struct, socket
from pathlib import Path

RECORD=struct.Struct('<QQQQIIIHHHHHhBB4sH')
def analyze(root,expected,base_port,samples=None,bits=32,cards=4):
    metadata_path=root/'run-config.json'
    metadata=json.loads(metadata_path.read_text(encoding='utf-8')) if metadata_path.exists() else {}
    if metadata:
        base_port=metadata.get('dataPort',base_port);samples=metadata.get('samples',samples)
        bits=metadata.get('bits',bits);cards=metadata.get('cards',cards)
    target_cards={int.from_bytes(socket.inet_aton(ip),'little'):c for c,ip in enumerate(metadata.get('targets',[]))}
    if expected<1 or cards<1 or cards>32 or bits not in (16,32):
        raise ValueError('invalid trace configuration')
    if samples is not None and (samples<1 or (samples*(8 if bits==32 else 4)+1439)//1440!=expected):
        raise ValueError('samples/width disagree with expected packet count')
    summary_path=root/'trace-summary.json'
    summary=json.loads(summary_path.read_text()) if summary_path.exists() else {}
    issued=summary.get('recordsIssued',0)
    identities=bytearray((issued+8)//8)
    groups={};files=[];bad=0;duplicate_records=0;records=0;max_sequence=0
    raw_count=0;feedback_count=0;short_count=0;unmapped_count=0;host_count=0
    current={};occurrences={};ambiguous_keys=set();unmatched_source=0;first_links={};schemas=set();trigger_jumps=[];sync_events=[]
    for path in sorted(root.glob('trace-*.bin'),key=lambda p:int(p.stem.split('-')[-1])):
        digest=hashlib.sha256();size=path.stat().st_size;tail=size%RECORD.size
        bad+=bool(tail)
        with path.open('rb') as f:
            while chunk:=f.read(RECORD.size*8192):
                digest.update(chunk)
                for r in RECORD.iter_unpack(chunk[:len(chunk)//64*64]):
                    seq,tick,session,link,tid,ip,value,local,source,length,trig,pkt,card,stage,reason,header,schema=r
                    records+=1;max_sequence=max(max_sequence,seq)
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
                    g=groups.setdefault(key,dict(session=session,card=card,trigger=trig,rawPackets=0,seen=0,
                        basePacket=None,first=None,last=None,duplicates=0,outsideAnchor=0,sourceClosed=None,
                        occurrence=occurrences[identity],sourceUnique=None,startupFiltered=False,startupBuffered=False,
                        sourceOutput=0,adapterOutput=0,shortNonTailPayloads=0,shortTailPayloads=0,sourceRejects={},outputQueueEvents={},hostDelivery={},terminationEvents={},deliverySessions=[]))
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
    return dict(schemaVersion=2,inputSchemas=sorted(schemas),traceIncomplete=bool(incomplete),records=records,recordsIssued=issued or None,
        missingRecordCount=missing_records,missingRecordRanges=missing_ranges,duplicateRecordCount=duplicate_records,malformed=bad,
        observedTriggerJumps=trigger_jumps,sourceSyncEvents=sync_events,unseenTriggerEstimateIsPhysicalTruth=False,
        rawIngress=raw_count,feedbackPortIngress=feedback_count,shortDataIngress=short_count,
        unmappedFeedbackDataIngress=unmapped_count,unmatchedSourceRecords=unmatched_source,
        rawSlotCompleteOccurrences=slot_complete,rawCompleteCardTriggers=raw_complete if samples is not None else None,
        rawOccurrenceCount=sum(g['rawPackets']>0 for g in groups.values()),
        repeatedTriggerIdentities=len(ambiguous_keys),associationComplete=not (ambiguous_keys and 1 in schemas) and not unmapped_count and not unmatched_source,
        rawPartialCardTriggers=partial,startupFilteredCardTriggers=source_filtered,
        firstObserved=list(first.values()),anomalies=anomalies,files=files,
        physicalTriggerTruth='unknown',productionHostDelivery='observed callback returns, not disk durability or completed reconstruction' if host_count else 'unknown: no stage 7 host records',
        hostDeliveryRecords=host_count,hostDeliveryTotals=host_totals,runIdentity=metadata,
        caveat='Raw occurrences split on each observed trigger change, including late/reused IDs. They are not physical trigger truth. Delayed source/adapter records for reused IDs are ambiguous; per-occurrence delivery counts must not be treated as verified. Tail payload completeness needs --samples. Feedback-port data mapping is unknown without endpoint metadata.')
def main():
    p=argparse.ArgumentParser();p.add_argument('trace',type=Path);p.add_argument('--packets',type=int,required=True)
    p.add_argument('--data-port',type=int,default=18001);p.add_argument('--samples',type=int)
    p.add_argument('--bits',type=int,default=32);p.add_argument('--cards',type=int,default=4)
    p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    result=analyze(a.trace,a.packets,a.data_port,a.samples,a.bits,a.cards);a.output.write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps({k:v for k,v in result.items() if k not in ('firstObserved','anomalies','files','caveat')},ensure_ascii=False))
if __name__=='__main__':main()
