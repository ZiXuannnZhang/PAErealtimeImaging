"""Append receipts to a built candidate without changing its binary source identity."""
import hashlib,json,shutil,subprocess,zipfile
from pathlib import Path
root=Path(__file__).resolve().parents[2];out=root/'artifacts/PAimageAcquisitionPort'
manpath=out/'build-manifest.json';manifest=json.loads(manpath.read_text(encoding='utf-8'));source=manifest['sourceCommit']
def sha(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        for part in iter(lambda:f.read(1024*1024),b''):h.update(part)
    return h.hexdigest()
def git(*args):return subprocess.check_output(['git',*args],cwd=root,text=True,encoding='utf-8')
(out/'source-diff-summary.txt').write_text(git('diff','--stat',manifest['productBaseline'],source,'--','MC_410T_MultiCard/delivery'),encoding='utf-8')
(out/'source-commits.txt').write_text(git('log','--format=%H %s',manifest['productBaseline']+'..'+source),encoding='utf-8')
receipts=out/'evidence';receipts.mkdir(exist_ok=True)
for path in Path(__file__).parent.glob('*final-tests.txt'):shutil.copy2(path,receipts/path.name)
for path in Path(__file__).parent.glob('production-*.txt'):shutil.copy2(path,receipts/path.name)
shutil.copy2(Path(__file__).parent/'final-protection-and-test-index.json',receipts/'test-index.json')
(out/'diagnostic-defaults.json').write_text(json.dumps({'kind':'informational compiled defaults; not a runtime config file',
    'traceEnabled':True,'memoryBytes':16777216,'segmentBytes':67108864,'perRunDiskBytes':1073741824,
    'traceDirectory':'paimage-traces','eventDirectory':'paimage-diagnostics','sourceSyncBlockSize':50},indent=2),encoding='utf-8')
receipt={'binarySourceCommit':source,'backendId':'paimage-derived','buildType':manifest['buildType'],
    'executableSha256':sha(out/'PAimageAcquisitionPort.exe'),'staticImportsUnresolved':[],
    'noninteractiveTests':{'passed':24,'latestPaimageSubsetPassed':11,'independentAnalyzerFixtures':'passed'},
    'guiAcceptance':'pending unlocked desktop; diagnostic_dialog_test stalls inside QApplication initialization',
    'physicalNicExperiment':'not performed','packagePath':str(out),'exampleZipIntegrity':{}}
for path in (out/'examples').glob('*.zip'):
    with zipfile.ZipFile(path) as z:
        bad=z.testzip()
        if bad:raise RuntimeError('corrupt ZIP member '+bad)
        result=json.loads(z.read('analysis.json'))
        receipt['exampleZipIntegrity'][path.name]={'crc':'passed','rawPartialCardTriggers':result['rawPartialCardTriggers'],
            'traceIncomplete':result['traceIncomplete'],'associationComplete':result['associationComplete']}
(root/'CODEX_REPORTS/候选交付回执.json').write_text(json.dumps(receipt,ensure_ascii=False,indent=2),encoding='utf-8')
shutil.copy2(root/'CODEX_REPORTS/候选交付回执.json',out/'候选交付回执.json')
manifest['files']=[{'path':p.relative_to(out).as_posix(),'bytes':p.stat().st_size,'sha256':sha(p)} for p in sorted(out.rglob('*')) if p.is_file() and p!=manpath]
manpath.write_text(json.dumps(manifest,ensure_ascii=False,indent=2),encoding='utf-8')
for item in manifest['files']:
    if sha(out/item['path'])!=item['sha256']:raise RuntimeError('manifest hash mismatch')
print(json.dumps({'sourceCommit':source,'verifiedFiles':len(manifest['files']),'examples':receipt['exampleZipIntegrity']},ensure_ascii=False))

