import json,struct,collections,datetime,statistics,hashlib
from pathlib import Path
P=Path(__file__).parent;S=struct.Struct("<QQQQIIIHHHHHhBB4sH");out={}
for cfg in sorted((P/"evidence/paimage").glob("*/run-config.json")):
 c=json.loads(cfg.read_text(encoding="utf-8"));expected=(c["samples"]*8+1439)//1440;groups={};links={};counts=collections.Counter()
 def wall(t):return datetime.datetime.fromtimestamp((c["wallAnchorMs"]+(t-int(c["monotonicAnchorNs"]))/1e6)/1000,datetime.timezone(datetime.timedelta(hours=8))).isoformat(timespec="milliseconds")
 for f in sorted(cfg.parent.glob("trace-*.bin")):
  with f.open("rb") as z:
   while b:=z.read(64*8192):
    for r in S.iter_unpack(b):
     seq,t,s,link,tid,ip,val,port,sp,n,tr,pk,card,stage,reason,head,ver=r;counts[(stage,reason,val if stage==7 else 0)]+=1
     if stage==1 and 8001<=port<=8004 and n>64:
      card=port-8001;pk,tr=struct.unpack("<HH",head);key=(s,card,tr);g=groups.setdefault(key,dict(session=s,card=card,trigger=tr,first=t,last=t,packets=set(),raw=0,save=0,host=0,base=pk));g["packets"].add(pk);g["raw"]+=1;g["last"]=t;links[link]=key
     elif stage==7 and link in links:
      g=groups[links[link]]
      if reason==0 and val==5:g["save"]+=1
      if reason==1 and val==1:g["host"]+=1
 gs=list(groups.values())
 for g in gs:
  pk=g.pop("packets");g["unique"]=len(pk);g["missing"]=[i for i in range(expected) if i not in pk];g["time"]=wall(g["first"])
 out[cfg.parent.name]=dict(config=c,groups=gs,counts={str(k):v for k,v in counts.items()})
 print("RUN",cfg.parent.name,"expected",expected,"counts",out[cfg.parent.name]["counts"],flush=True)
 if not gs:continue
 cs=sorted([g for g in gs if g["card"]==0],key=lambda g:g["first"]);rounds=[]
 for g in cs:
  if not rounds or g["first"]-rounds[-1][-1]["first"]>2e9:rounds.append([])
  rounds[-1].append(g)
 for rr in rounds:
  ids={g["trigger"] for g in rr};allg=[g for g in gs if g["trigger"] in ids];print("SEG",rr[0]["trigger"],rr[-1]["trigger"],len(rr),wall(rr[0]["first"]),wall(rr[-1]["last"]),"saved",sum(g["save"] for g in rr),"partial",[sum(g["unique"]<expected for g in allg if g["card"]==i) for i in range(4)],"missing",[sum(expected-g["unique"] for g in allg if g["card"]==i) for i in range(4)],"firstunique",[next(g["unique"] for g in allg if g["card"]==i and g["trigger"]==rr[0]["trigger"]) for i in range(4)],flush=True)
  ds=[(b["first"]-a["first"])/1e6 for a,b in zip(rr,rr[1:]) if b["trigger"]==a["trigger"]+1];print("medianms",statistics.median(ds) if ds else None,"badcard1",[(g["trigger"],g["unique"],g["time"]) for g in rr if g["unique"]<expected][:12],flush=True)
 print("bases",collections.Counter(g["base"] for g in gs),flush=True)
(P/"independent-groups.json").write_text(json.dumps(out,ensure_ascii=False),encoding="utf-8")
