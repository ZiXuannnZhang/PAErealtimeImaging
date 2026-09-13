import json,struct,collections,hashlib,datetime
from pathlib import Path
P=Path(__file__).parent;E=P/"evidence";d=json.loads((P/"independent-groups.json").read_text());rid,v=next(iter(d.items()));cfg=v["config"];n=cfg["samples"];exp=(n*8+1439)//1440;root=E/"paimage"/rid
def wall(t):return datetime.datetime.fromtimestamp((cfg["wallAnchorMs"]+(t-int(cfg["monotonicAnchorNs"]))/1e6)/1000,datetime.timezone(datetime.timedelta(hours=8))).isoformat(timespec="milliseconds")
gs=v["groups"];cs=sorted([g for g in gs if g["card"]==0],key=lambda g:g["first"]);rr=[]
for g in cs:
 if not rr or g["first"]-rr[-1][-1]["first"]>2e9:rr.append([])
 rr[-1].append(g)
rounds=[]
for rows in rr:
 start=rows[0]["trigger"];end=rows[-1]["trigger"];idx={(g["card"],g["trigger"]):g for g in gs if start<=g["trigger"]<=end};missing=[t for t in range(start,end+1) if all((c,t) not in idx for c in range(4))];bad=[t for t in range(start,end+1) if any((c,t) not in idx or idx[c,t]["unique"]!=exp for c in range(4))];lastbad=max(bad,default=start-1)
 r=dict(start=start,end=end,firstNs=rows[0]["first"],endNs=rows[-1]["last"],time=rows[0]["time"],endTime=rows[-1]["time"],observedPerCard=[sum(c==g["card"] for g in idx.values()) for c in range(4)],completeAll=end-start+1-len(bad),completeIfFirstExtra=sum(t!=start and t not in bad for t in range(start,end+1)),missing=missing,partial=[t for t in bad if t not in missing],firstPackets=[idx[c,start]["unique"] for c in range(4)],stableStart=lastbad+1,stableCount=end-lastbad,savedPerCard=[sum(g["save"] for g in idx.values() if g["card"]==c) for c in range(4)],firstArrivals=[dict(trigger=g["trigger"],relativeMs=(g["first"]-rows[0]["first"])/1e6) for g in rows[:4]])
 rounds.append(r);print("ROUND",json.dumps(r),flush=True)
(P/"rounds.json").write_text(json.dumps(rounds,indent=2))
L=struct.Struct("<QQQQIHHIIIIQQQ");counts=collections.Counter();seen=set();windows=[collections.defaultdict(list) for r in rounds];first=10**30;last=0;controls=[];setups=[];globalmax=0
for f in sorted(root.glob("looplog-*.bin")):
 with f.open("rb") as z:
  while b:=z.read(80*8192):
   for x in L.iter_unpack(b):
    seq,t,span,s,tid,k,flags,a,bv,c,dv,p,q,u=x;seen.add(seq);counts[k]+=1;first=min(first,t);last=max(last,t)
    if k==1:globalmax=max(globalmax,span)
    if k==8:controls.append(dict(time=wall(t),phase=a,durationNs=span,flags=flags,session=s))
    if k in (4,5):setups.append(x)
    for i,r in enumerate(rounds):
     if r["firstNs"]-5e9<=t<=r["firstNs"]+5e9:
      if k in (1,2,3,7):windows[i][k].append((t,span,a,bv,c,dv,p,q,u))
summary=dict(records=sum(counts.values()),first=wall(first),last=wall(last),minSeq=min(seen),maxSeq=max(seen),missingWithin=max(seen)-min(seen)+1-len(seen),globalMaxLoopMs=globalmax/1e6,controls=controls,setups=setups,windows=[])
for r,w in zip(rounds,windows):
 loops=w[1];between=[x for x in loops if r["firstNs"]<=x[0]<=r["firstNs"]+r["firstArrivals"][1]["relativeMs"]*1e6];item=dict(start=r["start"],loopCount=len(loops),maxLoopMs=max(x[1] for x in loops)/1e6,maxDrainMs=max(x[1] for x in w[2])/1e6,stallHints=len(w[7]),recvErrors=dict(collections.Counter(x[3] for x in w[3])),betweenFirstTwoLoops=len(between),betweenFirstTwoMaxMs=max((x[1] for x in between),default=0)/1e6);summary["windows"].append(item);print("WINDOW",item,flush=True)
(P/"loop-window-checks.json").write_text(json.dumps(summary,indent=2));print("LOOP",{k:v for k,v in summary.items() if k not in ("setups","windows")},flush=True)
events=[json.loads(s) for s in (E/"events.jsonl").read_text(encoding="utf-8").splitlines()];snaps=[e for e in events if e["message"]=="imaging_queue_snapshot"]
if snaps:print("IMAGINGMAX",{k:max(int(e["fields"].get(k,0)) for e in snaps) for k in snaps[-1]["fields"]},flush=True)
with (P/"selected-events.txt").open("w",encoding="utf-8") as z:
 for e in events:
  if any(x in e["message"].lower() for x in ["保存",".dat","source_","measurement","startup","停止","启动","build","identity"]):z.write(json.dumps(e,ensure_ascii=False)+"\n")
out=[]
for dirname,ri in [("06",1),("07",3)]:
 r=rounds[ri]
 for c in range(4):
  groups=sorted([g for g in gs if g["card"]==c and r["start"]<=g["trigger"]<=r["end"] and g["save"]],key=lambda g:g["first"])
  for ch in "AB":
   fs=sorted((Path("D:/zzx/data/实时重建")/dirname).glob(f"Card{c+1}_Ch{ch}_*.dat"));raw=b"".join(f.read_bytes() for f in fs);assert len(raw)==len(groups)*n*2
   zeroRegions=[not any(raw[j*n*2+slot*360:j*n*2+min(slot*180+180,n)*2]) for j,g in enumerate(groups) for slot in g["missing"]];v=dict(directory=dirname,card=c+1,channel=ch,bytes=len(raw),frames=len(groups),zeroFrames=sum(not any(raw[j*n*2:(j+1)*n*2]) for j in range(len(groups))),missingRegions=len(zeroRegions),allMissingZero=all(zeroRegions),files=[dict(name=f.name,bytes=f.stat().st_size,sha256=hashlib.sha256(f.read_bytes()).hexdigest()) for f in fs]);out.append(v);print("DATA",{k:v for k,v in v.items() if k!="files"},flush=True)
(P/"saved-data-checks.json").write_text(json.dumps(out,indent=2))
exe=Path("_startup_diagnostics_20260911/artifacts/StartupDiagnostics/bin/PAimageReceiverDiagnostics.exe");print("EXE",hashlib.sha256(exe.read_bytes()).hexdigest(),flush=True)
