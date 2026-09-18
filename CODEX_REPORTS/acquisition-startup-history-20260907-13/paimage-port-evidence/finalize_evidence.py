"""Read-only protection checks and compact evidence receipts inside this worktree."""
import hashlib,json,subprocess
from pathlib import Path
root=Path(__file__).resolve().parents[2]
evidence=Path(__file__).parent

def git(*args,cwd=root):return subprocess.check_output(['git',*args],cwd=cwd,text=True,encoding='utf-8').strip()
def hashfile(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        for part in iter(lambda:f.read(1024*1024),b''):h.update(part)
    return h.hexdigest()
refs=dict(line.split(' ',1) for line in git('for-each-ref','--format=%(refname) %(objectname)','refs/heads').splitlines())
old=dict(line.split(' ',1) for line in (evidence/'original-refs.txt').read_text(encoding='utf-8-sig').splitlines() if line)
changes={name:{'before':sha,'after':refs.get(name)} for name,sha in old.items() if name!='refs/heads/codex/paimage-acquisition-port-20260909' and refs.get(name)!=sha}
current=git('status','--short','--untracked-files=no',cwd=root.parent)
previous=(evidence/'original-root-status.txt').read_text(encoding='utf-8-sig').strip()
report={'originalBranchRefChanges':changes,'rootTrackedStatusMatchesRecorded':current==previous,
        'scope':'Only ref/status comparison; no complete pre-task per-file content hash snapshot exists.',
        'currentSourceHead':git('rev-parse','HEAD'),'evidence':[]}
for name in ['production-28-600s.log','production-70-600s.log','production-compare-off.log','production-compare-on.log','production-missing-middle.log','noninteractive-final-tests.log','paimage-final-tests.log']:
    path=root/'build'/name
    if path.exists():
        report['evidence'].append({'path':path.relative_to(root).as_posix(),'bytes':path.stat().st_size,'sha256':hashfile(path)})
        (evidence/(path.stem+'.txt')).write_bytes(path.read_bytes())
for run in ['b49e0acf-cf46-4803-9aa5-a8dd74a11a01','490b72f9-b92f-48e5-95c8-44f253da9625','636e29a6-2b24-4649-85e4-bdb50d7fd941','303c1c23-76bc-46c9-9bf5-81a8952ed864']:
    path=root/'build/paimage-host-checks/paimage-traces'/run/'trace-summary.json'
    report['evidence'].append({'runId':run,'summary':json.loads(path.read_text()),'summarySha256':hashfile(path)})
(evidence/'final-protection-and-test-index.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
print(json.dumps({'originalBranchRefChanges':changes,'rootTrackedStatusMatchesRecorded':current==previous,'evidence':len(report['evidence'])}))
if changes:raise SystemExit('unexpected protected branch ref change')
