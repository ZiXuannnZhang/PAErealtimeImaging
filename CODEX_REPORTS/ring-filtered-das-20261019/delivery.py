"""Reproducible build, validation and exact-SHA packaging; output stays outside Git."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

p=argparse.ArgumentParser();p.add_argument('action',choices=['build','tests','validate','replay','package']);p.add_argument('--stage',default='dev')
a=p.parse_args();root=Path(__file__).resolve().parents[2];project=root/'MC_410T_MultiCard/delivery'
out=root/'artifacts/filtered-das';out.mkdir(parents=True,exist_ok=True);native=project/'build/mingw_debug/bin'
cmake=Path('D:/Qt/Qt6.8.0/Tools/CMake_64/bin/cmake.exe');ctest=cmake.with_name('ctest.exe')
python=Path('C:/Users/yyps/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe')
git=lambda *args:subprocess.check_output(['git',*args],cwd=root,encoding='utf-8').strip()
sha=git('rev-parse','HEAD');branch=git('branch','--show-current');clean=not git('status','--porcelain','--untracked-files=no')
assert branch=='codex/ring-timevar-lowpass-20261016'
if a.stage=='final':assert clean,'final requires tracked-clean'
env=os.environ.copy();env['PATH']=';'.join([str(native),'D:/Qt/Qt6.8.0/6.8.0/mingw_64/bin',
    'D:/Qt/Qt6.8.0/Tools/mingw1310_64/bin','D:/Qt/Qt6.8.0/Tools/Ninja',env['PATH']])
env['QT_QPA_PLATFORM']='offscreen';env['QT_PLUGIN_PATH']='D:/Qt/Qt6.8.0/6.8.0/mingw_64/plugins'
(out/'temp').mkdir(exist_ok=True);env['TEMP']=env['TMP']=str(out/'temp')
receipt=dict(sha=sha,branch=branch,tracked_clean=clean,commands=[])
def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def run(name,command,cwd=root):
    command=list(map(str,command));log=out/f'{a.stage}-{a.action}-{name}.txt';start=time.monotonic()
    with log.open('w',encoding='utf-8') as f:
        f.write('command='+json.dumps(command)+'\ncwd='+str(cwd)+'\n');f.flush()
        r=subprocess.run(command,cwd=cwd,env=env,stdout=f,stderr=subprocess.STDOUT)
    receipt['commands'].append(dict(name=name,command=command,cwd=str(cwd),exit_code=r.returncode,
                                    seconds=time.monotonic()-start,log=str(log)))
    (out/f'{a.stage}-{a.action}.json').write_text(json.dumps(receipt,indent=2),encoding='utf-8')
    print(name,'exit',r.returncode,flush=True);assert r.returncode==0,str(log)
if a.action=='build':
    run('cmake-version',[cmake,'--version'])
    run('compiler-version',['D:/Qt/Qt6.8.0/Tools/mingw1310_64/bin/g++.exe','--version'])
    run('qt-version',['D:/Qt/Qt6.8.0/6.8.0/mingw_64/bin/qmake.exe','--version'])
    run('ninja-version',['D:/Qt/Qt6.8.0/Tools/Ninja/ninja.exe','--version'])
    run('configure-main',[cmake,'--preset','mingw-debug','-DBUILD_MAINWINDOW_START_TEST=ON'],project)
    run('standard-main',['cmd','/c','build_mingw_debug.cmd'],project)
    run('configure-tests',[cmake,'-S',project/'tests','-B',project/'build/filtered_das_tests','-G','Ninja',
        '-DCMAKE_BUILD_TYPE=Debug','-DCMAKE_CXX_COMPILER=D:/Qt/Qt6.8.0/Tools/mingw1310_64/bin/g++.exe',
        '-DQt6_DIR=D:/Qt/Qt6.8.0/6.8.0/mingw_64/lib/cmake/Qt6','-DPython3_EXECUTABLE='+str(python)])
    run('build-tests',[cmake,'--build',project/'build/filtered_das_tests','-j','8'])
elif a.action=='tests':
    run('build-tests',[cmake,'--build',project/'build/filtered_das_tests','-j','8'])
    run('list',[ctest,'--test-dir',project/'build/filtered_das_tests','-N'])
    run('regression-new',[ctest,'--test-dir',project/'build/filtered_das_tests','-V','--timeout','90'])
    assert '0 tests failed out of 55' in (out/f'{a.stage}-tests-regression-new.txt').read_text()
elif a.action=='validate':
    run('list',[ctest,'--test-dir',project/'build/filtered_das_tests','-N'])
    run('regression-new',[ctest,'--test-dir',project/'build/filtered_das_tests','-V','--timeout','90'])
    assert '0 tests failed out of 55' in (out/f'{a.stage}-validate-regression-new.txt').read_text()
    run('main-start',[ctest,'--test-dir',project/'build/mingw_debug','-V','-R','^mainwindow_start_integration_test$'])
    assert '0 tests failed out of 1' in (out/f'{a.stage}-validate-main-start.txt').read_text()
elif a.action=='replay':
    baseline=root/'artifacts/filtered-das/baseline/bin'
    receipt['baseline_service_sha256']=digest(baseline/'ImagingSvc.exe')
    for tv in ['off','on']:
        for label,service in [('baseline',baseline/'ImagingSvc.exe'),('candidate',native/'ImagingSvc.exe')]:
            target=out/f'{a.stage}-off-{label}-{tv}'
            command=[native/'ring_svc_selftest.exe','--synthetic','1','--samples','5000','--id','14',
                '--channels','15','--block','200','--rounds','1','--grid-mm','.1','--svc',service,
                '--timevar-lowpass',tv,'--filtered-das','off','--out',target]
            run(label+'-'+tv,command)
        old=out/f'{a.stage}-off-baseline-{tv}';new=out/f'{a.stage}-off-candidate-{tv}'
        files=sorted(old.glob('*.raw'));assert len(files)==20
        receipt['off_'+tv]=[]
        for f in files:
            first,second=digest(f),digest(new/f.name);assert first==second,f.name
            receipt['off_'+tv].append(dict(name=f.name,baseline=first,candidate=second))
    common=[native/'ring_svc_selftest.exe','--synthetic','1','--samples','5000','--id','14','--channels','15',
        '--block','200','--rounds','2','--grid-mm','.1','--svc',native/'ImagingSvc.exe','--no-save','1',
        '--timevar-lowpass','on','--filtered-das','on']
    for name,flags in [('identity',['--identity-jump-at','1']),('incomplete',['--drop-block','1']),('timeout',['--timeout-reset','2'])]:
        scenario=common.copy()
        if name=='timeout':
            # Keep 2 s idle and full round size, using smaller complete blocks
            # so the synchronous synthetic sender does not count DSP wall time as idle.
            scenario[scenario.index('--block')+1]='20'
        run(name,scenario+flags+['--out',out/(a.stage+'-'+name)])
    (out/f'{a.stage}-replay.json').write_text(json.dumps(receipt,indent=2))
else:
    assert a.stage=='final' and clean
    remote=git('ls-remote','origin','refs/heads/'+branch).split()[0];assert remote==sha
    header=(project/'build/mingw_debug/generated/PaimageAcquisition/BuildIdentity.h').read_text()
    assert f'PAIMAGE_GIT_SHA "{sha}"' in header and 'PAIMAGE_TRACKED_DIRTY false' in header
    assert sha.encode() in (native/'PAimageReceiverDiagnostics.exe').read_bytes()
    for action in ['build','validate','replay']:
        r=json.loads((out/f'final-{action}.json').read_text());assert r['sha']==sha and r['tracked_clean']
        assert all(x['exit_code']==0 for x in r['commands'])
    performance=json.loads((out/'performance/summary.json').read_text())
    provenance=json.loads((out/'performance/source-provenance.json').read_text())
    assert provenance['tool_sha256']==digest(native/'ring_svc_selftest.exe')
    for tool_hash,source_hash in provenance['tool_versions'].items():
        version=out/'performance/tool-versions'/tool_hash
        assert digest(version/'ring_svc_selftest.exe')==tool_hash
        assert digest(version/'ring_svc_selftest.cpp')==source_hash
    assert len(performance)==36 and len({r['name'] for r in performance})==36
    for r in performance:
        assert r['exit']==0 and r['blocks']==31 and r['service']['warm_count']==30
        assert r['tool_sha256'] in provenance['tool_versions'],'unrecorded performance tool'
        if r['name'].startswith('candidate_'):
            assert r['service_sha256']==digest(native/'ImagingSvc.exe'),'performance service differs from final'
    repeats=json.loads((out/'performance/repeat/summary.json').read_text())
    assert len(repeats)==4
    for r in repeats:
        assert r['exit']==0 and r['blocks']==31 and r['service']['warm_count']==30
        assert r['service_sha256']==digest(native/'ImagingSvc.exe') and r['tool_sha256']==digest(native/'ring_svc_selftest.exe')
    for attempt,blocks in [('attempt-01',11),('attempt-02',1)]:
        failures=json.loads((out/('performance/failed-attempts/'+attempt+'/summary.json')).read_text())
        assert len(failures)==1 and failures[0]['exit']==2 and failures[0]['blocks']==blocks
        assert failures[0]['service_sha256']==digest(native/'ImagingSvc.exe')
        assert failures[0]['tool_sha256'] in provenance['tool_versions']
    for path,expected_hash in (provenance['sources']|provenance['measurement_tool_source']).items():
        assert digest(root/path)==expected_hash,'production source changed after performance: '+path
    for name,expected_hash in provenance['runtime_sha256'].items():
        assert digest(native/name)==expected_hash,'runtime changed after performance: '+name
    # Test seams must never enter the production executable or service.
    commands=json.loads((project/'build/mingw_debug/compile_commands.json').read_text())
    for record in commands:
        command=record.get('command',' '.join(record.get('arguments',[])))
        if 'CMakeFiles/MC410T_Receiver.dir' in command.replace('\\','/') or 'CMakeFiles/ImagingSvc.dir' in command.replace('\\','/'):
            assert 'MAINWINDOW_START_TEST_SEAM' not in command and 'RING_TIMEVAR_TEST_SEAM' not in command
    stamp=datetime.datetime.now(datetime.timezone(datetime.timedelta(hours=8))).strftime('%Y%m%d-%H%M%S')
    target=root/'artifacts/build-delivery'/f'{stamp}_{sha[:8]}';target.mkdir(parents=True,exist_ok=False)
    # Preserve the operator's native state in place; a review bundle starts
    # from compiled factory defaults instead of silently shipping local INI/logs.
    local_state={'app_log.txt','PAimageReceiverDiagnostics.ini','paimage-diagnostics'}
    shutil.copytree(native,target/'bin',ignore=lambda directory,names:
                    local_state.intersection(names) if Path(directory)==native else set())
    assert not any((target/'bin'/name).exists() for name in local_state)
    for name in ['PAimageReceiverDiagnostics.exe','ImagingSvc.exe','ring_svc_selftest.exe','ring_udp_replay.exe']:
        assert digest(native/name)==digest(target/'bin'/name)
    assert (target/'bin/platforms/qwindows.dll').exists() and (target/'bin/diagnostic-tools').is_dir()
    keys=['pa_recon_core.dll','cufft64_12.dll','ring_recon_cuda.dll','cudart64_12.dll','libzmq-v141-mt-4_3_5.dll']
    expected=dict((line.split()[1],line.split()[0]) for line in (root/'CODEX_REPORTS/ring-timevar-lowpass-20261016/evidence/validation-runtime-sha256.txt').read_text().splitlines())
    for name in keys:assert digest(native/name)==expected[name],name
    (target/'dependency-sha256.txt').write_text('\n'.join(digest(native/name)+'  '+name for name in keys)+'\n')
    (target/'git-receipt.txt').write_text('branch='+branch+'\nlocal='+sha+'\nremote='+remote+'\ntracked_clean=true\n'+git('log','-1','--format=fuller')+'\n'+git('status','--short')+'\n',encoding='utf-8')
    (target/'BuildIdentity.h').write_text(header)
    (target/'build-manifest.txt').write_text('repository=ZiXuannnZhang/PAErealtimeImaging\nbranch='+branch+'\nsha='+sha+
        '\ntracked_clean=true\npresets=mingw-debug / mingw-debug-build\nscript=build_mingw_debug.cmd\nQt=6.8.0 MinGW=13.1.0 CMake=3.30.5 Ninja=1.12.1\n'+
        'Ring CUDA source=_migration_pack/prebuilt_cuda; unchanged ABI/core; cudart v12.8; cuFFT 12.1.0.31\n'+
        'Local INI, app_log and paimage-diagnostics sessions excluded; original native state preserved.\nnative='+str(native)+'\ndelivery='+str(target)+'\n')
    shutil.copy2(out/'final-build-standard-main.txt',target/'build.log')
    for action in ['build','validate','replay']:shutil.copy2(out/f'final-{action}.json',target/f'validation-{action}.json')
    shutil.copytree(root/'CODEX_REPORTS/ring-filtered-das-20261019',target/'evidence')
    for f in out.glob('final-*.txt'):shutil.copy2(f,target/f.name)
    shutil.copytree(out/'performance',target/'performance')
    (target/'validation.txt').write_text('55/55 regression/new + 1/1 real MainWindow startup PASS. MATLAB 72 cases / 1008 columns / 4 batch variants.\n'+
        '384 exact-eed98f0 CPU OFF fingerprints; timevar OFF/ON service OFF images bitwise.\nHardware visual/acquisition/T_arr=UNVERIFIED. Two enhancement operation card in evidence. Do not merge main automatically.\n')
    (target/'file-sha256.txt').write_text('\n'.join(digest(f)+'  '+f.relative_to(target).as_posix() for f in sorted(target.rglob('*')) if f.is_file())+'\n')
    receipt.update(delivery=str(target),local=sha,remote=remote,native=str(native),dependencies={name:digest(native/name) for name in keys})
    receipt['executables']={f.name:digest(f) for f in native.glob('*.exe')}
    (out/'final-package.json').write_text(json.dumps(receipt,indent=2))
    print(target)
