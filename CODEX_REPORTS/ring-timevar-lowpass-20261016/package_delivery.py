"""Package a clean, exact-HEAD standard build; refuse stale identity/runtime."""
import argparse
import datetime
import hashlib
import json
import os
import pathlib
import shutil
import subprocess

p=argparse.ArgumentParser();p.add_argument('--build-log',required=True);a=p.parse_args()
root=pathlib.Path(__file__).resolve().parents[2]
validation=root/'artifacts/ring-timevar-lowpass-20261016'
git=lambda *args:subprocess.check_output(['git',*args],cwd=root,text=True,encoding='utf-8').strip()
sha=git('rev-parse','HEAD');branch=git('branch','--show-current')
assert not git('status','--porcelain','--untracked-files=no'),'tracked tree is dirty'
assert not git('diff','--name-only','bac25da503022ba2e70e5405a3a3631efd9f5727',sha,'--',
    'MC_410T_MultiCard/delivery/src','MC_410T_MultiCard/delivery/include',
    'MC_410T_MultiCard/delivery/tests','MC_410T_MultiCard/delivery/CMakeLists.txt'),'validated source tree changed'
assert '100% tests passed, 0 tests failed out of 51' in (validation/'ctest.txt').read_text(errors='replace')
assert 'PASS baselineOFF=128 MATLAB cases=36 columns=327' in (validation/'numeric-final.txt').read_text()
for summary in ['service-matrix/summary.json','actual-geometry/summary.json']:
    for cell in json.loads((validation/summary).read_text()):
        assert cell['exit']==0 and cell['service']['warm_count']>=30,cell['name']
assert len(json.loads((validation/'off-reconstruction-bitwise.json').read_text())['files'])==20
for step in json.loads((validation/'final-replay-receipt.json').read_text()):
    if step['name']!='timeout-round':assert step['exit']==0,step['name']
assert json.loads((validation/'timeout-round-retry-receipt.json').read_text())['exit']==0
header=(root/'MC_410T_MultiCard/delivery/build/mingw_debug/generated/PaimageAcquisition/BuildIdentity.h').read_text()
assert f'#define PAIMAGE_GIT_SHA "{sha}"' in header
assert '#define PAIMAGE_TRACKED_DIRTY false' in header
native=root/'MC_410T_MultiCard/delivery/build/mingw_debug/bin'
for name in ['PAimageReceiverDiagnostics.exe','ImagingSvc.exe','ring_svc_selftest.exe','ring_udp_replay.exe',
             'platforms/qwindows.dll','diagnostic-tools/paimage_trace_analyze.py']:
    assert (native/name).is_file(),name
assert sha.encode() in (native/'PAimageReceiverDiagnostics.exe').read_bytes(),'embedded SHA missing'
key=['pa_recon_core.dll','cufft64_12.dll','ring_recon_cuda.dll','cudart64_12.dll','libzmq-v141-mt-4_3_5.dll']
def hashfile(f):
    with open(f,'rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()
expected={line.split()[1]:line.split()[0] for line in (validation/'validation-runtime-sha256.txt').read_text().splitlines()}
for name in key:assert hashfile(native/name)==expected[name],f'validation runtime changed: {name}'
validated_exe={line.split()[1]:line.split()[0] for line in (validation/'validation-executable-sha256.txt').read_text().splitlines()}
for name in ['ImagingSvc.exe','ring_svc_selftest.exe','ring_udp_replay.exe']:
    assert hashfile(native/name)==validated_exe[name],f'validated service/tool binary changed: {name}'
remote=git('ls-remote','origin','refs/heads/'+branch).split()[0]
assert remote==sha,'local/remote HEAD mismatch'
now=datetime.datetime.now(datetime.timezone(datetime.timedelta(hours=8)))
delivery=root/'artifacts/build-delivery'/f'{now:%Y%m%d-%H%M%S}_{sha[:7]}'
delivery.mkdir(parents=True,exist_ok=False);shutil.copytree(native,delivery/'bin')
manifest=dict(repository='ZiXuannnZhang/PAErealtimeImaging',branch=branch,commit=sha,tracked_tree_clean=True,
    implementation_source='bac25da503022ba2e70e5405a3a3631efd9f5727',build_time=now.isoformat(),
    machine=os.environ.get('COMPUTERNAME','unknown'),configure_preset='mingw-debug',build_preset='mingw-debug-build',
    build_script='MC_410T_MultiCard/delivery/build_mingw_debug.cmd',build_type='Debug',USE_ZEROMQ=True,
    Qt='6.8.0 mingw_64',compiler='GNU 13.1.0',CMake='3.30.5',Ninja='1.12.1',
    kernel_options='RingTimevarLowpass.cpp: -O2, Debug symbols retained, no fast-math',
    toolchain_root='D:/Qt/Qt6.8.0',Ring_CUDA_source='_migration_pack/prebuilt_cuda (unchanged)',
    cudart='CUDA toolkit v12.8; identical SHA to installed C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.8/bin/cudart64_12.dll',
    cufft='pre-existing accepted delivery/libs/imaging/cufft64_12.dll; NVIDIA signature valid; cuFFT 12.1.0.31; same SHA as prior accepted reports',
    ZeroMQ='repository third_party/zeromq 4.3.5',linear_imaging='tracked libs/imaging/pa_recon_core.dll',
    native_bin=str(native),delivery_path=str(delivery),zip_created=False)
(delivery/'build-manifest.txt').write_text(json.dumps(manifest,ensure_ascii=False,indent=2),encoding='utf-8')
(delivery/'git-receipt.txt').write_text(f'branch={branch}\nlocal_HEAD={sha}\nremote_HEAD={remote}\ntracked_clean=true\n'+git('log','-1','--format=fuller')+'\n'+header,encoding='utf-8')
(delivery/'dependency-sha256.txt').write_text(''.join(hashfile(delivery/'bin'/name)+'  bin/'+name+'\n' for name in key),encoding='utf-8')
(delivery/'validation.txt').write_text(
    'Software: 51/51 CTest PASS; MATLAB 36 configs/327 columns; 128 baseline OFF byte fingerprints PASS.\n'
    'Exact old service vs candidate OFF reconstruction: 20 wavelength image files (10 block snapshots) bitwise identical.\n'
    'Final source microbenchmark: 12 cells x 30 warm full blocks; service matrix 12 cells, >=30 warm blocks each.\n'
    'Actual local geometry: N=7500, 2ch, 36 mm FOV, .01 mm grid, 6.48 mm selected radii, delays 448/464; OFF/ON/high >=30 warm blocks.\n'
    'ON identity barrier, incomplete-round and timeout replay PASS.\n'
    'Hardware/visual/T_arr: UNVERIFIED; realtime remains conditional.\n'
    'Detailed logs and source/runtime provenance: CODEX_REPORTS/ring-timevar-lowpass-20261016 and artifacts/ring-timevar-lowpass-20261016.\n',encoding='utf-8')
shutil.copy2(a.build_log,delivery/'build.log')
files=sorted(f for f in delivery.rglob('*') if f.is_file())
(delivery/'file-sha256.txt').write_text(''.join(hashfile(f)+'  '+f.relative_to(delivery).as_posix()+'\n' for f in files),encoding='utf-8')
for line in (delivery/'file-sha256.txt').read_text().splitlines():
    digest,name=line.split('  ',1);assert hashfile(delivery/name)==digest,name
(validation/'final-delivery.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2),encoding='utf-8')
print(str(delivery))
