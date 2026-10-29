"""Build and package the lowpass + motor candidate with exact Git identity."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument('action', choices=['build', 'validate', 'package'])
parser.add_argument('--stage', default='dev')
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
project = root / 'MC_410T_MultiCard/delivery'
out = root / 'artifacts/motor-lowpass'
out.mkdir(parents=True, exist_ok=True)
native = project / 'build/mingw_debug/bin'
tests = project / 'build/filtered_das_tests'
qt = Path('D:/Qt/Qt6.8.0')
cmake = qt / 'Tools/CMake_64/bin/cmake.exe'
ctest = cmake.with_name('ctest.exe')
python = Path('C:/Users/yyps/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe')
branch = 'codex/ring-lowpass-motor-linkage-20261029'
baseline = '3340fbff5ed93ff2ba87b496275e663e877fa693'

def git(*parts):
    return subprocess.check_output(['git', *parts], cwd=root, encoding='utf-8').strip()

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

sha = git('rev-parse', 'HEAD')
clean = not git('status', '--porcelain', '--untracked-files=no')
assert git('branch', '--show-current') == branch
subprocess.run(['git', 'merge-base', '--is-ancestor', baseline, sha], cwd=root, check=True)
if args.stage == 'final':
    assert clean, 'final requires tracked-clean'
env = os.environ.copy()
env['PATH'] = ';'.join(map(str, [native, qt/'6.8.0/mingw_64/bin', qt/'Tools/mingw1310_64/bin',
                               qt/'Tools/Ninja', env['PATH']]))
env['QT_QPA_PLATFORM'] = 'offscreen'
env['QT_PLUGIN_PATH'] = str(qt/'6.8.0/mingw_64/plugins')
(out/'temp').mkdir(exist_ok=True)
env['TEMP'] = env['TMP'] = str(out/'temp')
env['CMAKE_BUILD_PARALLEL_LEVEL'] = '6'
receipt = dict(sha=sha, baseline=baseline, branch=branch, tracked_clean=clean, commands=[])

def save():
    (out/f'{args.stage}-{args.action}.json').write_text(json.dumps(receipt, indent=2), encoding='utf-8')

def run(name, command, cwd=root):
    command = list(map(str, command))
    log = out/f'{args.stage}-{args.action}-{name}.txt'
    start = time.monotonic()
    with log.open('w', encoding='utf-8') as f:
        f.write('command='+json.dumps(command)+'\ncwd='+str(cwd)+'\n'); f.flush()
        result = subprocess.run(command, cwd=cwd, env=env, stdout=f, stderr=subprocess.STDOUT)
    receipt['commands'].append(dict(name=name, command=command, cwd=str(cwd), log=str(log),
                                    exit_code=result.returncode, seconds=time.monotonic()-start))
    save()
    print(name, result.returncode, flush=True)
    assert result.returncode == 0, str(log)
    return log

if args.action == 'build':
    for name, command in [('cmake', [cmake, '--version']),
                          ('compiler', [qt/'Tools/mingw1310_64/bin/g++.exe', '--version']),
                          ('qt', [qt/'6.8.0/mingw_64/bin/qmake.exe', '--version']),
                          ('ninja', [qt/'Tools/Ninja/ninja.exe', '--version'])]:
        run(name, command)
    run('configure-main', [cmake, '--preset', 'mingw-debug', '-DBUILD_MAINWINDOW_START_TEST=ON'], project)
    run('standard-main', ['cmd', '/c', 'build_mingw_debug.cmd'], project)
    run('configure-tests', [cmake, '-S', project/'tests', '-B', tests, '-G', 'Ninja',
        '-DCMAKE_BUILD_TYPE=Debug', '-DCMAKE_CXX_COMPILER='+str(qt/'Tools/mingw1310_64/bin/g++.exe'),
        '-DCMAKE_MAKE_PROGRAM='+str(qt/'Tools/Ninja/ninja.exe'),
        '-DQt6_DIR='+str(qt/'6.8.0/mingw_64/lib/cmake/Qt6'), '-DPython3_EXECUTABLE='+str(python)])
    run('tests', [cmake, '--build', tests, '-j', '6'])
    for name in ['PAimageReceiverDiagnostics.exe', 'ImagingSvc.exe', 'ring_svc_selftest.exe', 'ring_udp_replay.exe']:
        assert (native/name).is_file(), name
elif args.action == 'validate':
    run('list', [ctest, '--test-dir', tests, '-N'])
    log = run('regression', [ctest, '--test-dir', tests, '-V', '--timeout', '120'])
    assert '0 tests failed out of 56' in log.read_text(encoding='utf-8', errors='replace')
    log = run('main-entry', [ctest, '--test-dir', project/'build/mingw_debug', '-V', '-R', '^mainwindow_start_integration_test$'])
    assert '0 tests failed out of 1' in log.read_text(encoding='utf-8', errors='replace')
    # Small synthetic process/SHM smoke test; CW + splice + all DSP combinations
    # are covered separately by the production-service MATLAB/DAS regression.
    run('selftest', [native/'ring_svc_selftest.exe', '--synthetic', '1', '--samples', '5000',
        '--id', '14', '--channels', '3', '--block', '200', '--rounds', '1', '--grid-mm', '.2',
        '--timevar-lowpass', 'on', '--filtered-das', 'on', '--benchmark-blocks', '2',
        '--no-save', '1', '--svc', native/'ImagingSvc.exe', '--out', out/(args.stage+'-selftest')])
else:
    assert args.stage == 'final' and clean
    remote = git('ls-remote', '--heads', 'origin', 'refs/heads/'+branch).split()[0]
    assert remote == sha
    identity = project/'build/mingw_debug/generated/PaimageAcquisition/BuildIdentity.h'
    header = identity.read_text()
    assert f'PAIMAGE_GIT_SHA "{sha}"' in header and 'PAIMAGE_TRACKED_DIRTY false' in header
    assert sha.encode() in (native/'PAimageReceiverDiagnostics.exe').read_bytes()
    for action in ['build', 'validate']:
        record = json.loads((out/f'final-{action}.json').read_text())
        assert record['sha'] == sha and record['tracked_clean'] and record['commands']
        assert all(c['exit_code'] == 0 for c in record['commands'])
    keys = ['pa_recon_core.dll', 'cufft64_12.dll', 'ring_recon_cuda.dll', 'cudart64_12.dll', 'libzmq-v141-mt-4_3_5.dll']
    expected = dict((line.split()[1], line.split()[0]) for line in
        (root/'CODEX_REPORTS/ring-timevar-lowpass-20261016/evidence/validation-runtime-sha256.txt').read_text().splitlines())
    for name in keys: assert digest(native/name) == expected[name], name
    commands = json.loads((project/'build/mingw_debug/compile_commands.json').read_text())
    for command in commands:
        text = command.get('command', ' '.join(command.get('arguments', []))).replace('\\', '/')
        if 'CMakeFiles/MC410T_Receiver.dir' in text or 'CMakeFiles/ImagingSvc.dir' in text:
            assert 'MAINWINDOW_START_TEST_SEAM' not in text and 'RING_TIMEVAR_TEST_SEAM' not in text
    stamp = datetime.datetime.now(datetime.timezone(datetime.timedelta(hours=8))).strftime('%Y%m%d-%H%M%S')
    target = root/'artifacts/build-delivery'/f'{stamp}_{sha[:8]}_lowpass_motor'
    target.mkdir(parents=True, exist_ok=False)
    state = {'PAimageReceiverDiagnostics.ini', 'app_log.txt', 'paimage-diagnostics', 'motor_scan_params.json'}
    shutil.copytree(native, target/'bin', ignore=lambda directory, names: state.intersection(names) if Path(directory)==native else set())
    assert (target/'bin/platforms/qwindows.dll').is_file() and (target/'bin/diagnostic-tools').is_dir()
    (target/'bin/PAimageReceiverDiagnostics.ini').write_text('[MotorScanLinkage]\nEnabled=true\nDirectionInverted=false\nTotalAlinesRule=directionOnly\nPollMs=500\nTriggerLatch=true\n', encoding='utf-8')
    for name in ['PAimageReceiverDiagnostics.exe', 'ImagingSvc.exe', 'ring_svc_selftest.exe', 'ring_udp_replay.exe']:
        assert digest(native/name) == digest(target/'bin'/name)
    shutil.copy2(identity, target/'BuildIdentity.h')
    shutil.copytree(Path(__file__).parent, target/'evidence')
    for path in out.glob('final-*.txt'): shutil.copy2(path, target/path.name)
    for action in ['build', 'validate']: shutil.copy2(out/f'final-{action}.json', target/f'validation-{action}.json')
    (target/'git-receipt.txt').write_text('branch='+branch+'\nlocal='+sha+'\nremote='+remote+'\ntracked_clean=true\nbaseline='+baseline+'\n'+git('log', '-3', '--oneline')+'\n', encoding='utf-8')
    (target/'dependency-sha256.txt').write_text(''.join(digest(native/name)+'  '+name+'\n' for name in keys))
    (target/'build-manifest.txt').write_text('repository=ZiXuannnZhang/PAErealtimeImaging\nbranch='+branch+'\nsha='+sha+
        '\ntracked_clean=true\nQt=6.8.0 MinGW=13.1.0; versions in final-build logs\npresets=mingw-debug / mingw-debug-build\nscript=build_mingw_debug.cmd\nRing CUDA source=_migration_pack/prebuilt_cuda/bin (same runtime hashes as lowpass baseline)\n'+
        'INI=minimal directionOnly motor configuration; no local acquisition/calibration state or stale motor JSON\nnative='+str(native)+'\ndelivery='+str(target)+'\n', encoding='utf-8')
    (target/'validation.txt').write_text('56/56 CTest; real MainWindow integration 1/1; synthetic service selftest PASS.\nHardware acquisition, image direction, motor live bridge, T_arr: UNVERIFIED; user will perform joint hardware acceptance.\n', encoding='utf-8')
    bridge = root/'_migration_pack/_handoff-20261029/sender-side-labview-bridge'
    if bridge.is_dir():
        reference = target/'sender-bridge-reference'; reference.mkdir()
        for name in ['motor_params_bridge.py', 'Start-MotorParamsBridge.cmd', 'bridge_config.txt']:
            shutil.copy2(bridge/name, reference/name)
    (target/'file-sha256.txt').write_text(''.join(digest(f)+'  '+f.relative_to(target).as_posix()+'\n'
        for f in sorted(target.rglob('*')) if f.is_file()), encoding='utf-8')
    receipt.update(delivery=str(target), local=sha, remote=remote,
                   dependencies={name:digest(native/name) for name in keys})
    save()
    print(target)
