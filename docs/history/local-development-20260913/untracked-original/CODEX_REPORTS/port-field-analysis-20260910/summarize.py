import json,collections,statistics,datetime,struct,hashlib
from pathlib import Path
P=Path(__file__).parent;d=json.loads((P/"independent-analysis.json").read_text(encoding="utf-8"));gs=d["groups"];cs=sorted([g for g in gs if g["card"]==0],key=lambda g:g["first"])
rounds=[]
for g in cs:
 if not rounds or (datetime.datetime.fromisoformat(g["first"])-datetime.datetime.fromisoformat(rounds[-1][-1]["first"])).total_seconds()>2:rounds.append([])
 rounds[-1].append(g)
for r in rounds:
 ids={g["trigger"] for g in r};allg=[g for g in gs if g["trigger"] in ids];bad=[g for g in r if g["unique"]!=35];missing=sorted(set(range(min(ids),max(ids)+1))-ids)
 print("ROUND",min(ids),max(ids),"span",max(ids)-min(ids)+1,"observed",len(ids),r[0]["first"],r[-1]["first"],"unseen",len(missing));print("gaps",[(a["trigger"],b["trigger"]) for a,b in zip(r,r[1:]) if b["trigger"]!=a["trigger"]+1]);print("bad",[(g["trigger"],g["unique"],g["first"]) for g in bad]);print("missing packets/card",[sum(35-g["unique"] for g in allg if g["card"]==c) for c in range(4)]);print("firstcard",[(g["trigger"],g["unique"],g["first"]) for g in r[:3]])
 if len(r)>10:print("median ms",statistics.median((datetime.datetime.fromisoformat(b["first"])-datetime.datetime.fromisoformat(a["first"])).total_seconds()*1000 for a,b in zip(r,r[1:]) if b["trigger"]==a["trigger"]+1))
saved=[g for g in cs if any(e[:3]==[7,0,5] for e in g["events"])];print("SAVED",len(saved),saved[0]["trigger"],saved[-1]["trigger"],"incomplete",len([g for g in saved if g["unique"]<35]))
print("all bases",collections.Counter(g["base"] for g in gs));print("badsets equal",all({g["trigger"] for g in gs if g["card"]==c and g["unique"]<35}=={g["trigger"] for g in cs if g["unique"]<35} for c in range(4)))
ids=[]
for p in sorted((P/"evidence/paimage/d1b49ca2-0dda-4c3c-b90f-bcd48170f264").glob("trace-*.bin")):
 b=p.read_bytes();ids.extend(struct.unpack_from("<Q",b,i)[0] for i in range(0,len(b),64))
print("record identity",len(ids),len(set(ids)),min(ids),max(ids));print("unseen total",12006-len(cs))
es=[json.loads(x) for x in (P/"evidence/events.jsonl").read_text(encoding="utf-8").splitlines()]
for e in es:
 if any(w in e["message"].lower() for w in ["保存","identity","build","sha256"]):print(e["timestamp"],e["message"],e["fields"])
