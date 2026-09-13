import json
from pathlib import Path
from collections import Counter, defaultdict

ROOT=Path(__file__).resolve().parent
EV=ROOT/'evidence'
def read(name):
    out=[]
    for line_no,line in enumerate((EV/name).read_text(encoding='utf-8-sig').splitlines(),1):
        if line.strip():
            obj=json.loads(line);obj['_line']=line_no;out.append(obj)
    return out
events=read('events.jsonl')
cards=read('card_history.jsonl')
settings=read('settings_history.jsonl')
network=read('network_history.jsonl')
summary={
    'programs':[x for x in settings if x.get('data',{}).get('kind')=='program'],
    'setting_kinds':dict(Counter(x.get('data',{}).get('kind') for x in settings)),
    'event_categories':dict(Counter(x.get('category') for x in events)),
    'event_messages':Counter(x.get('message') for x in events if x.get('category')!='runtime').most_common(70),
    'card_keys':dict(Counter(k for x in cards for k in x)),
    'network_kinds':dict(Counter(x.get('data',{}).get('kind') for x in network)),
    'runs':{},
}
for run in sorted({x['runId'] for x in events}):
    e=[x for x in events if x['runId']==run]
    c=[x for x in cards if x['runId']==run]
    summary['runs'][run]={'events':len(e),'first':e[0]['timestamp'],'last':e[-1]['timestamp'],'cards':len(c),'sessions':sorted({x.get('measurementSessionId','') for x in c})}
(ROOT/'inventory.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2),encoding='utf-8')
def slim(x):
    return {k:v for k,v in x.items() if k not in ("sourceRunDirectory","sourceRunId","windowRole")}
out=[]
out.append("SETTINGS")
out.extend(json.dumps(slim(x),ensure_ascii=False) for x in settings if x.get("data",{}).get("kind")!="program")
out.append("MEASUREMENT EVENTS")
out.extend(json.dumps(slim(x),ensure_ascii=False) for x in events if x.get("category") in ("measurement.session","network.measure","ui.action"))
out.append("CARD SAMPLE")
out.extend(json.dumps(slim(x),ensure_ascii=False) for x in cards if x.get("packetsDropped",0)>0 and x.get("cardIndex")==1)
(ROOT/"detail.txt").write_text("\n".join(out),encoding="utf-8")
print("\n".join(out[:27]))
