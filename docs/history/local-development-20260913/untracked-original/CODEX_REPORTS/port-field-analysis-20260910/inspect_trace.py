import json,struct,hashlib,collections,datetime
from pathlib import Path
ROOT=Path(__file__).parent
E=ROOT/"evidence"
R=E/"paimage/d1b49ca2-0dda-4c3c-b90f-bcd48170f264"
cfg=json.loads((R/"run-config.json").read_text())
def wall(t):
 return datetime.datetime.fromtimestamp((cfg["wallAnchorMs"]+(t-int(cfg["monotonicAnchorNs"]))/1e6)/1000,datetime.timezone(datetime.timedelta(hours=8))).isoformat(timespec="microseconds")
S=struct.Struct("<QQQQIIIHHHHHhBB4sH")
groups={};counts=collections.Counter();events=[];last=0;seqbad=[];raw=collections.Counter();lens=collections.Counter();links={};unknown=[]
for p in sorted(R.glob("trace-*.bin")):
 with p.open("rb") as f:
  while b:=f.read(64*8192):
   for r in S.iter_unpack(b):
    seq,t,s,link,tid,ip,val,port,sp,n,tr,pk,c,stage,reason,head,ver=r
    if seq!=last+1:seqbad.append([last,seq])
    last=seq;counts[(stage,reason)]+=1
    if stage==1:
     raw[(port,n)]+=1
     if n<=64:continue
     c=port-8001 if 8001<=port<=8004 else (ip>>24)-2
     pk,tr=struct.unpack("<HH",head);key=(s,c,tr)
     g=groups.setdefault(key,dict(session=s,card=c,trigger=tr,packets=[],lengths=[],first=t,last=t,events=[]))
     g["packets"].append(pk);g["lengths"].append(n);g["last"]=t;links[link]=key
    elif stage in (2,5,7) and c>=0:
     key=links.get(link)
     ev=[stage,reason,val,tr,wall(t),link]
     if key in groups:groups[key]["events"].append(ev)
     else:unknown.append(ev)
    elif stage in (2,3,6):events.append([stage,reason,val,tr,c,wall(t),link,pk])
out=[]
for g in groups.values():
 ps=g.pop("packets");ls=g.pop("lengths");g["rawCount"]=len(ps);g["unique"]=len(set(ps));g["packetMin"]=min(ps);g["packetMax"]=max(ps);g["base"]=ps[0];g["lengths"]=dict(collections.Counter(ls));g["missingRelative"]=[i for i in range(35) if (ps[0]+i)%65536 not in ps];g["first"]=wall(g["first"]);g["last"]=wall(g["last"]);g["eventCounts"]=dict(collections.Counter(str(e[:3]) for e in g["events"]))
 if g["unique"]!=35:g["packets"]=ps
 out.append(g)
hashes=[]
for x in json.loads((E/"paimage/manifest.json").read_text())["files"]:
 hashes.append([x["path"],hashlib.sha256((E/x["path"]).read_bytes()).hexdigest()==x["sha256"]])
result=dict(records=last,sequenceDiscontinuities=seqbad,hashes=hashes,counts={str(k):v for k,v in counts.items()},raw={str(k):v for k,v in raw.items()},groups=out,events=events,unknown=unknown)
(ROOT/"independent-analysis.json").write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding="utf-8")
print("records",last,"seqbad",len(seqbad),"hashes",all(x[1] for x in hashes),"groups",len(out),"unknown",len(unknown));print(result["counts"]);print(result["raw"])
for c in range(4):
 gs=[g for g in out if g["card"]==c];bad=[g for g in gs if g["unique"]!=35];print("CARD",c,"count",len(gs),"range",min(g["trigger"] for g in gs),max(g["trigger"] for g in gs),"bad",len(bad));print(json.dumps([{k:v for k,v in g.items() if k!="events"} for g in bad],ensure_ascii=False))
print("CONTROL/SYNC",json.dumps(events[:35],ensure_ascii=False),"last",events[-10:])
