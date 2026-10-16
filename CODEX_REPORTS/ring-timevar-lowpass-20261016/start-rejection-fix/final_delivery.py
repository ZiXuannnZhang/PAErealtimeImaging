"""Validate/package the exact committed startup fix, preserving original evidence."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

p = argparse.ArgumentParser()
p.add_argument('mode', choices=['validate', 'package'])
p.add_argument('--build-log')
a = p.parse_args()
root = Path(__file__).resolve().parents[3]
out = root / 'artifacts/ring-timevar-lowpass-start-fix'
out.mkdir(parents=True, exist_ok=True)
project = root / 'MC_410T_MultiCard/delivery'
native = project / 'build/mingw_debug/bin'
old = root / 'CODEX_REPORTS/ring-timevar-lowpass-20261016/evidence'
git = lambda *args: subprocess.check_output(['git', *args], cwd=root, encoding='utf-8').strip()
sha, branch = git('rev-parse', 'HEAD'), git('branch', '--show-current')
assert branch == 'codex/ring-timevar-lowpass-20261016'
assert not git('status', '--porcelain', '--untracked-files=no'), 'dirty tracked tree'
header = (project / 'build/mingw_debug/generated/PaimageAcquisition/BuildIdentity.h').read_text()
assert f'PAIMAGE_GIT_SHA "{sha}"' in header and 'PAIMAGE_TRACKED_DIRTY false' in header
assert sha.encode() in (native / 'PAimageReceiverDiagnostics.exe').read_bytes()
build_receipt = json.loads((out / 'final-build-receipt.json').read_text())
assert build_receipt['sha'] == sha and build_receipt['exit_code'] == 0

def digest(path):
    with open(path, 'rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()

keys = ['pa_recon_core.dll', 'cufft64_12.dll', 'ring_recon_cuda.dll',
        'cudart64_12.dll', 'libzmq-v141-mt-4_3_5.dll']
expected = dict((line.split()[1], line.split()[0]) for line in
                (old / 'validation-runtime-sha256.txt').read_text().splitlines())
for name in keys:
    assert digest(native / name) == expected[name], f'runtime changed: {name}'
previous_exes = dict((line.split()[1], line.split()[0]) for line in
                     (old / 'validation-executable-sha256.txt').read_text().splitlines())
for name in ['ImagingSvc.exe', 'ring_svc_selftest.exe', 'ring_udp_replay.exe']:
    assert digest(native / name) == previous_exes[name], f'service/tool changed: {name}'
assert not git('diff', '18b318236bdca26574304179b84e5d77d1f490c0', 'HEAD', '--',
               'MC_410T_MultiCard/delivery/src/ImagingSvc',
               'MC_410T_MultiCard/delivery/src/RingRecon',
               'MC_410T_MultiCard/delivery/src/RingTimevarLowpass.cpp',
               'MC_410T_MultiCard/delivery/src/RingTimevarLowpassSettings.cpp',
               'MC_410T_MultiCard/delivery/src/RingBlockAssembler.cpp'), 'numerical/service changes'

if a.mode == 'validate':
    env = os.environ.copy()
    env['PATH'] = ';'.join([str(native), 'D:/Qt/Qt6.8.0/6.8.0/mingw_64/bin',
                            'D:/Qt/Qt6.8.0/Tools/mingw1310_64/bin', env['PATH']])
    env['QT_QPA_PLATFORM'] = 'offscreen'
    env['QT_PLUGIN_PATH'] = 'D:/Qt/Qt6.8.0/6.8.0/mingw_64/plugins'
    (out / 'temp').mkdir(exist_ok=True)
    env['TEMP'] = env['TMP'] = str(out / 'temp')
    receipt = {'sha': sha, 'branch': branch, 'tracked_clean': True, 'commands': [],
               'runtime_sha256': {name: digest(native / name) for name in keys},
               'executable_sha256': {f.name: digest(f) for f in native.glob('*.exe')}}

    def run(name, command, cwd=root):
        command = list(map(str, command))
        begin = time.monotonic()
        with open(out / f'final-{name}.txt', 'w', encoding='utf-8') as log:
            log.write('command=' + json.dumps(command, ensure_ascii=False) + '\n')
            log.write('cwd=' + str(cwd) + '\n')
            log.flush()
            result = subprocess.run(command, cwd=cwd, env=env, stdout=log, stderr=subprocess.STDOUT)
        receipt['commands'].append({'name': name, 'command': command, 'cwd': str(cwd),
                                    'exit_code': result.returncode, 'seconds': time.monotonic() - begin})
        (out / 'final-validation.json').write_text(json.dumps(receipt, indent=2), encoding='utf-8')
        print(name, 'exit=', result.returncode, flush=True)
        assert result.returncode == 0, name

    ctest = 'D:/Qt/Qt6.8.0/Tools/CMake_64/bin/ctest.exe'
    run('integration', [ctest, '--test-dir', project / 'build/mingw_debug', '-V',
                        '-R', '^mainwindow_start_integration_test$'])
    assert '0 tests failed out of 1' in (out / 'final-integration.txt').read_text(errors='replace')
    run('regression-51', [ctest, '--test-dir', project / 'build/timevar_tests', '-V'])
    assert '0 tests failed out of 51' in (out / 'final-regression-51.txt').read_text(errors='replace')
    common = [native / 'ring_svc_selftest.exe', '--synthetic', '1', '--samples', '5000',
              '--id', '14', '--channels', '15', '--block', '200', '--grid-mm', '0.1',
              '--svc', native / 'ImagingSvc.exe']
    run('off-replay', common + ['--rounds', '1', '--timevar-lowpass', 'off', '--out', out / 'off-replay'])
    baseline = root / 'artifacts/ring-timevar-lowpass-20261016/off-compare/baseline'
    comparisons = []
    for file in sorted(baseline.glob('*.raw')):
        candidate = out / 'off-replay' / file.name
        assert digest(file) == digest(candidate), file.name
        comparisons.append({'name': file.name, 'baseline': digest(file), 'final': digest(candidate)})
    assert len(comparisons) == 20
    receipt['off_bitwise_files'] = comparisons
    for name, flags in [('identity-barrier', ['--identity-jump-at', '1']),
                        ('incomplete-round', ['--drop-block', '1']),
                        ('timeout-round', ['--timeout-reset', '2'])]:
        run(name, common + ['--rounds', '2', '--timevar-lowpass', 'on', '--no-save', '1',
                            '--out', out / name] + flags)
    receipt['passed'] = True
    (out / 'final-validation.json').write_text(json.dumps(receipt, indent=2), encoding='utf-8')
    (out / 'BuildIdentity.h.txt').write_text(header, encoding='utf-8')
else:
    assert a.build_log, '--build-log required'
    receipt = json.loads((out / 'final-validation.json').read_text())
    assert receipt['sha'] == sha and receipt['passed'] and len(receipt['commands']) == 6
    assert all(item['exit_code'] == 0 for item in receipt['commands'])
    for name, value in receipt['executable_sha256'].items():
        assert digest(native / name) == value, name
    remote = git('ls-remote', 'origin', 'refs/heads/' + branch).split()[0]
    assert remote == sha, 'local/remote mismatch'
    for name in ['platforms/qwindows.dll', 'diagnostic-tools/paimage_trace_analyze.py',
                 'PAimageReceiverDiagnostics.exe', 'ImagingSvc.exe',
                 'ring_svc_selftest.exe', 'ring_udp_replay.exe']:
        assert (native / name).is_file(), name
    now = datetime.datetime.now(datetime.timezone(datetime.timedelta(hours=8)))
    delivery = root / 'artifacts/build-delivery' / f'{now:%Y%m%d-%H%M%S}_{sha[:7]}'
    delivery.mkdir(parents=True, exist_ok=False)
    shutil.copytree(native, delivery / 'bin')
    manifest = dict(repository='ZiXuannnZhang/PAErealtimeImaging', branch=branch, commit=sha,
        reviewed_parent='18b318236bdca26574304179b84e5d77d1f490c0', tracked_tree_clean=True,
        build_time=now.isoformat(), machine=os.environ.get('COMPUTERNAME'),
        configure_preset='mingw-debug', build_preset='mingw-debug-build',
        build_script='MC_410T_MultiCard/delivery/build_mingw_debug.cmd', build_type='Debug',
        USE_ZEROMQ=True, Qt='6.8.0 mingw_64', compiler='GNU 13.1.0', CMake='3.30.5', Ninja='1.12.1',
        toolchain_root='D:/Qt/Qt6.8.0',
        Ring_CUDA_source='_migration_pack/prebuilt_cuda (unchanged, no CUDA rebuild)',
        Ring_CUDA_path=str(root / '_migration_pack/prebuilt_cuda/bin'),
        cudart='CUDA toolkit v12.8; unchanged SHA from original task validation',
        cufft='accepted delivery/libs/imaging/cufft64_12.dll, NVIDIA cuFFT 12.1.0.31, unchanged',
        ZeroMQ=str(project / 'third_party/zeromq') + ' (4.3.5)',
        linear_imaging=str(project / 'libs/imaging/pa_recon_core.dll'),
        native_bin=str(native), delivery_path=str(delivery), zip_created=False,
        integration_test='BUILD_MAINWINDOW_START_TEST=ON, separate startup-isolated target; production has no test seam',
        hardware_visual_T_arr='UNVERIFIED', performance='original evidence retained; not remeasured for UI-only fix')
    (delivery / 'build-manifest.txt').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    (delivery / 'git-receipt.txt').write_text(
        f'branch={branch}\nlocal_HEAD={sha}\nremote_HEAD={remote}\ntracked_clean=true\n'
        + git('log', '-1', '--format=fuller') + '\n' + header, encoding='utf-8')
    (delivery / 'dependency-sha256.txt').write_text(''.join(
        digest(delivery / 'bin' / name) + '  bin/' + name + '\n' for name in keys), encoding='utf-8')
    shutil.copy2(a.build_log, delivery / 'build.log')
    shutil.copy2(out / 'final-validation.json', delivery / 'validation.txt')
    (delivery / 'BuildIdentity.h.txt').write_text(header, encoding='utf-8')
    shutil.copytree(out, delivery / 'repair-evidence', ignore=shutil.ignore_patterns('temp', '*.raw'))
    files = sorted(file for file in delivery.rglob('*') if file.is_file())
    (delivery / 'file-sha256.txt').write_text(''.join(
        digest(file) + '  ' + file.relative_to(delivery).as_posix() + '\n' for file in files), encoding='utf-8')
    for line in (delivery / 'file-sha256.txt').read_text().splitlines():
        value, name = line.split('  ', 1)
        assert digest(delivery / name) == value, name
    (out / 'final-delivery.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    print(delivery)
