"""Read Windows peak working-set counters without injecting into measured programs."""
import ctypes as c
from ctypes import wintypes as w
import argparse,json,pathlib,time
p=argparse.ArgumentParser();p.add_argument('--out',required=True);p.add_argument('--stop',required=True);a=p.parse_args()
class Entry(c.Structure):
    _fields_=[('size',w.DWORD),('usage',w.DWORD),('pid',w.DWORD),('heap',c.c_size_t),
        ('module',w.DWORD),('threads',w.DWORD),('parent',w.DWORD),('priority',w.LONG),
        ('flags',w.DWORD),('exe',w.WCHAR*260)]
class Memory(c.Structure):
    _fields_=[('size',w.DWORD),('faults',w.DWORD)]+[(x,c.c_size_t) for x in [
        'peak_working_set','working_set','peak_paged_pool','paged_pool','peak_nonpaged_pool',
        'nonpaged_pool','pagefile','peak_pagefile']]
k=c.WinDLL('kernel32',use_last_error=True);ps=c.WinDLL('psapi',use_last_error=True)
k.CreateToolhelp32Snapshot.restype=w.HANDLE;k.CreateToolhelp32Snapshot.argtypes=[w.DWORD,w.DWORD]
k.Process32FirstW.argtypes=[w.HANDLE,c.POINTER(Entry)];k.Process32NextW.argtypes=[w.HANDLE,c.POINTER(Entry)]
k.OpenProcess.restype=w.HANDLE;k.OpenProcess.argtypes=[w.DWORD,w.BOOL,w.DWORD];k.CloseHandle.argtypes=[w.HANDLE]
ps.GetProcessMemoryInfo.argtypes=[w.HANDLE,c.POINTER(Memory),w.DWORD]
records={};out=pathlib.Path(a.out)
while not pathlib.Path(a.stop).exists():
    snap=k.CreateToolhelp32Snapshot(2,0);e=Entry();e.size=c.sizeof(e)
    ok=k.Process32FirstW(snap,c.byref(e))
    while ok:
        if e.exe.lower() in ['imagingsvc.exe','ring_timevar_bench.exe']:
            handle=k.OpenProcess(0x410,False,e.pid)
            if handle:
                m=Memory();m.size=c.sizeof(m)
                if ps.GetProcessMemoryInfo(handle,c.byref(m),m.size):
                    key=str(e.pid);old=records.get(key,{'pid':e.pid,'parent':e.parent,'exe':e.exe,'peak_working_set':0,'peak_pagefile':0})
                    old['peak_working_set']=max(old['peak_working_set'],m.peak_working_set)
                    old['peak_pagefile']=max(old['peak_pagefile'],m.peak_pagefile)
                    records[key]=old
                k.CloseHandle(handle)
        ok=k.Process32NextW(snap,c.byref(e))
    k.CloseHandle(snap)
    out.write_text(json.dumps(list(records.values()),indent=2));time.sleep(.25)
print('max_peak_working_set_bytes',max((v['peak_working_set'] for v in records.values()),default=0))
