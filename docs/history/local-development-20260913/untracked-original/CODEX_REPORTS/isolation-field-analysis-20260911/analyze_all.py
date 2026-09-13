import json,importlib.util,hashlib
from pathlib import Path
P=Path(__file__).parent;E=P/"evidence"
spec=importlib.util.spec_from_file_location("a",E/"paimage/tools/paimage_trace_analyze.py");a=importlib.util.module_from_spec(spec);spec.loader.exec_module(a)
hashes=[]
for f in json.loads((E/"paimage/manifest.json").read_text(encoding="utf-8"))["files"]:
 hashes.append(dict(path=f["path"],ok=hashlib.sha256((E/f["path"]).read_bytes()).hexdigest()==f["sha256"]))
(P/"hash-checks.json").write_text(json.dumps(hashes,indent=2))
print("hashes",len(hashes),all(f["ok"] for f in hashes),flush=True)
for cfg in sorted((E/"paimage").glob("*/run-config.json")):
 c=json.loads(cfg.read_text(encoding="utf-8"));n=(c["samples"]*8+1439)//1440
 d=a.analyze(cfg.parent,n,c["dataPort"],c["samples"],32,c["cards"])
 (P/(cfg.parent.name+"-analysis.json")).write_text(json.dumps(d,ensure_ascii=False,indent=2),encoding="utf-8")
 print(cfg.parent.name,json.dumps({k:v for k,v in d.items() if k not in ("anomalies","firstObserved","files","timing","runIdentity","sourceSyncEvents","observedTriggerJumps","caveat")}),flush=True)
 print("first",[(g["card"],g["trigger"],g["unique"]) for g in d["firstObserved"]],"jumps",d["observedTriggerJumps"][:10],"anomalies",len(d["anomalies"]),flush=True)
