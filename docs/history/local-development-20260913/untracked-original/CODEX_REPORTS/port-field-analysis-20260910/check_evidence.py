import json,hashlib,collections
from pathlib import Path
P=Path(__file__).parent;E=P/"evidence";d=json.loads((P/"independent-analysis.json").read_text(encoding="utf-8"))
results=[]
for c in range(4):
 gs=sorted([g for g in d["groups"] if g["card"]==c and any(e[:3]==[7,0,5] for e in g["events"])],key=lambda g:g["first"])
 for ch in "AB":
  files=sorted(Path("D:/zzx/data/实时重建/01").glob(f"Card{c+1}_Ch{ch}_test_*.dat"));raw=b"".join(f.read_bytes() for f in files);fails=[];checked=0;zero_frames=[]
  assert len(raw)==len(gs)*6250*2
  for i,g in enumerate(gs):
   frame=raw[i*12500:(i+1)*12500]
   if not any(frame):zero_frames.append(i)
   for slot in g["missingRelative"]:
    lo=slot*180;hi=min((slot+1)*180,6250);checked+=hi-lo
    if any(frame[lo*2:hi*2]):fails.append([i,g["trigger"],slot])
  results.append(dict(card=c+1,channel=ch,frames=len(gs),bytes=len(raw),missingSamplesChecked=checked,nonzeroMissingRegions=fails,allZeroFrames=zero_frames,files=[dict(name=f.name,sha256=hashlib.sha256(f.read_bytes()).hexdigest()) for f in files]))
print("DATA",json.dumps([{k:v for k,v in x.items() if k!="files"} for x in results]))
ns=[json.loads(x) for x in (E/"network_history.jsonl").read_text(encoding="utf-8").splitlines()];ns=[x for x in ns if x["data"].get("kind")=="runtime_ingress" and x["data"].get("listenId")=="12368ca7-a37a-4340-9ad5-be82a1f062c1"]
for x in (ns[0],ns[-1]):
 q=x["data"];print("NET",x["timestamp"],[(i["description"],i["receiveLinkSpeed"],{k:v["absolute"] for k,v in i["counters"].items()}) for i in q["relevantInterfaces"]],q["systemUdpCounters"])
events=[json.loads(x) for x in (E/"events.jsonl").read_text(encoding="utf-8").splitlines()]
for e in events:
 if any(k in str(e["fields"]).lower() for k in ["executablesha","sourcecommit"]):print("BUILD",e)
(P/"saved-data-verification.json").write_text(json.dumps(results,ensure_ascii=False,indent=2),encoding="utf-8")
