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
        unknown=m.analyze(root/'tail',2,18001,None,32,4)
        assert unknown['anomalies'][0]['validUnique']==1 and unknown['anomalies'][0]['missingValidSlots']==[1]
        assert not unknown['anomalies'][0]['tailPayloadVerified']
        repaired=fixture(root/'repaired',[(1,0,4),(1,0,1444),(1,1,4),(1,1,12)])
        assert repaired['rawCompleteCardTriggers']==1
        g=repaired['anomalies'][0]
        assert g['unique']==2 and g['validUnique']==2 and not g['missingValidSlots']
        assert g['shortNonTailPayloads']==1 and g['shortTailPayloads']==1 and g['duplicates']==2
        round_result=m.analyze(root/'repaired',2,18001,181,32,4,[{'extraTrigger':0,'effectiveCount':1,'basis':'user-confirmed'}])
        assert round_result['rounds'][0]['perCard'][0]['effectiveComplete']==1
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
        loop = root / 'loop'
        loop.mkdir()
        summary = loop / 'looplog-summary.json'
        summary.write_text(json.dumps(dict(mode='rolling-burst-windows', recordsIssued=80,
                                           recordsWritten=2, loopLogIncomplete=False)))
        records = [m.LOOPLOG.pack(seq, ts, 0, 0, 0, kind, 0, 0, 0, 0, 0, 0, 0, 0)
                   for seq, ts, kind in [(20, 1000000, 1), (25, 5001000000, 6)]]
        loop_file = loop / 'looplog-0.bin'
        loop_file.write_bytes(b''.join(records))
        complete = m._looplog(loop)
        assert complete['missingRecordCount'] == 0 and not complete['loopLogIncomplete']
        loop_file.write_bytes(records[0])
        missing = m._looplog(loop)
        assert missing['missingRecordCount'] == 1 and missing['loopLogIncomplete']
        summary.write_text(json.dumps(dict(mode='rolling-burst-windows', recordsIssued=80,
                                           recordsWritten=1.0, loopLogIncomplete=False,
                                           windows=[dict(postWindowTruncated=True)])))
        truncated = m._looplog(loop)
        assert truncated['missingRecordCount'] == 0 and truncated['loopLogIncomplete']
        summary.write_text(json.dumps(dict(recordsIssued=3, loopLogIncomplete=False)))
        legacy = m._looplog(loop)
        assert legacy['missingRecordCount'] == 19 and legacy['loopLogIncomplete']

        loop_file.unlink()
        ns = 1_000_000_000
        windows, exports = [], []
        for index, (epoch, start, burst, last) in enumerate([(1, 5, 10, 15), (2, 25, 30, 31), (3, 33, 38, 43)]):
            file = f'looplog-{index}.bin'
            source = f'looplog-window-{epoch}.bin'
            windows.append(dict(windowId=epoch, startNs=start*ns, endNs=(burst+5)*ns,
                                file=source, recordCount=3, burstEpochs=[epoch],
                                preWindowTruncated=epoch == 1, postWindowTruncated=epoch == 2))
            exports.append(dict(windowId=epoch, sourceFile=source, exportFile=f'paimage/loop/{file}',
                                exportRecords=3, inRequestedWindow=True))
            (loop/file).write_bytes(b''.join(m.LOOPLOG.pack(epoch*10+i, ts*ns, 0, 0, 0, kind, 0,
                                                          epoch, 0, 0, 0, 0, 0, 0)
                                             for i, (ts, kind) in enumerate([(start, 1), (burst, 6), (last, 1)])))
        saved = dict(mode='rolling-burst-windows', recordsWritten=9, loopLogIncomplete=True,
                     windows=windows, exportWindows=exports)
        summary.write_text(json.dumps(saved))
        scoped = m._looplog(loop)
        assert next(w for w in scoped['burstWindows'] if w['epoch'] == 2)['following5sCovered'] is False
        assert next(w for w in scoped['burstWindows'] if w['epoch'] == 3)['following5sCovered'] is True
        exports[1]['exportFile'] = 'paimage/another/looplog-1.bin'
        summary.write_text(json.dumps(saved))
        unknown = next(w for w in m._looplog(loop)['burstWindows'] if w['epoch'] == 2)
        assert unknown['prior5sCovered'] is None and unknown['following5sCovered'] is None
        for i in (0, 1):
            (loop/f'looplog-{i}.bin').unlink()
            exports[i].update(inRequestedWindow=False, exportFile='', exportRecords=0)
        summary.write_text(json.dumps(dict(saved, recordsWritten=3, loopLogIncomplete=False)))
        assert not m._looplog(loop)['loopLogIncomplete']
        (loop/'looplog-2.bin').rename(loop/'looplog-window-3.bin')
        saved.pop('exportWindows')
        summary.write_text(json.dumps(dict(saved, windows=windows[2:], recordsWritten=3, loopLogIncomplete=False)))
        raw = m._looplog(loop)
        assert raw['burstWindows'][0]['windowId'] == 3 and raw['burstWindows'][0]['following5sCovered'] is True
    print('PASS independent trace and LoopLog analyzer fixtures')
if __name__=='__main__':main()
