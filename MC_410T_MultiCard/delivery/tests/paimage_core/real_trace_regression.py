"""Read-only regression of the user-supplied September 10 trace."""
import importlib.util,json,sys
from pathlib import Path
sys.dont_write_bytecode=True
delivery=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('analyzer',delivery/'tools/paimage_trace_analyze.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
rounds=[dict(roundId=i+1,extraTrigger=e,effectiveCount=4000,basis='user-confirmed') for i,e in enumerate([4,4005,8006])]
r=m.analyze(Path(sys.argv[1]),35,8001,6250,rounds=rounds)
assert r['records']==3706288 and r['missingRecordCount']==0 and not r['traceIncomplete']
assert r['rawOccurrenceCount']==47028 and r['rawCompleteCardTriggers']==46904 and r['rawPartialCardTriggers']==124
for i,c in enumerate([563,568,562,564]):
    assert sum(len(g['missingRelativeSlots']) for g in r['anomalies'] if g['card']==i)==c
for rr,observed,partial,unseen in zip(r['rounds'],[3917,3924,3911],[2,6,19],[83,76,89]):
    for c in rr['perCard']:
        assert c['effectiveObserved']==observed and c['effectivePartial']==partial and c['effectiveCompletelyUnseen']==unseen and c['conservationVerified']
Path(sys.argv[2]).write_text(json.dumps({k:r[k] for k in ['records','missingRecordCount','fileOrderInversions','rawOccurrenceCount','rawCompleteCardTriggers','rawPartialCardTriggers','rounds']},ensure_ascii=False,indent=2),encoding='utf-8')
print('PASS real trace and confirmed rounds')
