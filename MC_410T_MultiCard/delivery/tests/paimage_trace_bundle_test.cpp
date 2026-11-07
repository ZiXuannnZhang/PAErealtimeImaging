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
#include <cstring>
#include <iostream>
#include <stdexcept>
void require(bool b){if(!b)throw std::runtime_error("fixed trace export boundary");}
void write(const QString& path,const QByteArray& data){QFile f(path);require(f.open(QIODevice::WriteOnly));require(f.write(data)==data.size());}
quint32 number(const QByteArray& b,int at,int n){quint32 value=0;std::memcpy(&value,b.constData()+at,n);return value;}
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
    paimage::installTraceBundle(traces,tools);
    DiagnosticRecorder::Options options;options.rootDirectory=root.path()+"/logs";DiagnosticRecorder recorder(options);
    auto cut=recorder.captureTimeWindowRequest(QDateTime::fromMSecsSinceEpoch(wall-1000),QDateTime::fromMSecsSinceEpoch(wall+1000));
    t.correlation=30;timer.observe(t,true);
    auto later=first;later.correlation=202;writer.push(later);
    auto laterLoop=firstLoop;laterLoop.timeNs=1000000;loopLog.push(laterLoop);
    auto result=DiagnosticRecorder::exportTimeWindow(cut,root.path()+"/export.zip");require(result.success);
    QFile zip(root.path()+"/export.zip");require(zip.open(QIODevice::ReadOnly));auto bytes=zip.readAll();QHash<QString,QByteArray> entries;
    for(int at=0;at+30<=bytes.size()&&number(bytes,at,4)==0x04034b50;){
        int size=number(bytes,at+18,4),name=number(bytes,at+26,2),extra=number(bytes,at+28,2),data=at+30+name+extra;
        require(data+size<=bytes.size());QByteArray payload;
        require(decodeZipPayload(bytes.mid(data,size),number(bytes,at+8,2),number(bytes,at+22,4),payload));
        entries.insert(QString::fromUtf8(bytes.mid(at+30,name)),payload);at=data+size;
    }
    auto trace=entries.value("paimage/run1/trace-0.bin");require(trace.size()==64);paimage::TraceRecord record;std::memcpy(&record,trace.constData(),64);
    require(record.sequence==1&&record.correlation==101);
    require(entries.value("paimage/run1/looplog-0.bin").size()==160);
    auto loopSummary=QJsonDocument::fromJson(entries.value("paimage/run1/looplog-summary.json")).object();
    require(loopSummary.value("recordsWritten").toInt()==2&&!loopSummary.value("loopLogIncomplete").toBool(true));
    auto manifest=QJsonDocument::fromJson(entries.value("paimage/manifest.json")).object();
    require(!manifest.value("traceIncomplete").toBool(true)&&manifest.value("runs").toArray().size()==3);
    require(entries.value("paimage/run2/trace-0.bin").size()==64);
    const auto only=entries.value("paimage/timing-only/timing-0.bin");
    require(only.size()==sizeof(paimage::TimingRecord));
    paimage::TimingRecord timed{};std::memcpy(&timed,only.constData(),sizeof(timed));
    require(timed.correlation==20&&entries.contains("paimage/timing-only/run-config.json"));
    const auto timingSummary=QJsonDocument::fromJson(entries.value("paimage/timing-only/timing-summary.json")).object();
    require(timingSummary.value("recordsWritten").toInt()==1&&timingSummary.value("exportFilteredRecords").toInt()==1);
    auto failed=DiagnosticRecorder::exportTimeWindow(cut,root.path());require(!failed.success);
    auto retried=DiagnosticRecorder::exportTimeWindow(cut,root.path()+"/retry.zip");require(retried.success);
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
    std::cout<<"PASS active writer flush, fixed sequence cut, excluded post-click record with old timestamp, ZIP manifest"<<std::endl;
}
