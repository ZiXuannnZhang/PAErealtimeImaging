"""Read schema-1/2 binary traces independently of GUI/source cumulative counters."""
import argparse, hashlib, heapq, json, struct, socket, tempfile, zipfile
from pathlib import Path

RECORD=struct.Struct('<QQQQIIIHHHHHhBB4sH')
TIMING=struct.Struct('<QQQQQIIIHhHHI')
LOOPLOG=struct.Struct('<QQQQIHHIIIIQQQ')
LOOP_KINDS={1:'loop',2:'drain',3:'recvFailure',4:'socketSetup',5:'threadStart',6:'burstMark',7:'stalled',8:'controlMark'}

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
            session=spec.get('session'),startNs=spec.get('startNs'),endNs=spec.get('endNs'),
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

def _looplog(root):
    summary_path=root/'looplog-summary.json'
    summary=json.loads(summary_path.read_text(encoding='utf-8')) if summary_path.exists() else {}
    records=[];bad=0;unique=0;maximum=0;by_kind={}
    for path in sorted(root.glob('looplog-*.bin'),key=lambda p:int(p.stem.split('-')[-1])):
        data=path.read_bytes();bad+=len(data)%80!=0
        for values in LOOPLOG.iter_unpack(data[:len(data)//80*80]):
            seq,tick,span,session,tid,kind,flags,v0,v1,v2,v3,pa,pb,pc=values
            if not seq:bad+=1;continue
            maximum=max(maximum,seq);unique+=1
            rec=dict(sequence=seq,timeNs=tick,spanNs=span,session=session,threadId=tid,
                     kind=LOOP_KINDS.get(kind,str(kind)),flags=flags,value0=v0,value1=v1,
                     value2=v2,value3=v3,payloadA=pa,payloadB=pb,payloadC=pc)
            by_kind[rec['kind']]=by_kind.get(rec['kind'],0)+1
            records.append(rec)
    issued=int(summary.get('recordsIssued',0));missing=max(issued,maximum)-unique
    loops=[r for r in records if r['kind']=='loop']
    gaps=sorted(r['spanNs'] for r in loops)
    bursts=[dict(epoch=r['value0']|(r['value1']<<32),burstNs=r['timeNs'],idleSpanNs=r['spanNs'],
                  previousSampleNs=r['payloadA']) for r in records if r['kind']=='burstMark']
    first_ns=min((r['timeNs'] for r in records),default=None)
    last_ns=max((r['timeNs'] for r in records),default=None)
    burst_windows=[]
    for burst in bursts:
        start=burst['burstNs']-5_000_000_000;end=burst['burstNs']+5_000_000_000
        burst_windows.append(dict(epoch=burst['epoch'],startNs=start,endNs=end,
            records=sum(start<=r['timeNs']<=end for r in records),
            prior5sCovered=first_ns is not None and first_ns<=start,
            following5sCovered=last_ns is not None and last_ns>=end))
    stalled=[dict(detectedNs=r['timeNs'],stalledNs=r['spanNs'],lastLoopNs=r['payloadA'])
             for r in records if r['kind']=='stalled']
    control=[dict(phase=r['value0'],success=r['value1'],session=r['session'],enterNs=r['timeNs'],
                  durationNs=r['spanNs'],enabledAfter=bool(r['flags']&1),confirmedAfter=bool(r['flags']&2))
             for r in records if r['kind']=='controlMark']
    return dict(status='not-recorded' if not summary and not records else 'ok',summary=summary,
        records=unique,missingRecordCount=max(0,missing),malformed=bad,recordsByKind=by_kind,
        loopCount=len(loops),maxLoopGapNs=gaps[-1] if gaps else None,
        loopGapsOver20ms=sum(1 for g in gaps if g>20000000),
        loopGapsOver100ms=sum(1 for g in gaps if g>100000000),
        receiveBursts=bursts,burstWindows=burst_windows,stallHints=stalled,controlMarks=control,
        loopLogIncomplete=bool(not summary or summary.get('loopLogIncomplete',True) or missing or bad),
        note='loop records are full receive-loop coverage; 100ms stall sampling cannot observe every 20ms gap and does not prove CPU preemption')

def _pcapng_packets(path):
    """Parse pktmon etl2pcap pcapng (Ethernet or raw-IP link types).
    Truncated packet bytes are kept; only IP/UDP headers and the first four
    protocol bytes are used. Repeated observation of one datagram by
    multiple components is collapsed by (addresses, ports, ids, time)."""
    data=path.read_bytes();at=0;interfaces=[]
    packets=[]
    while at+12<=len(data):
        block_type,block_len=struct.unpack_from('<II',data,at)
        if block_len<12 or at+block_len>len(data):break
        if block_type==1 and block_len>=20:
            linktype=struct.unpack_from('<H',data,at+8)[0]
            seconds_per_tick=1e-6
            option_at=at+16
            while option_at+4<=at+block_len-4:
                code,olen=struct.unpack_from('<HH',data,option_at)
                if code==0:break
                value_at=option_at+4
                if value_at+olen>at+block_len-4:break
                if code==9 and olen>=1:
                    resolution=data[value_at]
                    seconds_per_tick=(2.0**-(resolution&0x7f)) if resolution&0x80 else (10.0**-resolution)
                option_at=value_at+((olen+3)//4)*4
            interfaces.append((linktype,seconds_per_tick))
        elif block_type==0x00000006 and block_len>=32:  # enhanced packet block
            interface,ts_high,ts_low,cap_len,orig_len=struct.unpack_from('<IIIII',data,at+8)
            packet=data[at+28:at+28+cap_len]
            ts=(ts_high<<32)|ts_low
            if interface>=len(interfaces):
                at+=block_len;continue
            linktype,seconds_per_tick=interfaces[interface]
            ts_ms=ts*seconds_per_tick*1000.0
            offset=0
            if linktype==1:
                if len(packet)<14:at+=block_len;continue
                offset=14
                ether_type=struct.unpack_from('!H',packet,12)[0]
                while ether_type in (0x8100,0x88a8):
                    if len(packet)<offset+4:break
                    ether_type=struct.unpack_from('!H',packet,offset+2)[0];offset+=4
                if ether_type!=0x0800:at+=block_len;continue
            elif linktype in (0,101):
                offset=0
            else:
                at+=block_len;continue
            if len(packet)<offset+20 or packet[offset]>>4!=4:at+=block_len;continue
            ihl=(packet[offset]&15)*4
            if len(packet)<offset+ihl+8:at+=block_len;continue
            protocol=packet[offset+9]
            if protocol!=17:at+=block_len;continue
            src='.'.join(str(b) for b in packet[offset+12:offset+16])
            dst='.'.join(str(b) for b in packet[offset+16:offset+20])
            udp=offset+ihl
            sport,dport,ulen=struct.unpack_from('!HHH',packet,udp)
            head=packet[udp+8:udp+12]
            seq,trigger=struct.unpack('<HH',head) if len(head)>=4 else (None,None)
            packets.append(dict(src=src,dst=dst,sport=sport,dport=dport,
                trigger=trigger,packet=seq,tsMs=ts_ms,bytes=len(packet)))
        at+=block_len
    dedup=[];last_by_key={}
    for p in sorted(packets,key=lambda item:item['tsMs']):
        key=(p['src'],p['dst'],p['sport'],p['dport'],p['trigger'],p['packet'])
        previous=last_by_key.get(key)
        if previous is None or p['tsMs']-previous>2.0:
            dedup.append(p);last_by_key[key]=p['tsMs']
        # One datagram observed by several components is one packet; the
        # first observation wins and near-in-time repeats are ignored.
    return dedup

def _system_capture(capture_dir,groups,rounds,base_port,cards,wall_anchor_ms,mono_anchor_ns,anchor_known):
    manifest_path=capture_dir/'system-capture-manifest.json'
    if not manifest_path.exists():
        return dict(status='not-found',note='no system-capture manifest at the given directory')
    manifest=json.loads(manifest_path.read_text(encoding='utf-8'))
    result=dict(status=manifest.get('status'),trialId=manifest.get('trialId'),
        applicationRunId=manifest.get('applicationRunId'),stoppedBy=manifest.get('stoppedBy'),
        manifestFiles=manifest.get('files'),commands=[
            {k:c.get(k) for k in ('description','exitCode','startUtc','endUtc')}
            for c in manifest.get('commands',[])])
    if manifest.get('status')=='failed' or not any(f.get('kind')=='pktmon-pcapng' for f in manifest.get('files',[])):
        result['classification']='unverifiable'
        result['note']='system capture failed or converted pcapng missing; no layer statement is allowed'
        return result
    pcap=capture_dir/'startup-pktmon.pcapng'
    packets=_pcapng_packets(pcap)
    result['systemPacketsDeduplicated']=len(packets)
    sys_index={}
    for p in packets:
        sys_index.setdefault((p['src'],p['dport'],p['trigger']),[]).append(p)
    app_index={}
    for g in groups.values():
        if not g.get('rawPackets'):continue
        key=(g.get('sourceIp'),base_port+g['card'],g['trigger'])
        app_index.setdefault(key,[]).append(g)
    def utc_ms(mono_ns):
        return wall_anchor_ms+(mono_ns-mono_anchor_ns)/1000000.0
    window_ms=100.0
    def system_packets(source_ip,port,trigger,center_ms=None,custom_window_ms=None):
        found=[]
        for p in sys_index.get((source_ip,port,trigger),[]):
            if center_ms is None or abs(p['tsMs']-center_ms)<=(custom_window_ms or window_ms):found.append(p)
        return found
    correlation=dict(matchedAppTriggers=0,layerGapCandidates=[],unknownCandidates=[],
        appSideDiscards=[],lateDeliveries=[],unverifiableTriggers=[])
    if not anchor_known:
        correlation['note']='application wall anchor unknown; trigger-only matching across rounds is not attempted'
    source_by_card={}
    for (source_ip,port,trigger),candidates in app_index.items():
        if source_ip:source_by_card.setdefault(port-base_port,source_ip)
        for g in candidates:
            if g.get('unique',0)==g.get('expected',0):continue
            base=g.get('basePacket')
            if base is None:continue
            center=(g['first']+g['last'])/2
            center_ms=utc_ms(center) if anchor_known else None
            sys_here=system_packets(source_ip,port,trigger,center_ms)
            if not sys_here:
                correlation['unverifiableTriggers'].append(dict(card=g['card'],trigger=trigger,
                    reason='no system observation in window; layer statement unavailable'))
                continue
            correlation['matchedAppTriggers']+=1
            sys_slots={(p['packet']-base)&0xFFFF for p in sys_here}
            missing=[s for s in g.get('missingRelativeSlots',[]) if s in sys_slots]
            if missing:
                correlation['layerGapCandidates'].append(dict(card=g['card'],trigger=trigger,
                    missingSlotsPresentInSystem=missing,
                    conclusion='missing between the observable system layer and application ingress'))
            unknown_missing=[s for s in g.get('missingRelativeSlots',[]) if s not in sys_slots]
            if unknown_missing:
                correlation['unknownCandidates'].append(dict(card=g['card'],trigger=trigger,
                    missingSlotsAbsentFromSystem=unknown_missing,
                    conclusion='not present at the earliest observable layer either; cause unknown'))
            if g.get('sourceRejects') or g.get('terminationEvents'):
                correlation['appSideDiscards'].append(dict(card=g['card'],trigger=trigger,
                    sourceRejects=g.get('sourceRejects'),terminationEvents=g.get('terminationEvents'),
                    conclusion='application ingress exists and a later reject or cleanup event is explicit'))
            if anchor_known:
                sys_first=min(p['tsMs'] for p in sys_here)
                lag_ms=utc_ms(g['first'])-sys_first
                if lag_ms>100:
                    correlation['lateDeliveries'].append(dict(card=g['card'],trigger=trigger,
                        lagMs=lag_ms,conclusion='delivery/receive delay present; layer not determined by time difference alone'))
    for spec in rounds or []:
        for per_card in spec.get('perCard',[]):
            card=per_card['card'];port=base_port+card
            for trigger in per_card.get('missingEffectiveTriggers',[]):
                source_ip=source_by_card.get(card)
                app_here=bool(app_index.get((source_ip,port,trigger)))
                if app_here:continue
                if not anchor_known or spec.get('startNs') is None or spec.get('endNs') is None or not source_ip:
                    correlation['unverifiableTriggers'].append(dict(card=card,trigger=trigger,wholeTrigger=True,
                        reason='whole-trigger classification needs a source address, clock anchor and explicit round time window'))
                    continue
                start_ms=utc_ms(int(spec['startNs']));end_ms=utc_ms(int(spec['endNs']))
                center_ms=(start_ms+end_ms)/2
                round_window=max(window_ms,(end_ms-start_ms)/2+window_ms)
                sys_here=system_packets(source_ip,port,trigger,center_ms,round_window)
                if sys_here:
                    correlation['layerGapCandidates'].append(dict(card=card,trigger=trigger,
                        wholeTrigger=True,
                        conclusion='whole trigger unseen by application but present at the observable system layer; missing between that layer and application ingress'))
                else:
                    correlation['unknownCandidates'].append(dict(card=card,trigger=trigger,
                        wholeTrigger=True,
                        conclusion='not present at the earliest observable layer either; cause unknown, not proof of card/NIC/cable failure'))
    correlation['layerGapCandidates']=correlation['layerGapCandidates'][:2000]
    correlation['unknownCandidates']=correlation['unknownCandidates'][:2000]
    correlation['appSideDiscards']=correlation['appSideDiscards'][:2000]
    correlation['lateDeliveries']=correlation['lateDeliveries'][:2000]
    correlation['unverifiableTriggers']=correlation['unverifiableTriggers'][:2000]
    result['correlation']=correlation
    result['alignmentWindowMs']=window_ms
    result['note']=('classification uses the evidence table: application-side discard, '
        'gap between observable system layer and application ingress, delivery delay, '
        'unknown below the earliest observable layer, or unverifiable window')
    return result

def analyze(root,expected,base_port,samples=None,bits=32,cards=4,rounds=None,system_capture_dir=None):
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
                    exact_object=schema==2 and (stage in (4,5,7) or (stage==2 and reason in (6,7,8,9,13,14,15,18,19,20,21,22,23,24,25,26,27,28)))
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
                            basePacket=None,sourceIp=None,first=None,last=None,duplicates=0,outsideAnchor=0,sourceClosed=None,
                            occurrence=occurrences[identity],sourceUnique=None,startupFiltered=False,startupBuffered=False,
                            sourceOutput=0,adapterOutput=0,shortNonTailPayloads=0,shortTailPayloads=0,sourceRejects={},outputQueueEvents={},hostDelivery={},terminationEvents={},deliverySessions=[])
                    g=groups[key]
                    if exact_object and session not in g['deliverySessions']:g['deliverySessions'].append(session)
                    if stage==1:
                        if g['basePacket'] is None:
                            g['basePacket']=pkt;g['sourceIp']=socket.inet_ntoa(int(ip).to_bytes(4,'little'));g['first']=tick
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
                        elif reason in (13,19,20,21,22,23,24,25,26,27,28):
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
    looplog=_looplog(root)
    round_summaries=_round_summaries(groups,rounds,expected,cards,not incomplete)
    system=None
    if system_capture_dir is not None:
        wall=metadata.get('wallAnchorMs');mono_raw=metadata.get('monotonicAnchorNs')
        mono=int(mono_raw) if isinstance(mono_raw,(int,float)) else int(str(mono_raw)) if mono_raw else 0
        system=_system_capture(Path(system_capture_dir),groups,round_summaries,base_port,cards,
            float(wall) if wall else 0.0,mono,bool(wall and mono))
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
        hostDeliveryRecords=host_count,hostDeliveryTotals=host_totals,runIdentity=metadata,
        timing=timing,loopLog=looplog,systemCapture=system,rounds=round_summaries,
        caveat='Raw occurrences split on each observed trigger change, including late/reused IDs. They are not physical trigger truth. Delayed source/adapter records for reused IDs are ambiguous; per-occurrence delivery counts must not be treated as verified. Tail payload completeness needs --samples. Feedback-port data mapping is unknown without endpoint metadata.')
def _resolve_trace(path,run_id=None):
    if (path/'run-config.json').exists() or (path/'trace-summary.json').exists():return path
    candidates=list(path.glob('paimage/*/run-config.json'))+list(path.glob('*/run-config.json'))
    if run_id:candidates=[p for p in candidates if p.parent.name==run_id]
    if len(candidates)!=1:raise ValueError(f'expected one trace run, found {len(candidates)}; use --run-id')
    return candidates[0].parent

def main():
    p=argparse.ArgumentParser();p.add_argument('trace',type=Path);p.add_argument('--packets',type=int,required=True)
    p.add_argument('--data-port',type=int,default=18001);p.add_argument('--samples',type=int)
    p.add_argument('--bits',type=int,default=32);p.add_argument('--cards',type=int,default=4)
    p.add_argument('--rounds-json',type=Path);p.add_argument('--run-id');
    p.add_argument('--system-capture',type=Path);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    rounds=json.loads(a.rounds_json.read_text(encoding='utf-8')).get('rounds',[]) if a.rounds_json else []
    if a.trace.suffix.lower()=='.zip':
        with tempfile.TemporaryDirectory(prefix='paimage-analyze-') as d:
            with zipfile.ZipFile(a.trace) as z:z.extractall(d)
            result=analyze(_resolve_trace(Path(d),a.run_id),a.packets,a.data_port,a.samples,a.bits,a.cards,rounds,a.system_capture)
    else:result=analyze(_resolve_trace(a.trace,a.run_id),a.packets,a.data_port,a.samples,a.bits,a.cards,rounds,a.system_capture)
    a.output.write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps({k:v for k,v in result.items() if k not in ('firstObserved','anomalies','files','caveat','sourceSyncEvents')},ensure_ascii=False))
if __name__=='__main__':main()
