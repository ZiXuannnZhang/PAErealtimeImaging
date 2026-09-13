import json,re
from pathlib import Path
from collections import defaultdict,Counter
ROOT=Path(__file__).resolve().parent
def read(name):
    rows=[]
    for n,s in enumerate((ROOT/'evidence'/name).read_text(encoding='utf-8-sig').splitlines(),1):
        if not s.strip(): continue
        x=json.loads(s); x['_line']=n
        if x.get('runId','').startswith('4ba55') and x.get('windowRole')=='formal': rows.append(x)
    return rows
cards=read('card_history.jsonl'); net=read('network_history.jsonl'); events=read('events.jsonl')
keys='packetsDropped socketPacketsReceived processorPacketsDequeued packetsReceived triggersComplete triggersPartial triggersDiscarded sessionBoundaryPacketsDiscarded staleTriggerPacketsDiscarded assemblyDuplicatePackets assemblyOffsetOutOfRangePackets sameTriggerForwardGapEvents sameTriggerForwardGapPackets sameTriggerBackstepEvents sameTriggerDuplicateSeqEvents crossTriggerLateArrivalEvents batchBoundaryDiscards saveQueueDiscards'.split()
def snap(x): return {k:x.get(k) for k in ['_line','timestamp','measurementSessionId','lastTriggerSeq','lastPacketSeq']+keys}
out={'sessions':{},'network':{},'partials':{},'drop_intervals':[]}
for session in ['measurement-1','measurement-2']:
    out['sessions'][session]={}
    for card in range(1,5):
        rows=[x for x in cards if x.get('cardIndex')==card and x.get('measurementSessionId')==session and 'packetsDropped' in x]
        a,b=rows[0],rows[-1]
        out['sessions'][session][card]={'first':snap(a),'last':snap(b),'delta':{k:b.get(k,0)-a.get(k,0) for k in keys},'maxQueues':{k:max(x.get(k,0) for x in rows) for k in ['inputQueueDepth','saveQueueDepth']}}
        for prev,cur in zip(rows,rows[1:]):
            if cur['packetsDropped']>prev['packetsDropped']:
                out['drop_intervals'].append({'card':card,'session':session,'from':prev['timestamp'],'to':cur['timestamp'],'line':cur['_line'],'delta':{k:cur.get(k,0)-prev.get(k,0) for k in keys},'lastTriggerSeq':cur.get('lastTriggerSeq')})
    nr=[x for x in net if x.get('data',{}).get('kind')=='runtime_ingress' and x['data'].get('measurementSessionId')==session]
    a,b=nr[0],nr[-1]
    counters={}
    for scope in ['systemUdpCounters','systemIpv4Counters']:
        counters[scope]={k:{'first':v.get('absolute'),'last':b['data'][scope][k].get('absolute'),'difference':int(b['data'][scope][k]['absolute'])-int(v['absolute'])} for k,v in a['data'][scope].items() if isinstance(v,dict)}
    interfaces=[]
    for iface in a['data']['relevantInterfaces']:
        end=next(x for x in b['data']['relevantInterfaces'] if x['interfaceIndex']==iface['interfaceIndex'])
        interfaces.append({'name':iface['description'],'speed':end['receiveLinkSpeed'],'counters':{k:int(end['counters'][k]['absolute'])-int(v['absolute']) for k,v in iface['counters'].items()}})
    out['network'][session]={'first_line':a['_line'],'last_line':b['_line'],'first_time':a['timestamp'],'last_time':b['timestamp'],'counters':counters,'interfaces':interfaces,'receiver_first':a['data']['receiverGroups'],'receiver_last':b['data']['receiverGroups']}
    part=[]
    for x in events:
        m=re.search(r'卡([0-9]+) 部分触发 seq=([0-9]+) 缺([0-9]+)包',x.get('message',''))
        if m and (x['timestamp']<'2026-09-08T23:38:31')==(session=='measurement-1'):
            part.append({'line':x['_line'],'timestamp':x['timestamp'],'card':int(m[1]),'trigger':int(m[2]),'missing':int(m[3])})
    out['partials'][session]={'byCard':{c:{'events':sum(x['card']==c for x in part),'sum_missing':sum(x['missing'] for x in part if x['card']==c)} for c in range(1,5)},'first':part[:20],'last':part[-4:]}
    for c in range(1,5):
        partial=out['partials'][session]['byCard'][c]
        inferred=out['sessions'][session][c]['delta']['packetsDropped']-partial['sum_missing']
        expected=28 if session=='measurement-1' else 70
        assert inferred>=0 and inferred%expected==0
        assert partial['events']==out['sessions'][session][c]['delta']['triggersPartial']
        partial['whole_trigger_inferred_packets']=inferred
        partial['whole_trigger_inferred_count']=inferred//expected
    (ROOT/(session+'-partials.json')).write_text(json.dumps(part,ensure_ascii=False,indent=2),encoding='utf-8')
(ROOT/'counters.json').write_text(json.dumps(out,ensure_ascii=False,indent=2),encoding='utf-8')
print(json.dumps({k:v for k,v in out.items() if k!='drop_intervals'},ensure_ascii=False,indent=2))
