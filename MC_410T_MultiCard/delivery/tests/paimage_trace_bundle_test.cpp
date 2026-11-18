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
#include <QProcess>
#include <QTimer>
#include <cstring>
#include <future>
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
QJsonObject zipObject(const QHash<QString,QByteArray>& zip,const QString& name) {
    return QJsonDocument::fromJson(zip.value(name)).object();
}
void testBundledAnalyzers(const QTemporaryDir& root,DiagnosticRecorder& recorder,const QStringList& args) {
    const auto e2eRoot=root.filePath("closeout-e2e");
    const auto e2eRun=e2eRoot+"/listener";
    const auto e2eWall=QDateTime::currentDateTime().addSecs(-30);
    constexpr qint64 second=1000000000ll;
    const auto tick=[](qint64 seconds){return 1000000+seconds*second;};
    require(QDir().mkpath(e2eRun));
    write(e2eRun+"/run-config.json",QJsonDocument(QJsonObject{
        {"runId","listener"},{"wallAnchorMs",e2eWall.toMSecsSinceEpoch()},{"monotonicAnchorNs","1000000"},
        {"diagnosticTraceEnabled",true},{"timingEnabled",false},{"loopLogEnabled",true},
        {"dataPort",8001},{"cards",1},{"samples",180},{"bits",32}}).toJson());
    paimage::TraceRecord ingress;
    ingress.sequence=1;
    ingress.stage=1;
    ingress.localPort=8001;
    ingress.length=1444;
    ingress.monotonicNs=tick(8);
    write(e2eRun+"/trace-0.bin",recordBytes(ingress));
    write(e2eRun+"/trace-summary.json",QJsonDocument(QJsonObject{
        {"recordsIssued",1},{"recordsWritten",1},{"traceIncomplete",false}}).toJson());
    paimage::LoopRecord firstMarker;
    firstMarker.kind=uint16_t(paimage::LoopKind::BurstMark);
    firstMarker.sequence=1; firstMarker.timeNs=tick(6); firstMarker.value0=1;
    paimage::LoopRecord gap;
    gap.kind=uint16_t(paimage::LoopKind::Loop);
    gap.sequence=2; gap.timeNs=tick(9); gap.spanNs=50000000;
    auto stall=gap;
    stall.sequence=3; stall.timeNs=tick(10); stall.kind=uint16_t(paimage::LoopKind::Stalled);
    auto secondMarker=firstMarker;
    secondMarker.sequence=4; secondMarker.timeNs=tick(13); secondMarker.value0=2;
    auto outside=gap;
    outside.sequence=5; outside.timeNs=tick(19);
    const auto firstBody=recordBytes(firstMarker)+recordBytes(gap)+recordBytes(stall);
    const auto secondBody=recordBytes(gap)+recordBytes(stall)+recordBytes(secondMarker);
    write(e2eRun+"/looplog-window-1.bin",firstBody);
    paimage::installTraceBundle(e2eRoot,args[2]);
    const auto makeArchive=[&](bool conflict,const QString& path) {
        const auto count=conflict?4:3;
        write(e2eRun+"/looplog-window-2.bin",secondBody+(conflict?recordBytes(outside):QByteArray{}));
        const QJsonArray windowMeta{
            QJsonObject{{"windowId",1},{"file","looplog-window-1.bin"},{"startNs",tick(1)},{"endNs",tick(11)},
                {"recordCount",3},{"bytes",firstBody.size()},{"burstEpochs",QJsonArray{1}}},
            QJsonObject{{"windowId",2},{"file","looplog-window-2.bin"},{"startNs",tick(8)},{"endNs",tick(18)},
                {"recordCount",count},{"bytes",count*int(sizeof(gap))},{"burstEpochs",QJsonArray{2}}}};
        write(e2eRun+"/looplog-summary.json",QJsonDocument(QJsonObject{
            {"mode","rolling-burst-windows"},{"recordBytes",int(sizeof(gap))},{"recordsIssued",conflict?5:4},
            {"recordsWritten",3+count},{"queueDropped",0},{"loopLogIncomplete",false},{"windows",windowMeta}}).toJson());
        const auto request=recorder.registerTimeWindowRequest(e2eWall,e2eWall.addSecs(18));
        require(DiagnosticRecorder::exportTimeWindow(request,path).success);
        return zipEntries(path);
    };
    const auto healthy=makeArchive(false,root.filePath("e2e-healthy.zip"));
    require(!zipObject(healthy,"paimage/manifest.json").value("traceIncomplete").toBool(true));
    const auto e2eZip=root.filePath("e2e-bounds.zip");
    const auto real=makeArchive(true,e2eZip);
    const auto e2eSummary=zipObject(real,"paimage/listener/looplog-summary.json");
    const auto exports=e2eSummary.value("exportWindows").toArray();
    require(zipObject(real,"paimage/manifest.json").value("traceIncomplete").toBool() && e2eSummary.value("loopLogIncomplete").toBool() &&
        e2eSummary.value("exportFormatError").toBool() && e2eSummary.value("exportFilteredRecords").toInt(-1)==0 &&
        exports[0].toObject().value("status")=="verified" && exports[1].toObject().value("status")=="corrupt" &&
        exports[0].toObject().value("exportRecords").toInt()==3 && exports[1].toObject().value("exportRecords").toInt()==3);
    const auto scriptDir=root.filePath("zip-tools");
    require(QDir().mkpath(scriptDir));
    for (const auto& name : {"paimage_trace_analyze.py","startup_diagnostics_analyze.py"}) {
        const auto script=real.value(QString("paimage/tools/")+name);
        require(script==readFile(QDir(args[2]).filePath(name)));
        write(QDir(scriptDir).filePath(name),script);
    }
    const auto analyzeZip=[&](const QString& tool,const QStringList& inputs,const QString& outputName) {
        QProcess python;
        python.setProgram(args[1]);
        const auto output=root.filePath(outputName);
        python.setArguments(QStringList{"-B",QDir(scriptDir).filePath(tool)}+inputs+QStringList{"--output",output});
        python.start();
        require(python.waitForFinished(20000));
        if (python.exitCode()!=0) std::cerr<<python.readAllStandardError().constData();
        require(python.exitStatus()==QProcess::NormalExit && python.exitCode()==0);
        return QJsonDocument::fromJson(readFile(output)).object();
    };
    const auto generic=analyzeZip("paimage_trace_analyze.py",{e2eZip,"--packets","1","--data-port","8001"},"generic.json");
    const auto startup=analyzeZip("startup_diagnostics_analyze.py",{"--application-zip",e2eZip},"startup.json");
    const auto genericLoop=generic.value("loopLog").toObject();
    const auto application=startup.value("application").toObject();
    require(genericLoop==application.value("loop").toObject() &&
        genericLoop==startup.value("reusedTraceAnalyzer").toObject().value("loopLog").toObject());
    require(genericLoop.value("records").toInt()==6 && genericLoop.value("logicalRecords").toInt()==4 &&
        genericLoop.value("duplicateRecordCopies").toInt()==2 && genericLoop.value("loopLogIncomplete").toBool() &&
        genericLoop.value("loopGapsOver20ms").toInt()==1 && genericLoop.value("stallHints").toArray().size()==1);
    const auto physical=genericLoop.value("files").toArray();
    require(physical.size()==2 && physical[0].toObject().value("records").toInt()==3 &&
        physical[1].toObject().value("records").toInt()==3);
    int gapCount=0;
    for (const auto& row : application.value("diagnostics").toObject().value("perSecond").toArray())
        gapCount+=row.toObject().value("loopGapCount").toInt();
    require(application.value("ingressPackets").toArray().size()==1 && gapCount==1);
    std::cout<<"e2e_zip traceIncomplete=true exportFormatError=true physicalRecords=3+3 logicalRecords=4 "
             <<"loopGapsOver20ms=1 stallHints=1 startupLoopGapCount=1\n";

    const auto extracted=root.filePath("extracted");
    const auto extractedRun=extracted+"/paimage/listener";
    require(QDir().mkpath(extractedRun));
    for (const auto& name : {"paimage/manifest.json","paimage/listener/run-config.json",
            "paimage/listener/trace-summary.json","paimage/listener/trace-0.bin",
            "paimage/listener/looplog-summary.json","paimage/listener/looplog-0.bin"}) {
        require(real.contains(name));
        write(extracted+"/"+name,real.value(name));
    }
    const auto missing=analyzeZip("paimage_trace_analyze.py",
        {extractedRun,"--packets","1","--data-port","8001"},"missing.json").value("loopLog").toObject();
    require(missing.value("missingRecordCount").toInt()==3 && missing.value("loopLogIncomplete").toBool() &&
        missing.value("files").toArray()[1].toObject().value("status")=="missing");
}

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
    require(loopSummary.value("recordsWritten").toInt()==2&&loopSummary.value("exportCountCovered").toBool());
    require(loopSummary.value("exportWindows").toArray().first().toObject().value("status")=="truncated");
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
    require(skippedById.value("no-anchor").value("runTimeRangeStatus")=="unknown");
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
    require(QFile::rename(oldPath+"/trace-0.bin",oldPath+"/looplog-window-1.bin.part"));
    require(QFile::rename(activePath+"/trace-0.bin",activePath+"/looplog-window-1.bin.part"));
    write(unknownPath+"/user.txt","keep");
    write(unknownPath+"/looplog-window-2.bin.part","retained with unknown file");
    QLockFile active(activePath+"/.active.lock");require(active.tryLock(0));
    const auto accounted=diagnostic_storage::pruneTraces(managed);
    require(accounted && *accounted >= 8192);
    {
        const auto reader=diagnostic_storage::exportLease(managed);require(bool(reader));
        diagnostic_storage::pruneTraces(managed,0,1);
        require(QDir(oldPath).exists());
    }
    diagnostic_storage::pruneTraces(managed,0,1);
    require(!QDir(oldPath).exists()&&QFileInfo::exists(activePath+"/looplog-window-1.bin.part")&&
        QFileInfo::exists(unknownPath+"/looplog-window-2.bin.part"));

    const auto expiredPart=makeRun(),freshPart=makeRun();
    require(QFile::rename(expiredPart+"/trace-0.bin",expiredPart+"/looplog-window-1.bin.part"));
    require(QFile::rename(freshPart+"/trace-0.bin",freshPart+"/looplog-window-1.bin.part"));
    for(const auto& name:{"/run-config.json","/looplog-window-1.bin.part"}){
        QFile file(expiredPart+name);
        require(file.open(QIODevice::ReadWrite));
        require(file.setFileTime(QDateTime::currentDateTimeUtc().addDays(-8),QFileDevice::FileModificationTime));
    }
    diagnostic_storage::pruneTraces(managed);
    require(!QDir(expiredPart).exists()&&QDir(freshPart).exists()&&QDir(activePath).exists()&&QDir(unknownPath).exists());

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
            std::promise<void> releaseLoopWriter;
            auto release = releaseLoopWriter.get_future();
            paimage::LoopLog::Budget loopBudget{64};
            loopBudget.beforeDrain = [&] { if (release.valid()) release.get(); };
            paimage::LoopLog loopWriter(path,loopBudget);
            traceWriter.push(expectedTrace);
            timingWriter.observe(expectedTiming,true);
            loopWriter.push(expectedLoop);
            loopWriter.push(expectedBurst);
            pending=recorder.registerTimeWindowRequest(start,end);

            // The worker has not started. These timestamps are inside the window,
            // so the sequence boundary alone must exclude the later records.
            auto postTrace=expectedTrace;postTrace.correlation=1111;traceWriter.push(postTrace);
            auto postTiming=expectedTiming;postTiming.correlation=2222;timingWriter.observe(postTiming,true);
            auto postLoop=expectedLoop;postLoop.value0=3333;loopWriter.push(postLoop);
            releaseLoopWriter.set_value();
            require(bool(pending.bundleExtension));
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
    const auto preservedLoop = QJsonDocument::fromJson(preserved.value(prefix+"looplog-summary.json")).object();
    // The fixture starts less than five seconds into the synthetic clock.
    // Its pre-window is truncated, even though both records at the click cut survived.
    require(preservedManifest.value("traceIncomplete").toBool() &&
        preservedLoop.value("exportCountCovered").toBool() &&
        preservedLoop.value("exportClipped").toBool() &&
        preservedLoop.value("exportWindows").toArray().first().toObject().value("status") == "truncated");
    require(preservedManifest.value("runs").toArray().size()==1);
    diagnostic_storage::pruneTraces(stoppedRoot,0,1);
    require(!QDir(stoppedRun).exists());
    const auto reuseZip=root.path()+"/reused.zip";
    require(!DiagnosticRecorder::exportTimeWindow(pending,reuseZip).success&&!QFileInfo::exists(reuseZip));

    const QString formalMessage="formal-event-survives-attachment-failure";
    const auto failureStart=QDateTime::currentDateTime().addSecs(-5);
    const auto failureEnd=failureStart.addSecs(30);
    recorder.recordEvent("export-test",formalMessage);
    require(recorder.flush(5000));
    const auto readManifest=[](const auto& zip) {
        return QJsonDocument::fromJson(zip.value("paimage/manifest.json")).object();
    };
    {
        const auto locked=diagnostic_storage::lease(stoppedRoot);
        require(bool(locked));
        const auto path=root.filePath("lease-unavailable.zip");
        require(DiagnosticRecorder::exportTimeWindow(recorder.registerTimeWindowRequest(failureStart,failureEnd),path).success);
        const auto zip=zipEntries(path);
        const auto manifest=readManifest(zip);
        require(zip.value("events.jsonl").contains(formalMessage.toUtf8()) &&
            manifest.value("attachmentStatus")=="unavailable" && manifest.value("traceIncomplete").toBool() &&
            manifest.value("files").toArray().isEmpty() &&
            manifest.value("attachmentIssues").toArray().first().toObject().value("code")=="source_lease_unavailable");
        std::cout<<"lease_failure formalEvent=true attachmentStatus=unavailable code=source_lease_unavailable\n";
    }
    {
        std::promise<bool> ready;
        std::promise<void> release;
        auto held=ready.get_future();
        auto done=release.get_future();
        std::thread pruner([&]{
            const auto permit=diagnostic_storage::tryPrune(stoppedRoot);
            ready.set_value(bool(permit));
            done.wait();
        });
        const auto locked=held.get();
        const auto request=recorder.captureExportRequest();
        release.set_value();
        pruner.join();
        require(locked);
        const auto path=root.filePath("registration-unavailable.zip");
        require(DiagnosticRecorder::exportRequest(request,path).success);
        const auto zip=zipEntries(path);
        const auto manifest=readManifest(zip);
        require(zip.value("events.jsonl").contains(formalMessage.toUtf8()) &&
            manifest.value("attachmentStatus")=="unavailable" &&
            manifest.value("attachmentIssues").toArray().first().toObject().value("code")=="export_intent_unavailable");
    }

    // Loss outside managed cleanup must leave the formal log and other runs available.
    require(QDir().mkpath(stoppedRun));
    write(stoppedRun+"/run-config.json",config);
    DiagnosticRecorder::TimeWindowRequest lost;
    {
        paimage::TimingWriter lostWriter(std::filesystem::path(stoppedRun.toStdWString()));
        lostWriter.observe(expectedTiming,true);
        lost=recorder.registerTimeWindowRequest(failureStart,failureEnd);
        lostWriter.stop();
    }
    require(QDir(stoppedRun).removeRecursively());
    const auto available=stoppedRoot+"/available";
    require(QDir().mkpath(available));
    write(available+"/run-config.json",QJsonDocument(QJsonObject{
        {"runId","available"},{"wallAnchorMs",failureStart.toMSecsSinceEpoch()},
        {"monotonicAnchorNs","1000000"},{"diagnosticTraceEnabled",true}}).toJson());
    write(available+"/trace-0.bin",recordBytes(expectedTrace));
    write(available+"/trace-summary.json",QJsonDocument(QJsonObject{{"recordsIssued",1}}).toJson());
    const auto lostZip=root.path()+"/lost.zip";
    const auto lostResult=DiagnosticRecorder::exportTimeWindow(lost,lostZip);
    require(lostResult.success);
    const auto lostEntries=zipEntries(lostZip);
    const auto lostManifest=readManifest(lostEntries);
    const auto lostRun=lostManifest.value("skippedRuns").toArray().first().toObject();
    require(lostEntries.value("events.jsonl").contains(formalMessage.toUtf8()) &&
        lostEntries.value("paimage/available/trace-0.bin")==recordBytes(expectedTrace) &&
        lostManifest.value("attachmentStatus")=="partial" && lostManifest.value("traceIncomplete").toBool() &&
        lostRun.value("runId")==id && lostRun.value("reason")=="run_directory_unavailable");
    std::cout<<"lost_run formalEvent=true healthyRunPreserved=true attachmentStatus=partial code=run_directory_unavailable\n";

    auto failedWrite=recorder.registerTimeWindowRequest(failureStart,failureEnd);
    auto attachment=std::move(failedWrite.bundleExtension);
    bool memberRejected=false;
    failedWrite.bundleExtension=[&](const auto& sink,QString* error) {
        return attachment([&](const QString& name,const QByteArray& bytes) {
            if (name=="paimage/available/trace-0.bin") {
                memberRejected=true;
                return false;
            }
            return sink(name,bytes);
        },error);
    };
    const auto memberPath=root.filePath("failed-member.zip");
    const auto memberFailure=DiagnosticRecorder::exportTimeWindow(failedWrite,memberPath);
    require(memberRejected && !memberFailure.success && !memberFailure.error.isEmpty() && !QFileInfo::exists(memberPath) &&
        QDir(root.path()).entryList({"failed-member.zip.part-*"},QDir::Files).isEmpty());
    require(QDir().mkpath(stoppedRun));
    write(stoppedRun+"/run-config.json",config);
    diagnostic_storage::pruneTraces(stoppedRoot,0,1);
    require(!QDir(stoppedRun).exists()); // The degraded export released the pending intent too.

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

    const auto scopedRoot = root.filePath("scoped-windows");
    const auto scopedRun = scopedRoot + "/listener";
    require(QDir().mkpath(scopedRun));
    paimage::installTraceBundle(scopedRoot,tools);
    const auto scopedWall=QDateTime::currentDateTime().addSecs(-65);
    write(scopedRun+"/run-config.json",QJsonDocument(QJsonObject{
        {"runId","listener"},{"wallAnchorMs",double(scopedWall.toMSecsSinceEpoch())},
        {"monotonicAnchorNs","1000000"},{"diagnosticTraceEnabled",false},
        {"timingEnabled",false},{"loopLogEnabled",true}}).toJson());
    QJsonArray scopedWindows;
    auto addScopedWindow = [&](int id, qint64 firstSecond, bool truncated) {
        const auto firstNs=1000000+firstSecond*1000000000ll;
        auto record=keep;
        record.sequence=(id-1)*3+1;
        record.timeNs=firstNs;
        auto binary=recordBytes(record);
        auto marker=burst;
        marker.sequence=++record.sequence;
        marker.timeNs=firstNs+5000000000ull;
        binary+=recordBytes(marker);
        ++record.sequence;
        record.timeNs=firstNs+10000000000ull;
        binary+=recordBytes(record);
        const auto name=QStringLiteral("looplog-window-%1.bin").arg(id);
        write(scopedRun+"/"+name,binary);
        scopedWindows.append(QJsonObject{{"windowId",id},{"startNs",firstNs},{"endNs",qint64(record.timeNs)},
            {"file",name},{"recordCount",3},{"bytes",binary.size()},{"preWindowTruncated",truncated}});
    };
    addScopedWindow(1,1,true);
    addScopedWindow(2,51,false);
    const auto afterRange=scopedRoot+"/after-range";
    require(QDir().mkpath(afterRange));
    write(afterRange+"/run-config.json",QJsonDocument(QJsonObject{
        {"runId","after-range"},{"schemaVersion",3},{"backendId","paimage-receiver-diagnostics"},
        {"wallAnchorMs",scopedWall.addSecs(63).toMSecsSinceEpoch()},{"monotonicAnchorNs","1000000"},
        {"diagnosticTraceEnabled",false},{"loopLogEnabled",true}}).toJson());
    write(afterRange+"/looplog-summary.json",QJsonDocument(QJsonObject{
        {"mode","rolling-burst-windows"},{"recordBytes",int(sizeof(keep))},{"recordsIssued",9},
        {"recordsWritten",0},{"queueDropped",9},{"loopLogIncomplete",true},{"windows",QJsonArray{}}}).toJson());
    write(scopedRun+"/looplog-summary.json",QJsonDocument(QJsonObject{
        {"mode","rolling-burst-windows"},{"recordBytes",int(sizeof(keep))},
        {"recordsIssued",6},{"recordsWritten",6},{"loopLogIncomplete",false},{"windows",scopedWindows}}).toJson());
    const auto exportScoped = [&](qint64 startSecond,const QString& name) {
        const auto path=root.filePath(name);
        const auto request=recorder.registerTimeWindowRequest(scopedWall.addSecs(startSecond),scopedWall.addSecs(62));
        require(DiagnosticRecorder::exportTimeWindow(request,path).success);
        return zipEntries(path);
    };
    const auto onlyHealthy=exportScoped(50,"scoped-healthy.zip");
    const auto onlyHealthySummary=readLoopSummary(onlyHealthy,"listener");
    const auto oldWindow=onlyHealthySummary.value("exportWindows").toArray().first().toObject();
    require(oldWindow.value("status")=="truncated"&&oldWindow.value("exportRecords").toInt()==0&&
        !onlyHealthySummary.value("loopLogIncomplete").toBool(true)&&
        onlyHealthySummary.value("exportCountCovered").toBool()&&
        onlyHealthySummary.value("recordsWritten").toInt()==3&&
        !windowManifest(onlyHealthy).value("traceIncomplete").toBool(true));
    require(!onlyHealthy.contains("paimage/after-range/looplog-summary.json")&&
        windowManifest(onlyHealthy).value("runs").toArray().size()==1);
    const auto partial=exportScoped(10,"scoped-partial.zip");
    const auto partialSummary=readLoopSummary(partial,"listener");
    require(partialSummary.value("recordsWritten").toInt()==4&&
        partialSummary.value("loopLogIncomplete").toBool()&&
        windowManifest(partial).value("traceIncomplete").toBool());
    require(!oldWindow.value("inRequestedWindow").toBool(true)&&
        partialSummary.value("exportWindows").toArray().first().toObject().value("inRequestedWindow").toBool()&&
        partialSummary.value("exportClipped").toBool());
    const auto oldFile=scopedRun+"/looplog-window-1.bin";
    write(oldFile,readFile(oldFile)+QByteArray("x"));
    const auto oldCorrupt=exportScoped(50,"scoped-old-corrupt.zip");
    const auto corruptSummary=readLoopSummary(oldCorrupt,"listener");
    require(!corruptSummary.value("exportFormatError").toBool(true)&&
        !corruptSummary.value("loopLogIncomplete").toBool(true)&&
        corruptSummary.value("exportWindows").toArray().first().toObject().value("status")=="corrupt"&&
        !windowManifest(oldCorrupt).value("traceIncomplete").toBool(true));
    const auto overlappingCorrupt=exportScoped(11,"scoped-overlapping-corrupt.zip");
    require(readLoopSummary(overlappingCorrupt,"listener").value("exportFormatError").toBool()&&
        windowManifest(overlappingCorrupt).value("traceIncomplete").toBool());
    auto contradicting=keep;
    contradicting.sequence=1;
    contradicting.timeNs=51001000000ull;
    write(oldFile,recordBytes(contradicting)+readFile(oldFile).mid(sizeof(keep),2*sizeof(keep)));
    const auto inconsistentZip=exportScoped(50,"scoped-inconsistent.zip");
    const auto inconsistent=readLoopSummary(inconsistentZip,"listener");
    require(inconsistent.value("recordsWritten").toInt()==3&&
        !inconsistent.value("exportCountCovered").toBool(true)&&
        inconsistent.value("exportRetentionStatus")=="window_data_incomplete"&&
        windowManifest(inconsistentZip).value("traceIncomplete").toBool());
    auto reversed=scopedWindows;
    auto badBounds=reversed[0].toObject();
    badBounds.insert("startNs",badBounds.value("endNs").toInteger()+1);
    reversed[0]=badBounds;
    write(scopedRun+"/looplog-summary.json",QJsonDocument(QJsonObject{
        {"mode","rolling-burst-windows"},{"recordBytes",int(sizeof(keep))},{"recordsIssued",6},
        {"recordsWritten",6},{"windows",reversed}}).toJson());
    const auto reversedZip=exportScoped(50,"scoped-reversed.zip");
    const auto reversedSummary=readLoopSummary(reversedZip,"listener");
    const auto reversedWindow=reversedSummary.value("exportWindows").toArray().first().toObject();
    require(reversedWindow.value("status")=="invalid_manifest" &&
        reversedWindow.value("inRequestedWindow").isNull() &&
        windowManifest(reversedZip).value("traceIncomplete").toBool());
    // The same wall anchor from an older producer cannot prove a run started
    // there. Retain its cumulative loss and leave the in-window count unknown.
    write(afterRange+"/run-config.json",QJsonDocument(QJsonObject{
        {"runId","after-range"},{"wallAnchorMs",scopedWall.addSecs(63).toMSecsSinceEpoch()},
        {"monotonicAnchorNs","1000000"},{"diagnosticTraceEnabled",false},{"loopLogEnabled",true}}).toJson());
    const auto unknownRange=exportScoped(50,"unknown-legacy-range.zip");
    const auto unknownSummary=readLoopSummary(unknownRange,"after-range");
    require(unknownSummary.value("queueDropped").toInt()==9 &&
        unknownSummary.value("queueDropScope")=="source_run" &&
        unknownSummary.value("queueDroppedInRequestedWindow").isNull() &&
        unknownSummary.value("loopLogIncomplete").toBool());

    const auto liveRoot=root.filePath("live-burst");
    const auto liveRun=liveRoot+"/listener";
    require(QDir().mkpath(liveRun));
    write(liveRun+"/run-config.json",QJsonDocument(QJsonObject{
        {"runId","listener"},{"wallAnchorMs",double(wall-6000)},
        {"monotonicAnchorNs","1000000"},{"diagnosticTraceEnabled",false},
        {"timingEnabled",false},{"loopLogEnabled",true}}).toJson());
    paimage::installTraceBundle(liveRoot,tools);
    {
        QLockFile listening(liveRun+"/.active.lock");
        require(listening.tryLock(0));
        paimage::LoopLog live(std::filesystem::path(liveRun.toStdWString()),paimage::LoopLog::Budget{64});
        live.push(firstLoop);
        auto liveBurst=burst;
        liveBurst.timeNs=firstLoop.timeNs+5000000000ull;
        live.push(liveBurst);
        const auto liveStart=QDateTime::fromMSecsSinceEpoch(wall-7000);
        auto exportLive=[&](const QString& path){
            require(DiagnosticRecorder::exportTimeWindow(recorder.registerTimeWindowRequest(liveStart,end),path).success);
            require(!QFileInfo::exists(liveRun+"/looplog-summary.json"));
            return zipEntries(path);
        };
        const auto liveZip=exportLive(root.filePath("live.zip"));
        const auto liveSummary=readLoopSummary(liveZip,"listener");
        require(liveSummary.value("exportCountCovered").toBool()&&
            !liveSummary.value("loopLogIncomplete").toBool(true)&&
            liveSummary.value("sourceRecordsWritten").toInt()==2);
        require(liveSummary.value("exportWindows").toArray().first().toObject().value("status")=="verified");
        require(!windowManifest(liveZip).value("traceIncomplete").toBool(true));
        require(QFile::remove(liveRun+"/looplog-window-1.bin"));
        const auto missingZip=exportLive(root.filePath("live-missing.zip"));
        const auto missingSummary=readLoopSummary(missingZip,"listener");
        require(missingSummary.value("loopLogIncomplete").toBool()&&
            !missingSummary.value("exportCountCovered").toBool(true));
        require(missingSummary.value("exportWindows").toArray().first().toObject().value("status")=="missing_or_unreadable");
        require(windowManifest(missingZip).value("traceIncomplete").toBool()&&
            windowManifest(missingZip).value("skippedRuns").toArray().size()==1);
    }

    const auto pendingRoot=root.filePath("open-window");
    const auto pendingRun=pendingRoot+"/listener";
    require(QDir().mkpath(pendingRun));
    const auto pendingWall=QDateTime::currentDateTime();
    const auto pendingClock=std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    write(pendingRun+"/run-config.json",QJsonDocument(QJsonObject{
        {"runId","listener"},{"wallAnchorMs",pendingWall.toMSecsSinceEpoch()},
        {"monotonicAnchorNs",QString::number(pendingClock)},{"diagnosticTraceEnabled",false},
        {"timingEnabled",false},{"loopLogEnabled",true}}).toJson());
    paimage::installTraceBundle(pendingRoot,tools);
    {
        paimage::LoopLog live(std::filesystem::path(pendingRun.toStdWString()),paimage::LoopLog::Budget{64});
        auto history=keep;
        history.timeNs=pendingClock-6000000000ull;
        live.push(history);
        auto trigger=burst;
        trigger.timeNs=history.timeNs+5000000000ull;
        live.push(trigger);
        const auto registerPending=[&]{
            return recorder.registerTimeWindowRequest(pendingWall.addSecs(-7),pendingWall.addSecs(10));
        };
        const auto before=registerPending(), after=registerPending();
        history.timeNs=trigger.timeNs+1;
        live.push(history);
        const auto beforePath=root.filePath("open-window.zip");
        require(DiagnosticRecorder::exportTimeWindow(before,beforePath).success);
        const auto open=readLoopSummary(zipEntries(beforePath),"listener");
        require(open.value("openWindowPending").toBool() &&
            open.value("exportRetentionStatus")=="open_window_pending" && open.value("recordsWritten").toInt(-1)==0);
        const auto frozenFile=pendingRun+"/looplog-window-1.bin";
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(6);
        while(!QFileInfo::exists(frozenFile) && std::chrono::steady_clock::now()<deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        // Inventory publication follows the atomic file rename. Wait for the
        // inventory explicitly so this test does not race those two steps.
        const auto cuts=paimage::LoopLog::captureCuts();
        for (const auto& cut : cuts)
            if (cut.root==std::filesystem::path(pendingRun.toStdWString()))
                while (cut.windowInventory().find("\"openWindowPending\":true")!=std::string::npos &&
                       std::chrono::steady_clock::now()<deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
        const auto afterPath=root.filePath("frozen-window.zip");
        require(DiagnosticRecorder::exportTimeWindow(after,afterPath).success);
        const auto frozen=readLoopSummary(zipEntries(afterPath),"listener");
        require(!frozen.value("openWindowPending").toBool(true) &&
            frozen.value("exportRetentionStatus")=="windows_retained" &&
            frozen.value("recordsWritten").toInt()==2 && frozen.value("exportAfterCutRecords").toInt()==1);
    }

    for (const auto& scenario : {QStringLiteral("truncated"), QStringLiteral("prehistory"),
                                 QStringLiteral("budget_omitted"), QStringLiteral("write_failed")}) {
        const bool hasPrehistory = scenario == "prehistory";
        const auto status = hasPrehistory ? QStringLiteral("truncated") : scenario;
        const auto cutRoot = root.filePath("cut-" + scenario);
        const auto cutRun = cutRoot + "/listener";
        require(QDir().mkpath(cutRun));
        write(cutRun+"/run-config.json",QJsonDocument(QJsonObject{
            {"runId","listener"},{"wallAnchorMs",double(wall-6000)},
            {"monotonicAnchorNs","1000000"},{"diagnosticTraceEnabled",false},
            {"timingEnabled",false},{"loopLogEnabled",true}}).toJson());
        paimage::installTraceBundle(cutRoot,tools);
        paimage::LoopLog::Budget budget{64};
        if (status == "budget_omitted") budget.totalBytes = sizeof(paimage::LoopRecord);
        if (status == "write_failed") require(QDir().mkpath(cutRun+"/looplog-window-1.bin"));
        paimage::LoopLog live(std::filesystem::path(cutRun.toStdWString()),budget);
        const auto cutStart = QDateTime::fromMSecsSinceEpoch(wall-7000);
        if (hasPrehistory) live.push(firstLoop);
        const auto beforeBurst = recorder.registerTimeWindowRequest(cutStart,end);
        const auto beforeBurstThenStopped = recorder.registerTimeWindowRequest(cutStart,end);
        if (!hasPrehistory) live.push(firstLoop);
        live.push(burst);
        const auto newer = recorder.registerTimeWindowRequest(cutStart,end);
        const auto cuts = paimage::LoopLog::captureCuts();
        for (const auto& cut : cuts)
            if (cut.root == std::filesystem::path(cutRun.toStdWString()))
                cut.flush();

        auto verifyBeforeBurst = [&](const auto& request, const QString& path) {
            require(DiagnosticRecorder::exportTimeWindow(request,path).success);
            const auto zip = zipEntries(path);
            const auto scoped = readLoopSummary(zip,"listener");
            require(scoped.value("recordsIssued").toInt(-1)==(hasPrehistory?1:0) &&
                scoped.value("recordsWritten").toInt(-1)==0 &&
                scoped.value("sourceRecordsWritten").toInt(-1)==0);
            require(scoped.value("exportWindows").toArray().isEmpty() &&
                scoped.value("exportRetentionStatus")=="no_burst" &&
                !scoped.value("openWindowPending").toBool(true) &&
                !scoped.value("loopLogIncomplete").toBool(true) &&
                !scoped.value("budgetExhausted").toBool(true) &&
                !scoped.value("writeFailed").toBool(true));
            const auto manifest = windowManifest(zip);
            require(!manifest.value("traceIncomplete").toBool(true) &&
                manifest.value("skippedRuns").toArray().isEmpty());
        };
        verifyBeforeBurst(beforeBurst,root.filePath(scenario+"-early-live.zip"));
        live.stop();
        verifyBeforeBurst(beforeBurstThenStopped,root.filePath(scenario+"-early-stopped.zip"));

        const auto newerPath = root.filePath(scenario+"-newer.zip");
        require(DiagnosticRecorder::exportTimeWindow(newer,newerPath).success);
        const auto newerZip = zipEntries(newerPath);
        const auto newerSummary = readLoopSummary(newerZip,"listener");
        const auto windows = newerSummary.value("exportWindows").toArray();
        require(windows.size()==1 && windows.first().toObject().value("status")==status &&
            newerSummary.value("windows").toArray().first().toObject().value("triggerSequence").toInt()==2 &&
            newerSummary.value("loopLogIncomplete").toBool() &&
            windowManifest(newerZip).value("traceIncomplete").toBool());
    }

    const auto lossRoot = root.filePath("cut-loss");
    const auto lossRun = lossRoot + "/listener";
    require(QDir().mkpath(lossRun));
    write(lossRun+"/run-config.json",QJsonDocument(QJsonObject{
        {"runId","listener"},{"wallAnchorMs",double(wall-6000)},
        {"monotonicAnchorNs","1000000"},{"diagnosticTraceEnabled",false},
        {"timingEnabled",false},{"loopLogEnabled",true}}).toJson());
    paimage::installTraceBundle(lossRoot,tools);
    {
        std::promise<void> releaseWriter;
        auto release = releaseWriter.get_future();
        paimage::LoopLog::Budget budget{64};
        budget.beforeDrain = [&]{ if (release.valid()) release.get(); };
        paimage::LoopLog live(std::filesystem::path(lossRun.toStdWString()),budget);
        live.push(firstLoop);
        auto marker = burst;
        marker.timeNs = firstLoop.timeNs + 5000000000ull;
        live.push(marker);
        const auto lossStart = QDateTime::fromMSecsSinceEpoch(wall-7000);
        const auto beforeLoss = recorder.registerTimeWindowRequest(lossStart,end);
        auto outside = firstLoop;
        outside.timeNs = marker.timeNs + 5000000001ull;
        for (int i=0;i<80;++i) live.push(outside);
        const auto afterLoss = recorder.registerTimeWindowRequest(lossStart,end);
        releaseWriter.set_value();
        live.stop();
        require(live.dropped()==18);

        const auto beforePath = root.filePath("before-loss.zip");
        require(DiagnosticRecorder::exportTimeWindow(beforeLoss,beforePath).success);
        const auto before = zipEntries(beforePath);
        const auto summary = readLoopSummary(before,"listener");
        const auto metadata = summary.value("windows").toArray().first().toObject();
        require(summary.value("recordsWritten").toInt()==2 &&
            summary.value("queueDropped").toInt(-1)==0 &&
            metadata.value("queueDroppedAtOpen").toInt(-1)==0 &&
            !metadata.value("preWindowTruncated").toBool(true) &&
            summary.value("exportRetentionStatus")=="windows_retained" &&
            !windowManifest(before).value("traceIncomplete").toBool(true));
        const auto afterPath = root.filePath("after-loss.zip");
        require(DiagnosticRecorder::exportTimeWindow(afterLoss,afterPath).success);
        const auto after = zipEntries(afterPath);
        require(readLoopSummary(after,"listener").value("queueDropped").toInt()==18 &&
            windowManifest(after).value("traceIncomplete").toBool());
    }

    const auto mergedRoot = root.filePath("cut-merged");
    const auto mergedRun = mergedRoot + "/listener";
    require(QDir().mkpath(mergedRun));
    const auto clockNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto clockWall = QDateTime::currentDateTime();
    write(mergedRun+"/run-config.json",QJsonDocument(QJsonObject{
        {"runId","listener"},{"wallAnchorMs",double(clockWall.toMSecsSinceEpoch())},
        {"monotonicAnchorNs",QString::number(clockNs)},{"diagnosticTraceEnabled",false},
        {"timingEnabled",false},{"loopLogEnabled",true}}).toJson());
    paimage::installTraceBundle(mergedRoot,tools);
    {
        std::promise<void> releaseWriter;
        auto release = releaseWriter.get_future();
        paimage::LoopLog::Budget budget{64};
        budget.beforeDrain = [&]{ if (release.valid()) release.get(); };
        budget.windowRecordLimit = 3;
        paimage::LoopLog live(std::filesystem::path(mergedRun.toStdWString()),budget);
        auto history = firstLoop;
        history.timeNs = clockNs - 10000000000ull;
        live.push(history);
        auto marker = burst;
        marker.timeNs = history.timeNs + 5000000000ull;
        marker.value0 = 1;
        live.push(marker);
        const auto beforeMerge = recorder.registerTimeWindowRequest(clockWall.addSecs(-11),clockWall.addSecs(2));
        marker.timeNs += 2000000000ull;
        marker.value0 = 2;
        live.push(marker);
        auto tail = history;
        tail.timeNs = clockNs;
        live.push(tail);
        const auto afterMerge = recorder.registerTimeWindowRequest(clockWall.addSecs(-11),clockWall.addSecs(2));
        releaseWriter.set_value();
        // The source reaches the original window end but stops before the
        // post-click marker's extended end.
        live.stop();
        const auto verifyMerge = [&](const auto& cut, const QString& filename, bool extended) {
            const auto path = root.filePath(filename);
            require(DiagnosticRecorder::exportTimeWindow(cut,path).success);
            const auto zip = zipEntries(path);
            const auto summary = readLoopSummary(zip,"listener");
            const auto window = summary.value("windows").toArray().first().toObject();
            require(window.value("endNs").toInteger()==clockNs+(extended?2000000000ll:0) &&
                window.value("burstEpochs").toArray().size()==(extended?2:1) &&
                window.value("postWindowTruncated").toBool()==extended &&
                window.value("windowTruncated").toBool()==extended &&
                summary.value("recordsWritten").toInt()==(extended?3:2) &&
                windowManifest(zip).value("traceIncomplete").toBool()==extended);
        };
        verifyMerge(beforeMerge,"before-merge.zip",false);
        verifyMerge(afterMerge,"after-merge.zip",true);
    }

    if (app.arguments().size()==3) testBundledAnalyzers(root,recorder,app.arguments());
    DiagnosticRecorder::setBundleCapture({});
    std::cout<<"PASS trace boundaries, stop/prune handoff and LoopLog burst window integrity"<<std::endl;
}
