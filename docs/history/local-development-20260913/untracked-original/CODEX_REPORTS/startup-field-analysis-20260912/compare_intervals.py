import json,statistics
from pathlib import Path
P=Path(__file__).parent;v=next(iter(json.loads((P/"independent-groups.json").read_text()).values()));rounds=json.loads((P/"rounds.json").read_text());out=[]
def stats(xs):
 ys=sorted(xs)
 return {"count":len(xs),"min":min(xs),"median":statistics.median(xs),"p01":ys[int((len(ys)-1)*.01)],"p99":ys[int((len(ys)-1)*.99)],"max":max(xs),"mean":statistics.mean(xs),"outside24_26":sum(not 24<=x<=26 for x in xs)}
for r in rounds:
 row={"start":r["start"],"time":r["time"],"cards":[]}
 for c in range(4):
  gs=sorted([g for g in v["groups"] if g["card"]==c and r["start"]<=g["trigger"]<=r["end"]],key=lambda g:g["first"]);stable=[g for g in gs if g["trigger"]>=max(r["stableStart"],r["start"]+1)]
  ds=[(b["first"]-a["first"])/1e6 for a,b in zip(stable,stable[1:])];span=[(g["last"]-g["first"])/1e6 for g in stable]; deviations=[(g["first"]-stable[0]["first"])/1e6-(g["trigger"]-stable[0]["trigger"])*25 for g in stable]
  cr={"card":c+1,"firstPairs":[{"trigger":g["trigger"],"relativeMs":(g["first"]-gs[0]["first"])/1e6,"packetSpanMs":(g["last"]-g["first"])/1e6,"unique":g["unique"]} for g in gs[:6]],"stableIntervalsMs":stats(ds),"stablePacketSpanMs":stats(span),"relativeTo25msGridMs":{"min":min(deviations),"max":max(deviations),"end":deviations[-1]},"largestIntervals":[{"previous":a["trigger"],"current":b["trigger"],"ms":(b["first"]-a["first"])/1e6} for a,b in sorted(zip(stable,stable[1:]),key=lambda ab:ab[1]["first"]-ab[0]["first"],reverse=True)[:3]]};row["cards"].append(cr)
 print(json.dumps(row,ensure_ascii=False),flush=True);out.append(row)
(P/"interval-comparison.json").write_text(json.dumps(out,ensure_ascii=False,indent=2),encoding="utf-8")
