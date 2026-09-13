import json,struct,collections
from pathlib import Path
P=Path(__file__).parent;rid='1842ff9d-613e-4ae2-bdbf-c660435bd28b';v=json.loads((P/'independent-groups.json').read_text(encoding='utf-8'))[rid]
g=v['groups'];bounds={}
for name,lo,hi in [('save_start',16569,16620),('imaging_start',20570,20646),('imaging_run',20647,24570)]:
 x=[r for r in g if lo<=r['trigger']<=hi];bounds[name]=(min(r['first'] for r in x),max(r['last'] for r in x))
stats={k:collections.defaultdict(lambda:[0,0,None]) for k in bounds};S=struct.Struct('<QQQQQIIIHhHHI')
for p in sorted((P/'evidence/paimage'/rid).glob('timing-*.bin')):
 for r in S.iter_unpack(p.read_bytes()):
  seq,start,end,s,c,tid,v0,v1,port,card,kind,flags,res=r
  if kind==1:continue
  for name,(lo,hi) in bounds.items():
   if start<=hi and end>=lo:
    z=stats[name][kind];z[0]+=1
    if end-start>z[1]:z[1]=end-start;z[2]=dict(sequence=seq,correlation=c,card=card,start=start,end=end,value0=v0,flags=flags)
out={name:{kind:dict(retainedCount=x[0],maxMs=x[1]/1e6,record=x[2]) for kind,x in kinds.items()} for name,kinds in stats.items()}
(P/'imaging-timing-windows.json').write_text(json.dumps(out,indent=2))
for name,ks in out.items():print(name,{k:round(x['maxMs'],4) for k,x in ks.items()})
