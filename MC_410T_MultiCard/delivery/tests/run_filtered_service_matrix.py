"""Serial production GPU benchmark, exact eed98f0 service vs candidate. No T_arr claim."""
import argparse
import ctypes as c
from ctypes import wintypes as w
import hashlib
import json
import math
import os
from pathlib import Path
import re
import statistics
import subprocess
import time

p=argparse.ArgumentParser();p.add_argument('--bin',required=True);p.add_argument('--baseline',required=True)
p.add_argument('--out',required=True);p.add_argument('--phase',choices=['baseline','candidate','all'],default='all')
p.add_argument('--only',help='Comma-separated exact cell names for an independently retained repeat')
p.add_argument('--frame-timeout-ms',type=int,default=20000,help='Measurement sender deadline; no production timeout change')
a=p.parse_args();binary=Path(a.bin).resolve();baseline=Path(a.baseline).resolve();out=Path(a.out).resolve();out.mkdir(parents=True,exist_ok=True)
def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()
class Entry(c.Structure):
    _fields_=[('size',w.DWORD),('usage',w.DWORD),('pid',w.DWORD),('heap',c.c_size_t),('module',w.DWORD),
              ('threads',w.DWORD),('parent',w.DWORD),('priority',w.LONG),('flags',w.DWORD),('exe',w.WCHAR*260)]
class Memory(c.Structure):
    _fields_=[('size',w.DWORD),('faults',w.DWORD)]+[(x,c.c_size_t) for x in
        ['peak_working_set','working_set','peak_paged_pool','paged_pool','peak_nonpaged_pool','nonpaged_pool','pagefile','peak_pagefile']]
k=c.WinDLL('kernel32',use_last_error=True);ps=c.WinDLL('psapi',use_last_error=True)
k.CreateToolhelp32Snapshot.restype=w.HANDLE;k.CreateToolhelp32Snapshot.argtypes=[w.DWORD,w.DWORD]
k.Process32FirstW.argtypes=[w.HANDLE,c.POINTER(Entry)];k.Process32NextW.argtypes=[w.HANDLE,c.POINTER(Entry)]
k.OpenProcess.restype=w.HANDLE;k.OpenProcess.argtypes=[w.DWORD,w.BOOL,w.DWORD];k.CloseHandle.argtypes=[w.HANDLE]
ps.GetProcessMemoryInfo.argtypes=[w.HANDLE,c.POINTER(Memory),w.DWORD]
def sample(parent,peaks):
    snap=k.CreateToolhelp32Snapshot(2,0);e=Entry();e.size=c.sizeof(e);ok=k.Process32FirstW(snap,c.byref(e))
    while ok:
        if e.parent==parent and e.exe.lower()=='imagingsvc.exe':
            handle=k.OpenProcess(0x410,False,e.pid)
            if handle:
                m=Memory();m.size=c.sizeof(m)
                if ps.GetProcessMemoryInfo(handle,c.byref(m),m.size):
                    peaks['pid']=e.pid;peaks['working_set']=max(peaks.get('working_set',0),m.peak_working_set)
                    peaks['pagefile']=max(peaks.get('pagefile',0),m.peak_pagefile)
                k.CloseHandle(handle)
        ok=k.Process32NextW(snap,c.byref(e))
    k.CloseHandle(snap)

specs=[]
for n in [5000,7500]:
    for channels in [4,8]:
        for tv in ['off','on']:
            specs.append(dict(group='load',n=n,channels=channels,tv=tv,fd='off',batch=64,grid=.1,block=200))
            specs.append(dict(group='load',n=n,channels=channels,tv=tv,fd='on',batch=64,grid=.1,block=200))
        specs.append(dict(group='high',n=n,channels=channels,tv='high',fd='high',batch=64,grid=.1,block=200))
for tv in ['off','on']:
    for fd in ['off','on']:
        specs.append(dict(group='actual',n=7500,channels=2,tv=tv,fd=fd,batch=64,grid=.01,block=10))
for batch in [1,7]:
    specs.append(dict(group='batch',n=7500,channels=8,tv='off',fd='on',batch=batch,grid=.1,block=200))
