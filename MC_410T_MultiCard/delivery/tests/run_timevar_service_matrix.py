"""Reproducible serial GPU service benchmark; no hardware ingress/T_arr claim."""
import argparse
import pathlib
import subprocess
import json
import re
import math
import statistics
import time

p=argparse.ArgumentParser()
p.add_argument('--bin',required=True)
p.add_argument('--out',required=True)
p.add_argument('--grid-mm',default='0.1')
p.add_argument('--samples',default='5000,7500')
p.add_argument('--channels',default='4,8')
p.add_argument('--modes',default='off,on,high')
p.add_argument('--rounds',type=int,default=0)
p.add_argument('--radius-per-ch')
p.add_argument('--sys-delay-per-ch')
args=p.parse_args()
binary=pathlib.Path(args.bin).resolve();out=pathlib.Path(args.out).resolve();out.mkdir(parents=True,exist_ok=True)
results=[]
for n in map(int,args.samples.split(',')):
    for channels in map(int,args.channels.split(',')):
        for mode in args.modes.split(','):
            name=f'N{n}_C{channels}_{mode}_grid{args.grid_mm}'
            rounds=args.rounds or (4 if channels==4 else 7)
            command=[str(binary/'ring_svc_selftest.exe'),'--synthetic','1','--samples',str(n),
                '--svc',str(binary/'ImagingSvc.exe'),'--id','14','--channels',str((1<<channels)-1),
                '--block','200','--rounds',str(rounds),'--grid-mm',args.grid_mm,
                '--timevar-lowpass',mode,'--no-save','1','--out',str(out/name)]
            if args.radius_per_ch:command+=['--radius-per-ch',args.radius_per_ch]
            if args.sys_delay_per_ch:command+=['--sys-delay-per-ch',args.sys_delay_per_ch]
            log=out/(name+'.txt');start=time.monotonic()
            with log.open('wb') as f:
                run=subprocess.run(command,cwd=binary,stdout=f,stderr=subprocess.STDOUT,timeout=1800)
            text=log.read_text(errors='replace')
            values=[(float(a),float(b)) for a,b in re.findall(r'roundtrip_ms=([\d.]+) service_ms=([\d.]+)',text)]
            entry={'name':name,'command':command,'exit':run.returncode,'wall_seconds':time.monotonic()-start,'blocks':len(values)}
            if values:
                entry['cold_roundtrip_ms'],entry['cold_service_ms']=values[0]
                for index,key in [(0,'roundtrip'),(1,'service')]:
                    warm=sorted(v[index] for v in values[1:])
                    entry[key]={'warm_count':len(warm),'median_ms':statistics.median(warm),
                        'p95_ms':warm[math.ceil(.95*len(warm))-1],'max_ms':max(warm),
                        'lines_per_second':channels*200/(statistics.median(warm)/1000)}
            results.append(entry)
            (out/'summary.json').write_text(json.dumps(results,indent=2))
            print(json.dumps(entry),flush=True)
            if run.returncode or len(values)<2:raise SystemExit(1)
