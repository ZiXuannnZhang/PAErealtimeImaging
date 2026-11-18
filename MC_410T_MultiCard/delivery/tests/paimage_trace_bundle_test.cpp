#include "ZipTestPayload.h"
#include "DiagnosticStorage.h"
#include "PaimageAcquisition/TimingWriter.h"
#include "PaimageAcquisition/TraceBundle.h"
#include "PaimageAcquisition/TraceWriter.h"
#include "PaimageAcquisition/LoopLog.h"
#include "DiagnosticRecorder.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTimer>
#include <cstring>
#include <iostream>
#include <stdexcept>
void checked(bool b,int line){if(!b)throw std::runtime_error("fixed trace export boundary line "+std::to_string(line));}
#define require(b) checked((b),__LINE__)
void write(const QString& path,const QByteArray& data){QFile f(path);require(f.open(QIODevice::WriteOnly));require(f.write(data)==data.size());}
quint32 number(const QByteArray& b,int at,int n){quint32 value=0;std::memcpy(&value,b.constData()+at,n);return value;}
QByteArray readFile(const QString& path){QFile f(path);require(f.open(QIODevice::ReadOnly));return f.readAll();}
QHash<QString,QByteArray> zipEntries(const QString& path){
    const auto bytes=readFile(path);QHash<QString,QByteArray> entries;
    for(int at=0;at+30<=bytes.size()&&number(bytes,at,4)==0x04034b50;){
        int size=number(bytes,at+18,4),name=number(bytes,at+26,2),extra=number(bytes,at+28,2),data=at+30+name+extra;
        require(data+size<=bytes.size());QByteArray payload;
        require(decodeZipPayload(bytes.mid(data,size),number(bytes,at+8,2),number(bytes,at+22,4),payload));
        entries.insert(QString::fromUtf8(bytes.mid(at+30,name)),payload);at=data+size;
    }
    return entries;
}
template<typename Record>
QByteArray recordBytes(const Record& record){return QByteArray(reinterpret_cast<const char*>(&record),sizeof(record));}
int main(int argc,char** argv){QCoreApplication app(argc,argv);
    QTemporaryDir root(QDir::currentPath()+"/trace-export-XXXXXX");require(root.isValid());
    const QString traces=root.path()+"/traces",run=traces+"/run1",tools=root.path()+"/tools";
    QDir().mkpath(run);QDir().mkpath(tools);write(tools+"/paimage_trace_analyze.py","fixture tool");write(tools+"/paimage-trace-schema.md","fixture schema");write(tools+"/startup-looplog-schema.md","fixture loop schema");write(tools+"/startup_diagnostics_analyze.py","fixture startup analyzer");
    const qint64 wall=QDateTime::currentMSecsSinceEpoch()-100;
    write(run+"/run-config.json",QJsonDocument(QJsonObject{{"runId","run1"},{"wallAnchorMs",double(wall)},{"monotonicAnchorNs","1000000"}}).toJson());
    paimage::TraceWriter writer(std::filesystem::path(run.toStdWString()));
    paimage::LoopLog loopLog(std::filesystem::path(run.toStdWString()));
    paimage::TraceRecord first;first.stage=1;first.monotonicNs=1000000;first.correlation=101;writer.push(first);
    paimage::LoopRecord firstLoop;firstLoop.kind=std::uint16_t(paimage::LoopKind::Loop);firstLoop.timeNs=1000000;loopLog.push(firstLoop);
    paimage::LoopRecord burst=firstLoop;burst.kind=std::uint16_t(paimage::LoopKind::BurstMark);loopLog.push(burst);
    const QString run2=traces+"/run2";QDir().mkpath(run2);
    write(run2+"/run-config.json",QJsonDocument(QJsonObject{{"runId","run2"},{"wallAnchorMs",double(wall)},{"monotonicAnchorNs","1000000"}}).toJson());
    {paimage::TraceWriter stopped(std::filesystem::path(run2.toStdWString()));stopped.push(first);stopped.stop();}
    const QString timingOnly=traces+"/timing-only";QDir().mkpath(timingOnly);
    write(timingOnly+"/run-config.json",QJsonDocument(QJsonObject{
        {"runId","timing-only"},{"wallAnchorMs",double(wall)},{"monotonicAnchorNs","4000000000"},
        {"diagnosticTraceEnabled",false},{"timingEnabled",true}}).toJson());
    paimage::TimingWriter timer(std::filesystem::path(timingOnly.toStdWString()));
    paimage::TimingRecord t;t.kind=1;t.startNs=t.endNs=1000000000;t.correlation=10;timer.observe(t,true);
    t.startNs=t.endNs=4000000000;t.correlation=20;timer.observe(t,true);
    // A complete record plus a damaged tail must survive boundary coverage checks.
    auto fixture=[&](const QString& id,const QString& stream,const QByteArray& bytes,bool traceEnabled){
        const QString path=traces+"/"+id;QDir().mkpath(path);
        write(path+"/run-config.json",QJsonDocument(QJsonObject{{"runId",id},{"wallAnchorMs",double(wall)},
            {"monotonicAnchorNs","1000000"},{"diagnosticTraceEnabled",traceEnabled}}).toJson());
        write(path+"/"+stream+"-0.bin",bytes);
        write(path+"/"+stream+"-summary.json",QJsonDocument(QJsonObject{{"recordsIssued",1}}).toJson());
    };
    paimage::TraceRecord tr{};tr.sequence=1;tr.monotonicNs=1000000;
    paimage::TimingRecord ti{};ti.sequence=1;ti.startNs=ti.endNs=1000000;
    paimage::LoopRecord lo{};lo.sequence=1;lo.timeNs=1000000;
    auto damaged=[](const auto& record){return QByteArray(reinterpret_cast<const char*>(&record),sizeof(record))+QByteArray(1,'x');};
    fixture("bad-trace","trace",damaged(tr),true);
    auto outside=tr;outside.sequence=2;outside.monotonicNs=1000000+5000000000ULL;
    fixture("clipped-trace","trace",QByteArray(reinterpret_cast<const char*>(&tr),sizeof(tr))+
        QByteArray(reinterpret_cast<const char*>(&outside),sizeof(outside)),true);
    write(traces+"/clipped-trace/trace-summary.json",QJsonDocument(QJsonObject{{"recordsIssued",2}}).toJson());
    fixture("bad-timing","timing",damaged(ti),false);
    fixture("bad-loop","looplog",damaged(lo),false);
    fixture("zero","looplog",QByteArray(1,'x'),false);
    fixture("loop-only","looplog",QByteArray(reinterpret_cast<const char*>(&lo),sizeof(lo)),false);
    const QString missing=traces+"/missing";QDir().mkpath(missing);
    write(missing+"/run-config.json",QJsonDocument(QJsonObject{{"runId","missing"},{"wallAnchorMs",double(wall)},
        {"monotonicAnchorNs","1000000"},{"diagnosticTraceEnabled",false}}).toJson());
    write(missing+"/timing-summary.json",QJsonDocument(QJsonObject{{"recordsIssued",1}}).toJson());
    write(traces+"/loop-only/timing-summary.json",QJsonDocument(QJsonObject{{"recordsIssued",1}}).toJson());
    fixture("enabled-missing", "trace", QByteArray(reinterpret_cast<const char*>(&tr),sizeof(tr)), true);
    write(traces+"/enabled-missing/run-config.json",QJsonDocument(QJsonObject{{"runId","enabled-missing"},
        {"wallAnchorMs",double(wall)},{"monotonicAnchorNs","1000000"},{"diagnosticTraceEnabled",true},
        {"timingEnabled",true},{"loopLogEnabled",true}}).toJson());
    const QString emptyEnabled=traces+"/empty-enabled";QDir().mkpath(emptyEnabled);
    write(emptyEnabled+"/run-config.json",QJsonDocument(QJsonObject{{"runId","empty-enabled"},
        {"wallAnchorMs",double(wall)},{"monotonicAnchorNs","1000000"},{"diagnosticTraceEnabled",false},
        {"timingEnabled",true},{"loopLogEnabled",true}}).toJson());
    const QString anchorMissing=traces+"/anchor-missing";QDir().mkpath(anchorMissing);
    write(anchorMissing+"/run-config.json",QJsonDocument(QJsonObject{{"runId","anchor-missing"},
        {"diagnosticTraceEnabled",false},{"timingEnabled",true},{"loopLogEnabled",false}}).toJson());
    const QString noAnchor=traces+"/no-anchor";QDir().mkpath(noAnchor);
    write(noAnchor+"/run-config.json",QJsonDocument(QJsonObject{{"runId","no-anchor"}}).toJson());
    write(noAnchor+"/looplog-summary.json",QJsonDocument(QJsonObject{{"recordsIssued",1}}).toJson());
    paimage::installTraceBundle(traces,tools);
    DiagnosticRecorder::Options options;options.rootDirectory=root.path()+"/logs";DiagnosticRecorder recorder(options);
    auto cut=recorder.captureTimeWindowRequest(QDateTime::fromMSecsSinceEpoch(wall-1000),QDateTime::fromMSecsSinceEpoch(wall+1000));
    t.correlation=30;timer.observe(t,true);
    auto later=first;later.correlation=202;writer.push(later);
    auto laterLoop=firstLoop;laterLoop.timeNs=1000000;loopLog.push(laterLoop);
    auto result=DiagnosticRecorder::exportTimeWindow(cut,root.path()+"/export.zip");require(result.success);
    const auto entries=zipEntries(root.path()+"/export.zip");
    auto trace=entries.value("paimage/run1/trace-0.bin");require(trace.size()==64);paimage::TraceRecord record;std::memcpy(&record,trace.constData(),64);
    require(record.sequence==1&&record.correlation==101);
    require(entries.value("paimage/run1/looplog-0.bin").size()==160);
    auto loopSummary=QJsonDocument::fromJson(entries.value("paimage/run1/looplog-summary.json")).object();
    require(loopSummary.value("recordsWritten").toInt()==2&&!loopSummary.value("loopLogIncomplete").toBool(true));
    auto manifest=QJsonDocument::fromJson(entries.value("paimage/manifest.json")).object();
    require(manifest.value("traceIncomplete").toBool()&&manifest.value("runs").toArray().size()==9);
    const auto skipped=manifest.value("skippedRuns").toArray();require(skipped.size()==5);
    QHash<QString,QJsonObject> skippedById;
    for(const auto& item:skipped){const auto obj=item.toObject();skippedById.insert(obj.value("runId").toString(),obj);}
    const auto missingTiming=QJsonDocument::fromJson(entries.value("paimage/enabled-missing/timing-summary.json")).object();
    const auto missingLoop=QJsonDocument::fromJson(entries.value("paimage/enabled-missing/looplog-summary.json")).object();
    require(missingTiming.value("exportMissing").toBool()&&missingTiming.value("timingIncomplete").toBool());
    require(missingLoop.value("exportMissing").toBool()&&missingLoop.value("loopLogIncomplete").toBool());
    bool foundMissingRun=false;
    for(const auto& item:manifest.value("runs").toArray()){
        const auto obj=item.toObject();
        if(obj.value("runId").toString()=="enabled-missing"){
            foundMissingRun=true;require(obj.value("traceIncomplete").toBool()&&obj.value("timingMissing").toBool()&&obj.value("loopLogMissing").toBool());
        }
    }
    require(foundMissingRun);
    require(skippedById.value("empty-enabled").value("timingMissing").toBool()&&
        skippedById.value("empty-enabled").value("loopLogMissing").toBool());
    require(skippedById.value("anchor-missing").value("reason").toString()=="missing_time_anchor"&&
        skippedById.value("anchor-missing").value("timingMissing").toBool()&&
        !skippedById.value("anchor-missing").value("loopLogMissing").toBool());
    require(skippedById.value("no-anchor").value("reason").toString()=="missing_time_anchor");
    require(skippedById.value("zero").value("loopLogFormatError").toBool()&&
        skippedById.value("zero").value("loopLogIncomplete").toBool());
    require(skippedById.value("missing").value("timingIncomplete").toBool()&&
        !skippedById.value("missing").value("timingFormatError").toBool());
    for(const auto& item:{qMakePair(QString("bad-trace"),QString("trace")),qMakePair(QString("bad-timing"),QString("timing")),qMakePair(QString("bad-loop"),QString("looplog"))}){
        const auto prefix="paimage/"+item.first+"/"+item.second;
        const auto summary=QJsonDocument::fromJson(entries.value(prefix+"-summary.json")).object();
        require(summary.value("exportFormatError").toBool()&&summary.value("exportCountCovered").toBool());
        require(entries.value(prefix+"-0.bin").size()+1==(item.second=="trace"?int(sizeof(tr))+1:item.second=="timing"?int(sizeof(ti))+1:int(sizeof(lo))+1));
        const auto incompleteKey=item.second=="trace"?"traceIncomplete":item.second=="timing"?"timingIncomplete":"loopLogIncomplete";
        require(summary.value(incompleteKey).toBool());
    }
    const auto clipped=QJsonDocument::fromJson(entries.value("paimage/clipped-trace/trace-summary.json")).object();
    require(clipped.value("traceIncomplete").toBool()&&clipped.value("exportClipped").toBool()&&
        !clipped.value("exportFormatError").toBool(true)&&clipped.value("exportCountCovered").toBool());
    require(entries.value("paimage/clipped-trace/trace-0.bin").size()==int(sizeof(tr)));
    const auto loopOnlySummary=QJsonDocument::fromJson(entries.value("paimage/loop-only/looplog-summary.json")).object();
    require(!loopOnlySummary.value("loopLogIncomplete").toBool(true)&&!loopOnlySummary.value("exportFormatError").toBool(true));
    require(QJsonDocument::fromJson(entries.value("paimage/loop-only/timing-summary.json")).object().value("timingIncomplete").toBool());
    require(entries.value("paimage/run2/trace-0.bin").size()==64);
    const auto only=entries.value("paimage/timing-only/timing-0.bin");
    require(only.size()==sizeof(paimage::TimingRecord));
    paimage::TimingRecord timed{};std::memcpy(&timed,only.constData(),sizeof(timed));
    require(timed.correlation==20&&entries.contains("paimage/timing-only/run-config.json"));
    const auto timingSummary=QJsonDocument::fromJson(entries.value("paimage/timing-only/timing-summary.json")).object();
    require(timingSummary.value("recordsWritten").toInt()==1&&timingSummary.value("exportFilteredRecords").toInt()==1);
    for(const auto& id:{"bad-trace","bad-timing","bad-loop","zero","loop-only","missing","clipped-trace","no-anchor","enabled-missing","empty-enabled","anchor-missing"})
        require(QDir(traces+"/"+id).removeRecursively());
    const QString cleanRoot=root.path()+"/clean-traces";QDir().mkpath(cleanRoot+"/run2");
    for(const auto& file:{"run-config.json","trace-0.bin","trace-summary.json"})
        require(QFile::copy(run2+"/"+file,cleanRoot+"/run2/"+file));
    const QString outsideRun=cleanRoot+"/outside";QDir().mkpath(outsideRun);
    write(outsideRun+"/run-config.json",QJsonDocument(QJsonObject{{"runId","outside"},{"wallAnchorMs",double(wall)},
        {"monotonicAnchorNs","1000000"},{"timingEnabled",false},{"loopLogEnabled","unknown"}}).toJson());
    outside.sequence=1;
    write(outsideRun+"/trace-0.bin",QByteArray(reinterpret_cast<const char*>(&outside),sizeof(outside)));
    write(outsideRun+"/trace-summary.json",QJsonDocument(QJsonObject{{"recordsIssued",1}}).toJson());
    paimage::installTraceBundle(cleanRoot,tools);
    auto cleanCut=recorder.captureTimeWindowRequest(QDateTime::fromMSecsSinceEpoch(wall-1000),QDateTime::fromMSecsSinceEpoch(wall+1000));
    require(DiagnosticRecorder::exportTimeWindow(cleanCut,root.path()+"/clean.zip").success);
    QFile cleanZip(root.path()+"/clean.zip");require(cleanZip.open(QIODevice::ReadOnly));const auto cleanBytes=cleanZip.readAll();
    QJsonObject cleanManifest;
    for(int at=0;at+30<=cleanBytes.size()&&number(cleanBytes,at,4)==0x04034b50;){
        int size=number(cleanBytes,at+18,4),name=number(cleanBytes,at+26,2),extra=number(cleanBytes,at+28,2),data=at+30+name+extra;
        require(data+size<=cleanBytes.size());
        const auto entryName=QString::fromUtf8(cleanBytes.mid(at+30,name));
        require(!entryName.startsWith("paimage/outside/"));
        if(entryName=="paimage/manifest.json"){
            QByteArray payload;require(decodeZipPayload(cleanBytes.mid(data,size),number(cleanBytes,at+8,2),number(cleanBytes,at+22,4),payload));
            cleanManifest=QJsonDocument::fromJson(payload).object();
        }
        at=data+size;
    }
    require(!cleanManifest.isEmpty()&&!cleanManifest.value("traceIncomplete").toBool(true));
    require(cleanManifest.value("runs").toArray().size()==1&&
        cleanManifest.value("runs").toArray().first().toObject().value("runId").toString()=="run2"&&
        cleanManifest.value("skippedRuns").toArray().isEmpty());
    paimage::installTraceBundle(traces,tools);
    const QString failedPath=root.path()+"/already-exists.zip";
    write(failedPath,"existing file");
    auto retryCut=recorder.captureTimeWindowRequest(QDateTime::fromMSecsSinceEpoch(wall-1000),QDateTime::fromMSecsSinceEpoch(wall+1000));
    auto failed=DiagnosticRecorder::exportTimeWindow(retryCut,failedPath);require(!failed.success);
    require(readFile(failedPath)=="existing file");
    retryCut=recorder.captureTimeWindowRequest(QDateTime::fromMSecsSinceEpoch(wall-1000),QDateTime::fromMSecsSinceEpoch(wall+1000));
    auto retried=DiagnosticRecorder::exportTimeWindow(retryCut,root.path()+"/retry.zip");require(retried.success);
    writer.stop();loopLog.stop();timer.stop();
    auto stoppedCut=recorder.captureTimeWindowRequest(QDateTime::fromMSecsSinceEpoch(wall-1000),QDateTime::fromMSecsSinceEpoch(wall+1000));
    require(DiagnosticRecorder::exportTimeWindow(stoppedCut,root.path()+"/stopped.zip").success);
    DiagnosticRecorder::setBundleCapture({});
    // Quota cleanup cannot remove an active or exporting run or an unknown directory.
    const QString managed=root.path()+"/managed";QDir().mkpath(managed);
    auto makeRun=[&]{
        const auto id=QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto path=QDir(managed).filePath(id);QDir().mkpath(path);
        write(path+"/run-config.json",QJsonDocument(QJsonObject{{"runId",id},{"backendId","paimage-receiver-diagnostics"}}).toJson());
        write(path+"/trace-0.bin",QByteArray(4096,'x'));return path;
    };
    const auto activePath=makeRun(),oldPath=makeRun(),unknownPath=makeRun();
    write(unknownPath+"/user.txt","keep");
    QLockFile active(activePath+"/.active.lock");require(active.tryLock(0));
    {
        const auto reader=diagnostic_storage::exportLease(managed);require(bool(reader));
        diagnostic_storage::pruneTraces(managed,0,1);
        require(QDir(oldPath).exists());
    }
    diagnostic_storage::pruneTraces(managed,0,1);
    require(!QDir(oldPath).exists()&&QDir(activePath).exists()&&QDir(unknownPath).exists());

    const QString stoppedRoot=root.path()+"/stop-prune";
    const QString id=QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString stoppedRun=stoppedRoot+"/"+id;
    require(QDir().mkpath(stoppedRun));
    const QByteArray config=QJsonDocument(QJsonObject{{"runId",id},{"backendId","paimage-receiver-diagnostics"},
        {"wallAnchorMs",double(wall)},{"monotonicAnchorNs","1000000"},{"diagnosticTraceEnabled",true},
        {"timingEnabled",true},{"loopLogEnabled",true}}).toJson();
    write(stoppedRun+"/run-config.json",config);
    paimage::installTraceBundle(stoppedRoot,tools);
    const auto start=QDateTime::fromMSecsSinceEpoch(wall-1000),end=QDateTime::fromMSecsSinceEpoch(wall+1000);
    DiagnosticRecorder::TimeWindowRequest pending;
    auto expectedTrace=first;expectedTrace.sequence=1;
    auto expectedTiming=t;expectedTiming.sequence=1;expectedTiming.startNs=expectedTiming.endNs=1000000;
    expectedTiming.correlation=303;
    auto expectedLoop=firstLoop;expectedLoop.sequence=1;expectedLoop.value0=404;
    auto expectedBurst=burst;expectedBurst.sequence=2;
    {
        QLockFile listening(stoppedRun+"/.active.lock");require(listening.tryLock(0));
        {
            const std::filesystem::path path(stoppedRun.toStdWString());
            paimage::TraceWriter traceWriter(path);
            paimage::TimingWriter timingWriter(path);
            paimage::LoopLog loopWriter(path,paimage::LoopLog::Budget{64});
            traceWriter.push(expectedTrace);
            timingWriter.observe(expectedTiming,true);
            loopWriter.push(expectedLoop);
            loopWriter.push(expectedBurst);
            pending=recorder.registerTimeWindowRequest(start,end);
            require(bool(pending.bundleExtension));

            // The worker has not started. These timestamps are inside the window,
            // so the sequence boundary alone must exclude the later records.
            auto postTrace=expectedTrace;postTrace.correlation=1111;traceWriter.push(postTrace);
            auto postTiming=expectedTiming;postTiming.correlation=2222;timingWriter.observe(postTiming,true);
            auto postLoop=expectedLoop;postLoop.value0=3333;loopWriter.push(postLoop);
            traceWriter.stop();timingWriter.stop();loopWriter.stop();
        } // Unregister the writers, as on listener stop.
    } // Release the active-run lock before stop-triggered cleanup.
    require(!QFileInfo::exists(stoppedRun+"/.active.lock"));
    require(readFile(stoppedRun+"/trace-0.bin").size()==2*sizeof(expectedTrace));
    require(readFile(stoppedRun+"/timing-0.bin").size()==2*sizeof(expectedTiming));
    require(readFile(stoppedRun+"/looplog-window-1.bin").size()==3*sizeof(expectedLoop));
    {
        const auto lease=diagnostic_storage::lease(stoppedRoot);
        require(bool(lease)); // Registration must not acquire the worker's disk lease.
    }
    diagnostic_storage::pruneTraces(stoppedRoot,0,1);
    require(QDir(stoppedRun).exists());
    bool eventDispatched=false;
    QTimer::singleShot(0,&app,[&]{eventDispatched=true;});
    QCoreApplication::processEvents();
    require(eventDispatched);

    // Keep a copy alive to verify that a handoff releases the pending intent
    // for all copies, not just the copy used by the exporter.
    auto queuedCopy=pending;
    const auto stopZip=root.path()+"/stop-prune.zip";
    require(recorder.exportTimeWindowAsync(queuedCopy,stopZip).get().success);
    const auto preserved=zipEntries(stopZip);
    const auto prefix="paimage/"+id+"/";
    require(preserved.value(prefix+"trace-0.bin")==recordBytes(expectedTrace));
    require(preserved.value(prefix+"timing-0.bin")==recordBytes(expectedTiming));
    require(preserved.value(prefix+"looplog-0.bin")==recordBytes(expectedLoop)+recordBytes(expectedBurst));
    const auto preservedManifest=QJsonDocument::fromJson(preserved.value("paimage/manifest.json")).object();
    require(!preservedManifest.value("traceIncomplete").toBool(true));
    require(preservedManifest.value("runs").toArray().size()==1);
    diagnostic_storage::pruneTraces(stoppedRoot,0,1);
    require(!QDir(stoppedRun).exists());
    const auto reuseZip=root.path()+"/reused.zip";
    require(!DiagnosticRecorder::exportTimeWindow(pending,reuseZip).success&&!QFileInfo::exists(reuseZip));

    // Simulate loss outside managed cleanup. A timing-only registered identity
    // must still be found missing, and the failed archive must not be published.
    require(QDir().mkpath(stoppedRun));
    write(stoppedRun+"/run-config.json",config);
    DiagnosticRecorder::TimeWindowRequest lost;
    {
        paimage::TimingWriter lostWriter(std::filesystem::path(stoppedRun.toStdWString()));
        lostWriter.observe(expectedTiming,true);
        lost=recorder.registerTimeWindowRequest(start,end);
        lostWriter.stop();
    }
    require(QDir(stoppedRun).removeRecursively());
    const auto lostZip=root.path()+"/lost.zip";
    const auto lostResult=DiagnosticRecorder::exportTimeWindow(lost,lostZip);
    require(!lostResult.success&&lostResult.error.contains(id)&&!QFileInfo::exists(lostZip));
    require(QDir().mkpath(stoppedRun));
    write(stoppedRun+"/run-config.json",config);
    diagnostic_storage::pruneTraces(stoppedRoot,0,1);
    require(!QDir(stoppedRun).exists()); // Failure released the pending intent too.

    const auto windowRoot = root.filePath("burst-windows");
    paimage::installTraceBundle(windowRoot, tools);
    lo.kind = uint16_t(paimage::LoopKind::Loop);
    auto keep = lo;
    auto clip = lo;
    clip.sequence = 2;
    clip.timeNs += 5000000000ULL;
    auto makeWindowRun = [&](const QString &runId, const QString &kind) {
        const auto path = QDir(windowRoot).filePath(runId);
        require(QDir().mkpath(path));
        write(path+"/run-config.json", QJsonDocument(QJsonObject{
            {"runId", runId}, {"wallAnchorMs", double(wall)}, {"monotonicAnchorNs", "1000000"},
            {"diagnosticTraceEnabled", false}, {"timingEnabled", false}, {"loopLogEnabled", true}}).toJson());
        const bool noBurst = kind == "no_burst", budget = kind == "budget_omitted";
        QJsonArray windows;
        if (!noBurst) windows.append(QJsonObject{
            {"windowId", 1}, {"file", budget ? "" : "looplog-window-1.bin"},
            {"recordCount", 2}, {"bytes", 2*int(sizeof(lo))}, {"budgetOmitted", budget}});
        if (kind == "verified" || kind == "corrupt")
            write(path+"/looplog-window-1.bin", recordBytes(keep)+recordBytes(clip)+
                (kind == "corrupt" ? QByteArray("x") : QByteArray{}));
        write(path+"/looplog-summary.json", QJsonDocument(QJsonObject{
            {"schemaVersion", 2}, {"mode", "rolling-burst-windows"}, {"recordBytes", int(sizeof(lo))},
            {"recordsIssued", 100}, {"recordsWritten", noBurst||budget ? 0 : 2},
            {"burstMarkEpochs", noBurst ? 0 : 1}, {"budgetExhausted", budget}, {"windows", windows}}).toJson());
    };
    makeWindowRun("quiet", "no_burst");
    makeWindowRun("healthy", "verified");
    auto healthyPath = root.filePath("healthy-windows.zip");
    require(DiagnosticRecorder::exportTimeWindow(recorder.registerTimeWindowRequest(start,end), healthyPath).success);
    const auto healthyZip = zipEntries(healthyPath);
    const auto windowManifest = [](const auto &zip) {
        return QJsonDocument::fromJson(zip.value("paimage/manifest.json")).object();
    };
    const auto readLoopSummary = [](const auto &zip, const QString &id) {
        return QJsonDocument::fromJson(zip.value("paimage/"+id+"/looplog-summary.json")).object();
    };
    const auto quiet = readLoopSummary(healthyZip, "quiet");
    const auto healthyLoop = readLoopSummary(healthyZip, "healthy");
    require(!windowManifest(healthyZip).value("traceIncomplete").toBool(true));
    require(windowManifest(healthyZip).value("skippedRuns").toArray().isEmpty());
    require(quiet.value("exportRetentionStatus") == "no_burst" &&
        !quiet.value("loopLogIncomplete").toBool(true) && quiet.value("recordsWritten").toInt(-1) == 0);
    require(healthyLoop.value("exportCountCovered").toBool() &&
        healthyLoop.value("exportClipped").toBool() && !healthyLoop.value("loopLogIncomplete").toBool(true));
    require(healthyZip.value("paimage/healthy/looplog-0.bin") == recordBytes(keep));

    makeWindowRun("lost-window", "missing_or_unreadable");
    makeWindowRun("corrupt-window", "corrupt");
    makeWindowRun("budget", "budget_omitted");
    const auto faultyPath = root.filePath("faulty-windows.zip");
    require(DiagnosticRecorder::exportTimeWindow(recorder.registerTimeWindowRequest(start,end), faultyPath).success);
    const auto faultyZip = zipEntries(faultyPath);
    auto windowStatus = [&](const char *id) {
        const auto summary = readLoopSummary(faultyZip, id);
        require(summary.value("loopLogIncomplete").toBool());
        return summary.value("exportWindows").toArray().at(0).toObject().value("status").toString();
    };
    require(windowStatus("lost-window") == "missing_or_unreadable");
    require(windowStatus("corrupt-window") == "corrupt");
    require(windowStatus("budget") == "budget_omitted");
    require(windowManifest(faultyZip).value("traceIncomplete").toBool() &&
        windowManifest(faultyZip).value("skippedRuns").toArray().size() == 2);

    DiagnosticRecorder::setBundleCapture({});
    std::cout<<"PASS trace boundaries, stop/prune handoff and LoopLog burst window integrity"<<std::endl;
}
