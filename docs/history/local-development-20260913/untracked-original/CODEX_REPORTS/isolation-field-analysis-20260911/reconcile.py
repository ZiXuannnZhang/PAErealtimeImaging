import json,collections,hashlib
from pathlib import Path
P=Path(__file__).parent
d=json.loads((P/"independent-groups.json").read_text(encoding="utf-8")); rounds=[]
for rid,v in d.items():
 gs=v["groups"]; n=v["config"]["samples"]; exp=(n*8+1439)//1440
 cs=sorted([g for g in gs if g["card"]==0],key=lambda g:g["first"]); rr=[]
 for g in cs:
  if not rr or g["first"]-rr[-1][-1]["first"]>2e9:rr.append([])
  rr[-1].append(g)
 for rows in rr:
  start=rows[0]["trigger"];end=rows[-1]["trigger"]; lo=rows[0]["first"]-1000000000;hi=rows[-1]["last"]+1000000000
  groups=[g for g in gs if lo<=g["first"]<=hi]; index={(g["card"],g["trigger"]):g for g in groups}
  missing=[t for t in range(start+1,end+1) if all((c,t) not in index for c in range(4))]
  bad=[t for t in range(start+1,end+1) if any((c,t) not in index or index[c,t]["unique"]!=exp for c in range(4))]
  partial=[t for t in bad if t not in missing]; lastbad=max(bad,default=start)
  r=dict(run=rid,samples=n,us=n//250,start=start,end=end,time=rows[0]["time"],lastTime=rows[-1]["time"],extraPackets=[index[c,start]["unique"] for c in range(4)],missingEffective=missing,partialEffective=partial,completeEffective=end-start-len(bad),stableStart=lastbad+1,stableCount=end-lastbad,missingPacketsObserved=sum(exp-g["unique"] for g in groups if g["trigger"]!=start),savedPerCard=[sum(g["save"] for g in groups if g["card"]==c) for c in range(4)],partialPerCard=[sum(g["unique"]<exp for g in groups if g["card"]==c and g["trigger"]!=start) for c in range(4)])
  rounds.append(r);print(json.dumps(r,ensure_ascii=False),flush=True)
(P/"rounds.json").write_text(json.dumps(rounds,indent=2),encoding="utf-8")
events=[json.loads(s) for s in (P/"evidence/events.jsonl").read_text(encoding="utf-8").splitlines()]
selected=[e for e in events if e["category"]!="imaging.bypass" and any(x in json.dumps(e,ensure_ascii=False).lower() for x in [".dat","保存","saving","build","sha256"])]
(P/"save-events.json").write_text(json.dumps(selected,ensure_ascii=False,indent=2),encoding="utf-8")
snap=[e for e in events if e["message"]=="imaging_queue_snapshot"]
print("IMAGING_MAX",{k:max(int(e["fields"].get(k,0)) for e in snap) for k in snap[-1]["fields"]},flush=True)
files=[]
for name,n in [("001",7500),("002",7500),("003",12500),("004",12500)]:
 root=Path("D:/zzx/data/实时重建")/name
 for c in range(1,5):
  for ch in "AB":
   fs=sorted(root.glob(f"Card{c}_Ch{ch}_*.dat")); total=0;ff=[]
   for f in fs:
    h=hashlib.sha256()
    with f.open("rb") as z:
     while b:=z.read(1024*1024):h.update(b)
    total+=f.stat().st_size;ff.append(dict(name=f.name,bytes=f.stat().st_size,sha256=h.hexdigest()))
   r=dict(directory=name,card=c,channel=ch,bytes=total,assumedSamples=n,frames=total//(n*2),remainder=total%(n*2),files=ff);files.append(r)
   print("DATA",{k:v for k,v in r.items() if k!="files"},flush=True)
(P/"saved-files.json").write_text(json.dumps(files,indent=2),encoding="utf-8")