results=[]
summary=out/'summary.json'
if summary.exists():results=json.loads(summary.read_text())
for phase in (['baseline','candidate'] if a.phase=='all' else [a.phase]):
    service=(baseline if phase=='baseline' else binary)/'ImagingSvc.exe'
    service_hash=digest(service);tool_hash=digest(binary/'ring_svc_selftest.exe')
    for spec in specs:
        if phase=='baseline' and (spec['fd']!='off' or spec['group'] not in ['load','actual']):continue
        name=f"{phase}_{spec['group']}_N{spec['n']}_C{spec['channels']}_tv{spec['tv']}_fd{spec['fd']}_b{spec['batch']}"
        if a.only and name not in a.only.split(','):continue
        old=next((r for r in results if r['name']==name),None)
        if old:
            assert old['service_sha256']==service_hash,'refuse mixed service timings'
            if old['tool_sha256']!=tool_hash:
                previous=out/'tool-versions'/old['tool_sha256']/'ring_svc_selftest.exe'
                assert previous.exists() and digest(previous)==old['tool_sha256'],'previous measurement tool must be retained'
            assert old['exit']==0 and old['service']['warm_count']>=30
            continue
        blocks_per_round=8000//(spec['channels']*spec['block'])
        rounds=math.ceil(31/blocks_per_round)
        command=[str(binary/'ring_svc_selftest.exe'),'--synthetic','1','--samples',str(spec['n']),
                 '--svc',str(service),'--id','14','--channels',str((1<<spec['channels'])-1),
                 '--block',str(spec['block']),'--rounds',str(rounds),'--grid-mm',str(spec['grid']),
                 '--benchmark-blocks','31',
                 '--frame-timeout-ms',str(a.frame_timeout_ms),
                 '--timevar-lowpass',spec['tv'],'--filtered-das',spec['fd'],'--filtered-batch',str(spec['batch']),
                 '--no-save','1','--out',str(out/name)]
        if spec['group']=='actual':
            command+=['--radius-per-ch','6.48,6.48,6.57,6.57,6.57,6.57,6.57,6.57',
                      '--splice','1','--splice-blend','5','--sos-radii-mm','7.5','--sos','1490,1540',
                      '--sector-start','0','--mask-length','375',
                      '--sys-delay-per-ch','448,464,448,464,358,371,358,371,358,371,358,371,358,371,358,371']
        env=os.environ.copy();env['PATH']=str(service.parent)+';'+str(binary)+';'+env['PATH']
        start=time.monotonic();peaks={}
        with (out/(name+'.txt')).open('w',encoding='utf-8') as f:
            f.write('command='+json.dumps(command)+'\ncwd='+str(binary)+'\n');f.flush()
            proc=subprocess.Popen(command,cwd=binary,stdout=f,stderr=subprocess.STDOUT,env=env)
            while proc.poll() is None:
                sample(proc.pid,peaks);time.sleep(.1)
        text=(out/(name+'.txt')).read_text(errors='replace')
        values=[(float(x),float(y)) for x,y in re.findall(r'roundtrip_ms=([\d.]+) service_ms=([\d.]+)',text)]
        item=dict(name=name,spec=spec,command=command,cwd=str(binary),exit=proc.returncode,
                  wall_seconds=time.monotonic()-start,blocks=len(values),service_sha256=service_hash,
                  tool_sha256=tool_hash,peak_bytes=peaks)
        if values:
            item['cold_roundtrip_ms'],item['cold_service_ms']=values[0]
        if len(values)>1:
            for i,key in enumerate(['roundtrip','service']):
                warm=sorted(x[i] for x in values[1:]);median=statistics.median(warm)
                item[key]=dict(warm_count=len(warm),median_ms=median,p95_ms=warm[math.ceil(.95*len(warm))-1],
                               max_ms=max(warm),lines_per_second=spec['channels']*spec['block']*1000/median)
        metrics=re.findall(r'design_us=([\d.]+) cached_bytes=([\d.]+) workspace_bytes=([\d.]+) max_batch=([\d.]+)',text)
        if metrics:item['filtered_metrics']=dict(zip(['design_us','cached_bytes','workspace_bytes','max_batch'],map(float,metrics[-1])))
        results.append(item);summary.write_text(json.dumps(results,indent=2),encoding='utf-8')
        print(name,'exit',proc.returncode,'blocks',len(values),'median',item.get('service',{}).get('median_ms'),flush=True)
        assert proc.returncode==0 and len(values)>=31 and peaks.get('working_set',0)>0,name
