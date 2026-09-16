"""Build a reviewable local candidate; never publishes or edits another worktree."""
import argparse,hashlib,json,re,shutil,subprocess,zipfile
from pathlib import Path

def digest(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda:f.read(1024*1024),b''):h.update(chunk)
    return h.hexdigest()

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--build',default='build/paimage-delivery-debug');args=parser.parse_args()
    root=Path(__file__).resolve().parents[3];build=(root/args.build).resolve();out=root/'artifacts/PAimageAcquisitionPort'
    if root not in build.parents:raise RuntimeError('build must be in isolated worktree')
    sha=subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip()
    dirty=subprocess.check_output(['git','status','--porcelain','--untracked-files=no'],cwd=root,text=True).strip()
    if dirty:raise RuntimeError('commit tracked sources before packaging')
    identity=(build/'generated/PaimageAcquisition/BuildIdentity.h').read_text()
    if f'#define PAIMAGE_GIT_SHA "{sha}"' not in identity or '#define PAIMAGE_TRACKED_DIRTY false' not in identity:
        raise RuntimeError('reconfigure/rebuild committed sources first')
    out.mkdir(parents=True,exist_ok=True)
    required_bins={'PAimageReceiverDiagnostics.exe','ImagingSvc.exe'}
    built_bins={p.name for p in (build/'bin').glob('*.exe')}
    missing=sorted(required_bins-built_bins)
    if missing:
        raise RuntimeError('incomplete build; missing required executable(s): '+','.join(missing))
    for path in (build/'bin').iterdir():
        if path.name in ('ring_svc_selftest.exe','ring_udp_replay.exe'):continue
        if path.is_dir():shutil.copytree(path,out/path.name,dirs_exist_ok=True)
        elif path.suffix.lower() in ('.dll','.exe'):shutil.copy2(path,out/path.name)
    reports=['PAimage采集行为映射.md','移植差异与限制.md','软件验证报告.md','同机分时测试操作说明.md']
    for name in reports:shutil.copy2(root/'CODEX_REPORTS'/name,out/name)
    toolout=out/'tools';toolout.mkdir(exist_ok=True)
    for name in ('paimage_trace_analyze.py','paimage-trace-schema.md'):shutil.copy2(Path(__file__).parent/name,toolout/name)
    examples=out/'examples';examples.mkdir(exist_ok=True)
    samples={'normal-70':('636e29a6-2b24-4649-85e4-bdb50d7fd941','production-compare-analysis.json'),
             'missing-middle-28':('303c1c23-76bc-46c9-9bf5-81a8952ed864','production-missing-analysis.json')}
    for label,(run,analysis) in samples.items():
        trace=root/'build/paimage-host-checks/paimage-traces'/run
        if not trace.exists() or not (root/'build'/analysis).exists():
            continue
        source=list(trace.glob('trace-*.bin'))+[trace/'run-config.json',trace/'trace-summary.json']
        manifest={'exampleKind':'offline replay snapshot; not GUI export or hardware evidence','runId':run,'files':[]}
        with zipfile.ZipFile(examples/(label+'.zip'),'w',zipfile.ZIP_DEFLATED,compresslevel=1) as z:
            for path in source:
                name='paimage/'+run+'/'+path.name;z.write(path,name);manifest['files'].append({'path':name,'sha256':digest(path)})
            z.write(root/'build'/analysis,'analysis.json')
            for name in ('paimage_trace_analyze.py','paimage-trace-schema.md'):z.write(toolout/name,'tools/'+name)
            z.writestr('manifest.json',json.dumps(manifest,ensure_ascii=False,indent=2))
    objdump=Path('D:/Qt/Qt6.8.0/Tools/mingw1310_64/bin/objdump.exe')
    bundled={p.name.lower() for p in out.rglob('*') if p.is_file()}
    imports=[];missing=[]
    for path in out.rglob('*'):
        if path.suffix.lower() not in ('.dll','.exe'):continue
        report=subprocess.check_output([str(objdump),'-p',str(path)],text=True,errors='replace')
        for name in re.findall(r'DLL Name:\s*(\S+)',report):
            status='bundled' if name.lower() in bundled else 'system' if (Path('C:/Windows/System32')/name).exists() or name.lower().startswith(('api-ms-','ext-ms-')) else 'unresolved'
            imports.append({'file':str(path.relative_to(out)),'import':name,'resolution':status})
            if status=='unresolved':missing.append(name)
    if missing:raise RuntimeError('unresolved import(s): '+','.join(sorted(set(missing))))
    manifest={'sourceCommit':sha,'trackedDirty':False,'backendId':'paimage-derived','behaviorMappingVersion':'production-3',
        'productBaseline':'e66a29bfbbaa6534911a23d48e8624fb8d552bec','sourcePaimageSha256':'2b2f4a8b82ff49fc35c2d3d96fe3f999a0dcc403144f39ef520c5af5cea9a6ec',
        'sourceInstallerSha256':'b33953b0a74966cb2d6a6db8deda1977aab39cb2eea1df8e7ab05cb42c3c4d2e',
        'buildType':'Debug','compiler':'GNU 13.1.0 MinGW x64','qt':'6.8.0','buildDirectory':str(build),
        'acceptance':'candidate; GUI acceptance pending unlocked desktop; physical NIC experiment not performed',
        'dependencyCheck':'static PE imports; system presence is not a runtime/GPU compatibility test','imports':imports,'files':[]}
    for path in sorted(out.rglob('*')):
        if path.is_file() and path.name!='build-manifest.json':manifest['files'].append({'path':path.relative_to(out).as_posix(),'bytes':path.stat().st_size,'sha256':digest(path)})
    (out/'build-manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps({'package':str(out),'sourceCommit':sha,'files':len(manifest['files']),'unresolvedImports':missing},ensure_ascii=False))
if __name__=='__main__':main()

