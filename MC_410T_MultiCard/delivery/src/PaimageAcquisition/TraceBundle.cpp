#include "PaimageAcquisition/TraceBundle.h"
#include "PaimageAcquisition/TraceWriter.h"
#include "PaimageAcquisition/TimingWriter.h"
#include "PaimageAcquisition/LoopLog.h"
#include "DiagnosticRecorder.h"
#include "DiagnosticStorage.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QCryptographicHash>
#include <cstring>
#include <limits>
namespace paimage {
namespace {
QByteArray read(const QString& name){QFile f(name);return f.open(QIODevice::ReadOnly)?f.readAll():QByteArray{};}
QJsonObject object(const QString& name){return QJsonDocument::fromJson(read(name)).object();}
}
void installTraceBundle(const QString& root,const QString& toolsDirectory){
    DiagnosticRecorder::setBundleCapture([root,toolsDirectory](qint64 start,qint64 end){
        // Capture just identities and sequence fences on the UI thread. File
        // reads, writer flush waits, filtering and hashing run in the exporter.
        const bool rootExists=QDir(root).exists();
        const auto lease=rootExists ? diagnostic_storage::exportLease(root) : nullptr;
        if(rootExists&&!lease)
            return DiagnosticRecorder::BundleExtension([](const auto&,QString* error){
                if(error)*error=QStringLiteral("诊断目录正在清理，请重试导出");return false;
            });
        auto cuts=TraceWriter::captureCuts();
        auto timingCuts=TimingWriter::captureCuts();
        auto loopCuts=LoopLog::captureCuts();
        auto dirs=QDir(root).entryList(QDir::Dirs|QDir::NoDotAndDotDot,QDir::Name);
        return DiagnosticRecorder::BundleExtension([root,toolsDirectory,start,end,lease,cuts=std::move(cuts),timingCuts=std::move(timingCuts),loopCuts=std::move(loopCuts),dirs=std::move(dirs)]
            (const DiagnosticRecorder::BundleSink& sink,QString* error){
            QJsonArray runs,files,skippedRuns;bool bundleIncomplete=false;
            auto write=[&](const QString& name,const QByteArray& bytes){
                if(!sink(name,bytes)){if(error&&error->isEmpty())*error="PAimage trace ZIP write failed";return false;}
                files.append(QJsonObject{{"path",name},{"bytes",double(bytes.size())},
                    {"sha256",QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex())}});return true;
            };
            for(const auto& dir:dirs){
                const auto runInfo=QFileInfo(QDir(root).filePath(dir));
                if(runInfo.isSymLink()||runInfo.isJunction())continue;
                const QString path=QDir(root).filePath(dir);auto metadata=object(QDir(path).filePath("run-config.json"));
                if(metadata.isEmpty())continue;
                const qint64 wall=metadata.value("wallAnchorMs").toVariant().toLongLong();
                const qint64 mono=metadata.value("monotonicAnchorNs").toString().toLongLong();
                bool anchorKnown=wall&&mono;
                if(!anchorKnown && start!=std::numeric_limits<qint64>::min()){
                    const auto binary=QDir(path).entryList({"trace-*.bin","timing-*.bin","looplog-*.bin"},QDir::Files);
                    if(!binary.isEmpty()){
                        skippedRuns.append(QJsonObject{{"runId",metadata.value("runId")},{"reason","missing_time_anchor"}});
                        bundleIncomplete=true;
                    }
                    continue;
                }
                auto summary=object(QDir(path).filePath("trace-summary.json"));
                quint64 boundary=quint64(summary.value("recordsIssued").toDouble());bool flushed=!summary.isEmpty(),boundaryKnown=flushed;
                for(const auto& cut:cuts)if(QDir::fromNativeSeparators(QString::fromStdWString(cut.root.wstring()))==QDir::fromNativeSeparators(path)){boundary=cut.sequence;boundaryKnown=true;flushed=cut.flush();break;}
                bool incomplete=!flushed||!anchorKnown||summary.value("traceIncomplete").toBool(false);
                quint64 selected=0,filtered=0,maxSequence=0;int part=0;
                qint64 firstMs=std::numeric_limits<qint64>::max(),lastMs=std::numeric_limits<qint64>::min();
                QJsonArray runFiles;
                auto names=QDir(path).entryList({"trace-*.bin"},QDir::Files,QDir::Name);
                std::sort(names.begin(),names.end(),[](const QString& a,const QString& b){return a.mid(6).section('.',0,0).toInt()<b.mid(6).section('.',0,0).toInt();});
                for(const auto& name:names){
                    QByteArray bytes=read(QDir(path).filePath(name)),chosen;chosen.reserve(bytes.size());
                    if(bytes.size()%64)incomplete=true;
                    for(qsizetype at=0;at+64<=bytes.size();at+=64){TraceRecord r;std::memcpy(&r,bytes.constData()+at,64);
                        if(boundaryKnown&&r.sequence>boundary)continue;maxSequence=std::max(maxSequence,quint64(r.sequence));
                        const qint64 time=anchorKnown?wall+(qint64(r.monotonicNs)-mono)/1000000:0;
                        if(anchorKnown&&(time<start||time>end)){++filtered;continue;}
                        chosen.append(bytes.constData()+at,64);++selected;
                        if(anchorKnown){firstMs=std::min(firstMs,time);lastMs=std::max(lastMs,time);}
                    }
                    if(!chosen.isEmpty()){const QString out=QString("paimage/%1/trace-%2.bin").arg(dir).arg(part++);
                        if(!write(out,chosen))return false;runFiles.append(out);}
                }
                auto timingSummary=object(QDir(path).filePath("timing-summary.json"));
                bool timingFlushed=true, timingLive=false;
                quint64 timingBoundary=0;
                for(const auto& cut:timingCuts)
                    if(QDir::fromNativeSeparators(QString::fromStdWString(cut.root.wstring()))==QDir::fromNativeSeparators(path)){
                        timingLive=true;timingBoundary=cut.sequence;timingFlushed=cut.flush();break;
                    }
                auto timingNames=QDir(path).entryList({"timing-*.bin"},QDir::Files,QDir::Name);
                std::sort(timingNames.begin(),timingNames.end(),[](const QString& a,const QString& b){
                    return a.mid(7).section('.',0,0).toInt()<b.mid(7).section('.',0,0).toInt();
                });
                int timingPart=0;quint64 timingSelected=0,timingFiltered=0;
                bool timingBad=false;
                for(const auto& name:timingNames){
                    const QByteArray bytes=read(QDir(path).filePath(name));
                    QByteArray chosen;
                    if(bytes.size()%sizeof(TimingRecord))timingBad=true;
                    for(qsizetype at=0;at+qsizetype(sizeof(TimingRecord))<=bytes.size();at+=sizeof(TimingRecord)){
                        TimingRecord r;std::memcpy(&r,bytes.constData()+at,sizeof(r));
                        if(timingLive&&r.sequence>timingBoundary)continue;
                        const auto first=qMin(r.startNs,r.endNs),last=qMax(r.startNs,r.endNs);
                        if(anchorKnown && (wall+(qint64(last)-mono)/1000000<start ||
                                           wall+(qint64(first)-mono)/1000000>end)){
                            ++timingFiltered;continue;
                        }
                        chosen.append(bytes.constData()+at,sizeof(r));++timingSelected;
                    }
                    if(!chosen.isEmpty()){
                        const auto out=QString("paimage/%1/timing-%2.bin").arg(dir).arg(timingPart++);
                        if(!write(out,chosen))return false;runFiles.append(out);
                    }
                }
                const QString prefix="paimage/"+dir+"/";
                bool loopFlushed=true,loopIncomplete=false,loopBudgetExhausted=false,loopWriteFailed=false;
                quint64 loopBoundary=0,loopDropped=0,loopFreezeEpoch=0; bool loopLive=false;
                for(const auto& cut:loopCuts)if(QDir::fromNativeSeparators(QString::fromStdWString(cut.root.wstring()))==QDir::fromNativeSeparators(path)){
                    loopLive=true;loopBoundary=cut.sequence;loopDropped=cut.dropped;loopFreezeEpoch=cut.freezeEpoch;
                    loopIncomplete=cut.incomplete;loopBudgetExhausted=cut.budgetExhausted;loopWriteFailed=cut.writeFailed;
                    loopFlushed=cut.flush();break;}
                auto loopSummary=object(QDir(path).filePath("looplog-summary.json"));
                if(loopSummary.isEmpty()&&loopBoundary)loopSummary=QJsonObject{{"schemaVersion",1},{"recordBytes",80},
                    {"recordsIssued",double(loopBoundary)},{"queueDropped",double(loopDropped)},
                    {"budgetExhausted",loopBudgetExhausted},{"writeFailed",loopWriteFailed},
                    {"burstMarkEpochs",double(loopFreezeEpoch)}};
                auto loopNames=QDir(path).entryList({"looplog-*.bin"},QDir::Files,QDir::Name);
                std::sort(loopNames.begin(),loopNames.end(),[](const QString& a,const QString& b){
                    return a.section('-',-1).section('.',0,0).toInt()<b.section('-',-1).section('.',0,0).toInt();
                });
                int loopPart=0;quint64 loopSelected=0,loopFiltered=0;
                for(const auto& name:loopNames){
                    QByteArray bytes=read(QDir(path).filePath(name)),chosen;if(bytes.size()%80)incomplete=true;
                    for(qsizetype at=0;at+80<=bytes.size();at+=80){LoopRecord r;std::memcpy(&r,bytes.constData()+at,80);
                        if(loopLive&&r.sequence>loopBoundary)continue;
                        const qint64 time=anchorKnown?wall+(qint64(r.timeNs)-mono)/1000000:0;
                        if(anchorKnown&&(time<start||time>end)){++loopFiltered;continue;}
                        chosen.append(bytes.constData()+at,80);++loopSelected;}
                    if(!chosen.isEmpty()){const QString out=QString("paimage/%1/looplog-%2.bin").arg(dir).arg(loopPart++);
                        if(!write(out,chosen))return false;runFiles.append(out);}}
                if(!selected&&!timingSelected&&!loopSelected)continue;
                const bool traceExpected=metadata.value("diagnosticTraceEnabled").toBool(true);
                if(!boundary)boundary=maxSequence;
                const bool traceIncomplete=traceExpected&&(!flushed||!anchorKnown||
                    summary.value("traceIncomplete").toBool()||filtered>0||selected!=boundary);
                incomplete=traceIncomplete;
                if(!write(prefix+"run-config.json",QJsonDocument(metadata).toJson()))return false;
                if(traceExpected){
                    auto exported=summary;
                    exported.insert("recordsIssued",double(boundary));
                    exported.insert("recordsWritten",double(selected));
                    exported.insert("traceIncomplete",traceIncomplete);
                    exported.insert("exportClipped",filtered>0);
                    exported.insert("exportFilteredRecords",double(filtered));
                    exported.insert("exportFlushCompleted",flushed);
                    if(!write(prefix+"trace-summary.json",QJsonDocument(exported).toJson()))return false;
                }
                if(timingSelected){
                    if(timingLive)timingSummary.insert("recordsIssued",double(timingBoundary));
                    timingSummary.insert("exportFlushCompleted",timingFlushed);
                    timingSummary.insert("recordsWritten",double(timingSelected));
                    timingSummary.insert("exportFilteredRecords",double(timingFiltered));
                    const bool timingIncomplete=timingSummary.value("timingIncomplete").toBool()||
                        !timingFlushed||timingBad||!anchorKnown||
                        (timingLive&&timingSelected+timingFiltered!=timingBoundary);
                    timingSummary.insert("timingIncomplete",timingIncomplete);
                    incomplete|=timingIncomplete;
                    if(!write(prefix+"timing-summary.json",QJsonDocument(timingSummary).toJson()))return false;
                }
                if(!loopSummary.isEmpty()){loopSummary.insert("exportFlushCompleted",loopFlushed);
                    loopSummary.insert("recordsWritten",double(loopSelected));loopSummary.insert("exportFilteredRecords",double(loopFiltered));
                    loopSummary.insert("loopLogIncomplete",loopSummary.value("loopLogIncomplete").toBool(loopIncomplete)||loopIncomplete||!loopFlushed||(loopBoundary&&loopSelected+loopFiltered!=loopBoundary));
                    if(!write(prefix+"looplog-summary.json",QJsonDocument(loopSummary).toJson()))return false;}
                incomplete=incomplete||loopIncomplete||!loopFlushed||(loopBoundary&&loopSelected+loopFiltered!=loopBoundary);
                bundleIncomplete|=incomplete;
                runs.append(QJsonObject{{"runId",metadata.value("runId")},{"recordBoundary",double(boundary)},
                    {"recordsIncluded",double(selected)},{"recordsOutsideWindow",double(filtered)},
                    {"traceIncomplete",incomplete},{"traceEnabled",traceExpected},{"timingRecordsIncluded",double(timingSelected)},{"anchorKnown",anchorKnown},{"files",runFiles},
                    {"firstIncludedMs",anchorKnown?QJsonValue(double(firstMs)):QJsonValue()},
                    {"lastIncludedMs",anchorKnown?QJsonValue(double(lastMs)):QJsonValue()}});
            }
            for(const auto& name:{QString("paimage_trace_analyze.py"),QString("paimage-trace-schema.md"),
                                   QString("startup-looplog-schema.md"),QString("startup_diagnostics_analyze.py")}){
                auto bytes=read(QDir(toolsDirectory).filePath(name));
                if(bytes.isEmpty())bundleIncomplete=true;else if(!write("paimage/tools/"+name,bytes))return false;
            }
            QJsonObject manifest{{"schemaVersion",2},{"backendId","paimage-derived"},
                {"requestedStartMs",double(start)},{"fixedEndMs",double(end)},{"traceIncomplete",bundleIncomplete},
                {"runs",runs},{"skippedRuns",skippedRuns},{"files",files},{"coverageNote","Window clipping is explicit; missing pre-window object anchors remain unknown. Callback returns do not prove disk durability or completed reconstruction."}};
            return sink("paimage/manifest.json",QJsonDocument(manifest).toJson());
        });
    });
}
}
