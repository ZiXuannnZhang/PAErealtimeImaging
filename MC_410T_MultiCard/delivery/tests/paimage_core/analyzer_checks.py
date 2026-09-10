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
        assert gap_result['fileOrderInversions']==0
        ordered=root/'ordered';fixture(ordered,[(1,0,1444),(1,1,12)])
        raw=(ordered/'trace-0.bin').read_bytes();first=list(m.RECORD.unpack(raw[:64]));second=list(m.RECORD.unpack(raw[64:]));first[0]=2;second[0]=1
        (ordered/'trace-0.bin').write_bytes(m.RECORD.pack(*first)+m.RECORD.pack(*second))
        swapped=m.analyze(ordered,2,18001,181,32,4)
        assert swapped['fileOrderInversions']==1 and swapped['missingRecordCount']==0 and not swapped['traceIncomplete']
        dict_targets=root/'dict-targets';fixture(dict_targets,[(1,0,1444),(1,1,12)])
        (dict_targets/'run-config.json').write_text(json.dumps({'targets':[{'card':0,'ip':'127.0.0.2'}],'cards':4,'dataPort':18001,'samples':181,'bits':32}))
        assert m.analyze(dict_targets,2,18001,181,32,4)['rawCompleteCardTriggers']==1
        rounds=m.analyze(root/'good',2,18001,181,32,4,[{'trialId':'t','roundId':1,'extraTrigger':0,'effectiveCount':1,'basis':'user-confirmed'}])
        card0=rounds['rounds'][0]['perCard'][0]
        assert card0['effectiveComplete']==1 and card0['effectiveCompletelyUnseen']==0 and card0['conservationVerified']
        duplicate_session=root/'duplicate-session'
        fixture(duplicate_session,[(1,0,1444),(1,1,12)])
        raw=(duplicate_session/'trace-0.bin').read_bytes()
        extra=[]
        for offset in (0,64):
            record=list(m.RECORD.unpack(raw[offset:offset+64]));record[0]+=2;record[2]=2;record[3]+=2
            extra.append(m.RECORD.pack(*record))
        (duplicate_session/'trace-0.bin').write_bytes(raw+b''.join(extra))
        (duplicate_session/'trace-summary.json').write_text(json.dumps(dict(recordsIssued=4,traceIncomplete=False)))
        round_spec={'extraTrigger':0,'effectiveCount':1,'basis':'user-confirmed'}
        ambiguous=m.analyze(duplicate_session,2,18001,181,32,4,[round_spec])['rounds'][0]
        assert not ambiguous['perCard'][0]['conservationVerified']
        assert ambiguous['perCard'][0]['effectiveAmbiguous']==1 and ambiguous['classificationStatus']=='candidate/unknown'
        scoped=m.analyze(duplicate_session,2,18001,181,32,4,[dict(round_spec,session=1)])['rounds'][0]
        assert scoped['perCard'][0]['conservationVerified'] and scoped['perCard'][0]['effectiveComplete']==1
        wrap=root/'wrap';fixture(wrap,[(65535,0,1444),(65535,1,12),(0,0,1444),(0,1,12)])
        wrapped=m.analyze(wrap,2,18001,181,32,4,[dict(round_spec,extraTrigger=65535)])['rounds'][0]['perCard'][0]
        assert wrapped['effectiveComplete']==1 and wrapped['extraComplete'] and wrapped['conservationVerified']
    print('PASS independent analyzer fixtures: complete, short tail, reused ID, cross-session exact link, short datagram, missing summary')
if __name__=='__main__':main()
