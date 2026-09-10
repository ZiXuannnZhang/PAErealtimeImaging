#include "PaimageAcquisition/TraceBundle.h"
#include "PaimageAcquisition/TraceWriter.h"
#include "PaimageAcquisition/TimingWriter.h"
#include "DiagnosticRecorder.h"
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
        auto cuts=TraceWriter::captureCuts();
        auto timingCuts=TimingWriter::captureCuts();
        auto dirs=QDir(root).entryList(QDir::Dirs|QDir::NoDotAndDotDot,QDir::Name);
        return [root,toolsDirectory,start,end,cuts=std::move(cuts),timingCuts=std::move(timingCuts),dirs=std::move(dirs)]
            (const DiagnosticRecorder::BundleSink& sink,QString* error){
            QJsonArray runs,files;bool bundleIncomplete=false;
            auto write=[&](const QString& name,const QByteArray& bytes){
                if(!sink(name,bytes)){if(error&&error->isEmpty())*error="PAimage trace ZIP write failed";return false;}
                files.append(QJsonObject{{"path",name},{"bytes",double(bytes.size())},
                    {"sha256",QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex())}});return true;
            };
            for(const auto& dir:dirs){
                const QString path=QDir(root).filePath(dir);auto metadata=object(QDir(path).filePath("run-config.json"));
                if(metadata.isEmpty())continue;
                const qint64 wall=metadata.value("wallAnchorMs").toVariant().toLongLong();
                const qint64 mono=metadata.value("monotonicAnchorNs").toString().toLongLong();
                bool anchorKnown=wall&&mono;
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
                bool timingFlushed=true;quint64 timingBoundary=0;for(const auto& cut:timingCuts)if(QDir::fromNativeSeparators(QString::fromStdWString(cut.root.wstring()))==QDir::fromNativeSeparators(path)){timingBoundary=cut.sequence;timingFlushed=cut.flush();break;}
                auto timingSummary=object(QDir(path).filePath("timing-summary.json"));
                if(timingSummary.isEmpty()&&timingBoundary)incomplete=true;
                auto timingNames=QDir(path).entryList({"timing-*.bin"},QDir::Files,QDir::Name);int timingPart=0;quint64 timingSelected=0;
                for(const auto& name:timingNames){QByteArray bytes=read(QDir(path).filePath(name)),chosen;if(bytes.size()%64)incomplete=true;for(qsizetype at=0;at+64<=bytes.size();at+=64){TimingRecord r;std::memcpy(&r,bytes.constData()+at,64);if(timingBoundary&&r.sequence>timingBoundary)continue;chosen.append(bytes.constData()+at,64);++timingSelected;}if(!chosen.isEmpty()){const QString out=QString("paimage/%1/timing-%2.bin").arg(dir).arg(timingPart++);if(!write(out,chosen))return false;runFiles.append(out);}}
                if(!selected)continue;
                if(!boundary)boundary=maxSequence;
                incomplete=incomplete||filtered>0||selected!=boundary;bundleIncomplete|=incomplete;
                auto exported=summary;exported.insert("recordsIssued",double(boundary));exported.insert("recordsWritten",double(selected));
                exported.insert("traceIncomplete",incomplete);exported.insert("exportClipped",filtered>0);
                exported.insert("exportFilteredRecords",double(filtered));exported.insert("exportFlushCompleted",flushed);
                const QString prefix="paimage/"+dir+"/";
                if(!write(prefix+"run-config.json",QJsonDocument(metadata).toJson())||
                   !write(prefix+"trace-summary.json",QJsonDocument(exported).toJson()))return false;
                if(!timingSummary.isEmpty()){timingSummary.insert("exportFlushCompleted",timingFlushed);timingSummary.insert("recordsWritten",double(timingSelected));timingSummary.insert("timingIncomplete",timingSummary.value("timingIncomplete").toBool()||!timingFlushed||(timingBoundary&&timingSelected!=timingBoundary));if(!write(prefix+"timing-summary.json",QJsonDocument(timingSummary).toJson()))return false;}
                runs.append(QJsonObject{{"runId",metadata.value("runId")},{"recordBoundary",double(boundary)},
                    {"recordsIncluded",double(selected)},{"recordsOutsideWindow",double(filtered)},
                    {"traceIncomplete",incomplete},{"anchorKnown",anchorKnown},{"files",runFiles},
                    {"firstIncludedMs",anchorKnown?QJsonValue(double(firstMs)):QJsonValue()},
                    {"lastIncludedMs",anchorKnown?QJsonValue(double(lastMs)):QJsonValue()}});
            }
            for(const auto& name:{QString("paimage_trace_analyze.py"),QString("paimage-trace-schema.md"),QString("receiver_system_capture.ps1")}){
                auto bytes=read(QDir(toolsDirectory).filePath(name));
                // An optional system helper is not evidence of lost application records.
                if(bytes.isEmpty()){if(name!="receiver_system_capture.ps1")bundleIncomplete=true;}else if(!write("paimage/tools/"+name,bytes))return false;
            }
            QJsonObject manifest{{"schemaVersion",2},{"backendId","paimage-derived"},
                {"requestedStartMs",double(start)},{"fixedEndMs",double(end)},{"traceIncomplete",bundleIncomplete},
                {"runs",runs},{"files",files},{"coverageNote","Window clipping is explicit; missing pre-window object anchors remain unknown. Callback returns do not prove disk durability or completed reconstruction."}};
            return sink("paimage/manifest.json",QJsonDocument(manifest).toJson());
        };
    });
}
}
