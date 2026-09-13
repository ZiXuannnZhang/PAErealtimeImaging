import json,hashlib,collections
from pathlib import Path
P=Path(__file__).parent;d=json.loads((P/'independent-groups.json').read_text(encoding='utf-8'));v=d['1842ff9d-613e-4ae2-bdbf-c660435bd28b'];rs=[]
for c in range(4):
 gs=sorted([g for g in v['groups'] if g['card']==c and g['save']],key=lambda g:g['first'])
 for ch in 'AB':
  fs=sorted(Path('D:/zzx/data/实时重建/01').glob(f'Card{c+1}_Ch{ch}_test_*.dat'));raw=b''.join(f.read_bytes() for f in fs);assert len(raw)==len(gs)*10000
  checks=[]
  for j,g in enumerate(gs):
   for slot in g['missing']:
    lo=slot*180;hi=min(lo+180,5000);checks.append(not any(raw[j*10000+lo*2:j*10000+hi*2]))
  r=dict(card=c+1,channel=ch,frames=len(gs),bytes=len(raw),firstTrigger=gs[0]['trigger'],lastTrigger=gs[-1]['trigger'],partial=[g['trigger'] for g in gs if g['unique']<28],allMissingRegionsZero=all(checks),zeroFrames=[i for i in range(len(gs)) if not any(raw[i*10000:(i+1)*10000])],files=[dict(name=f.name,sha256=hashlib.sha256(f.read_bytes()).hexdigest()) for f in fs]);rs.append(r);print({k:z for k,z in r.items() if k!='files'})
(P/'saved-data-checks.json').write_text(json.dumps(rs,indent=2))
ns=[json.loads(x) for x in (P/'evidence/network_history.jsonl').read_text(encoding='utf-8').splitlines()];ls=collections.defaultdict(list)
for x in ns:
 if x['data'].get('kind')=='runtime_ingress':ls[x['data']['listenId']].append(x)
for lid,rows in ls.items():
 print('NETWORK',lid)
 for x in (rows[0],rows[-1]):
  q=x['data'];print(x['timestamp'],[(i['description'],{k:v['absolute'] for k,v in i['counters'].items() if k in ('InErrors','InDiscards','InUcastPkts')}) for i in q['relevantInterfaces']],q['systemUdpCounters'].get('InErrors'))
for rid,v in d.items():
 exp=(v['config']['samples']*8+1439)//1440;b=[g for g in v['groups'] if g['unique']<exp]
 if b:print('BADTIME',rid,min(g['time'] for g in b),max(g['time'] for g in b),'uniqueTriggers',len({g['trigger'] for g in b}))
