import json,hashlib
from pathlib import Path
P=Path(__file__).parent;d=json.loads((P/"independent-groups.json").read_text());rounds=json.loads((P/"rounds.json").read_text());out=[]
for directory,prefix,n in [("002","d812",7500),("003","cf88",12500)]:
 v=next(v for k,v in d.items() if k.startswith(prefix));exp=(n*8+1439)//1440
 for c in range(4):
  gs=sorted([g for g in v["groups"] if g["card"]==c and g["save"]],key=lambda g:g["first"])
  for ch in "AB":
   fs=sorted((Path("D:/zzx/data/实时重建")/directory).glob(f"Card{c+1}_Ch{ch}_*.dat"));raw=b"".join(f.read_bytes() for f in fs);assert len(raw)==len(gs)*n*2
   checked=0;nonzero=0
   for j,g in enumerate(gs):
    for slot in g["missing"]:
     lo=slot*180;hi=min(lo+180,n);checked+=1;nonzero+=bool(any(raw[j*n*2+lo*2:j*n*2+hi*2]))
   r=dict(directory=directory,card=c+1,channel=ch,frames=len(gs),missingRegionsChecked=checked,nonzeroMissingRegions=nonzero,zeroFrames=sum(not any(raw[j*n*2:(j+1)*n*2]) for j in range(len(gs))));out.append(r);print(r,flush=True)
(P/"payload-zero-checks.json").write_text(json.dumps(out,indent=2))
for r in sorted(rounds,key=lambda r:r["time"]):
 gs=sorted([g for g in d[r["run"]]["groups"] if g["card"]==0 and r["start"]<=g["trigger"]<=r["end"]],key=lambda g:g["first"]);first=gs[0]
 print("START",r["us"],r["start"],[(g["trigger"],round((g["first"]-first["first"])/1e6,4)) for g in gs[:4]],"bad_count",len(r["partialEffective"]),flush=True)
exe=Path("_receiver_diagnostics_20260910/MC_410T_MultiCard/delivery/build/mingw_release/bin/PAimageReceiverDiagnostics.exe")
print("EXE",hashlib.sha256(exe.read_bytes()).hexdigest(),flush=True)
for cfg in (P/"evidence/paimage").glob("*/run-config.json"):
 t=json.loads((cfg.parent/"timing-summary.json").read_text());print("TIMING",cfg.parent.name,{k:v for k,v in t.items() if not isinstance(v,(list,dict))},flush=True)
events=[json.loads(s) for s in (P/"evidence/events.jsonl").read_text(encoding="utf-8").splitlines()]
for e in events:
 if any(x in e["message"] for x in ["保存",".dat","启动成功","成像重建已启动"]):print("EVENT",e["timestamp"],e["message"],e["fields"],flush=True)
