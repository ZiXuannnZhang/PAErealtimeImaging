"""Independent fixture records; no source parser used to generate expectations."""
import importlib.util,json,sys,tempfile
from pathlib import Path
sys.dont_write_bytecode=True
tool=Path(__file__).resolve().parents[2]/'tools/paimage_trace_analyze.py'
spec=importlib.util.spec_from_file_location('trace_analyzer',tool)
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)

def fixture(root,packets,summary=True,schema=1):
    root.mkdir(parents=True,exist_ok=True)
    data=[]
    for i,(trigger,packet,length) in enumerate(packets,1):
        header=packet.to_bytes(2,'little')+trigger.to_bytes(2,'little')
        data.append(m.RECORD.pack(i,i*1000,1,i,1,0,0,18001,8000,length,trigger,packet,-1,1,0,header,schema))
    (root/'trace-0.bin').write_bytes(b''.join(data))
    if summary:(root/'trace-summary.json').write_text(json.dumps(dict(recordsIssued=len(data),traceIncomplete=False)))
    return m.analyze(root,2,18001,181,32,4)

def main():
    base=Path(sys.argv[1]);base.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='analyzer-check-',dir=base) as d:
        root=Path(d)
        good=fixture(root/'good',[(1,0,1444),(1,1,12)])
        assert good['rawCompleteCardTriggers']==1 and not good['traceIncomplete']
        tail=fixture(root/'tail',[(1,0,1444),(1,1,4)])
        assert tail['rawCompleteCardTriggers']==0 and tail['anomalies'][0]['shortTailPayloads']==1
        reused=fixture(root/'reused',[(1,0,1444),(2,0,1444),(1,1,12)])
        assert reused['rawCompleteCardTriggers']==0 and reused['rawOccurrenceCount']==3
        assert reused['repeatedTriggerIdentities']==1 and not reused['associationComplete']
        exact=root/'exact'
        fixture(exact,[(1,0,1444),(2,0,1444),(1,1,12)],schema=2)
        with (exact/'trace-0.bin').open('ab') as f:
            f.write(m.RECORD.pack(4,4000,2,1,2,0,0,0,0,0,1,0,0,4,0,bytes(4),2))
        (exact/'trace-summary.json').write_text(json.dumps(dict(recordsIssued=4,traceIncomplete=False)))
        linked=m.analyze(exact,2,18001,181,32,4)
        assert linked['associationComplete'] and linked['repeatedTriggerIdentities']==1
        old=next(g for g in linked['anomalies'] if g['trigger']==1 and g['occurrence']==1)
        later=next(g for g in linked['anomalies'] if g['trigger']==1 and g['occurrence']==2)
        assert old['adapterOutput']==1 and old['deliverySessions']==[2] and later['adapterOutput']==0
        with (exact/'trace-0.bin').open('ab') as f:
            f.write(m.RECORD.pack(5,5000,2,1,2,0,6,0,0,0,1,0,0,7,0,bytes(4),2))
        (exact/'trace-summary.json').write_text(json.dumps(dict(recordsIssued=5,traceIncomplete=False)))
        host=m.analyze(exact,2,18001,181,32,4)
        assert host['hostDeliveryRecords']==1 and host['associationComplete']
        old=next(g for g in host['anomalies'] if g['trigger']==1 and g['occurrence']==1)
        assert old['hostDelivery']=={'saveConsumer:6':1}
        short=fixture(root/'short',[(1,0,2)])
        assert short['shortDataIngress']==1 and not short['traceIncomplete']
        incomplete=fixture(root/'no-summary',[(1,0,1444)],False)
        assert incomplete['traceIncomplete']
        gaps=root/'gaps';fixture(gaps,[(1,0,1444),(1,1,12)])
        records=(gaps/'trace-0.bin').read_bytes();second=list(m.RECORD.unpack(records[64:]));second[0]=3
        (gaps/'trace-0.bin').write_bytes(records[:64]+m.RECORD.pack(*second))
        (gaps/'trace-summary.json').write_text(json.dumps(dict(recordsIssued=5,traceIncomplete=False)))
        gap_result=m.analyze(gaps,2,18001,181,32,4)
        assert gap_result['traceIncomplete'] and gap_result['missingRecordRanges']==[[2,2],[4,5]]
    print('PASS independent analyzer fixtures: complete, short tail, reused ID, cross-session exact link, short datagram, missing summary')
if __name__=='__main__':main()
