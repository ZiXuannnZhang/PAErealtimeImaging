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
#include <QHash>
#include <QSet>
#include <cstring>
#include <limits>
#include <mutex>
#include <utility>
namespace paimage {
namespace {
QByteArray read(const QString& name){QFile f(name);return f.open(QIODevice::ReadOnly)?f.readAll():QByteArray{};}
QJsonObject object(const QString& name){return QJsonDocument::fromJson(read(name)).object();}
QString cutPath(const std::filesystem::path& path){
    return QDir::cleanPath(QDir(QString::fromStdWString(path.wstring())).absolutePath());
}

// All copies of a queued request share a single handoff (or cancellation).
struct TraceExportIntent {
    std::mutex mutex;
    std::shared_ptr<diagnostic_storage::ExportIntent> pending;

    std::shared_ptr<diagnostic_storage::ExportIntent> take(){
        std::lock_guard<std::mutex> lock(mutex);
        return std::exchange(pending,nullptr);
    }
};

struct LoopFileExport {
    qint64 bytes = -1;
    quint64 selected = 0, filtered = 0, afterCut = 0;
    QString exportedName;
    bool formatError = false;
};

struct LoopWindowCoverage {
    QJsonValue covered;
    QString status;
    QJsonArray windows;
    bool incomplete = false;
};

LoopWindowCoverage checkLoopWindows(const QJsonObject &summary,
                                     const QHash<QString, LoopFileExport> &files)
{
    LoopWindowCoverage result;
    if (!summary.value("windows").isArray()) {
        result.status = "window_inventory_unavailable";
        return result;
    }
    const auto windows = summary.value("windows").toArray();
    bool matches = summary.value("recordBytes").toInt() == sizeof(LoopRecord);
    bool budget = summary.value("budgetExhausted").toBool();
    bool writeFailure = summary.value("writeFailed").toBool();
    bool truncated = false;
    qint64 expectedRecords = 0;
    QSet<QString> expected;
    QSet<qint64> ids;
    for (const auto &entry : windows) {
        const auto window = entry.toObject();
        const auto id = window.value("windowId").toInteger(-1);
        const auto count = window.value("recordCount").toInteger(-1);
        const auto bytes = window.value("bytes").toInteger(-1);
        const auto name = window.value("file").toString();
        const auto canonical = QStringLiteral("looplog-window-%1.bin").arg(id);
        const bool isTruncated = window.value("preWindowTruncated").toBool() ||
            window.value("beforeListenerStart").toBool() || window.value("postWindowTruncated").toBool() ||
            window.value("windowTruncated").toBool();
        truncated |= isTruncated;
        QJsonObject detail{{"windowId", id}, {"sourceFile", name}, {"truncated", isTruncated}};
        QString status;
        if (id <= 0 || ids.contains(id) || count < 0 || bytes < 0 ||
            bytes % sizeof(LoopRecord) || bytes / qint64(sizeof(LoopRecord)) != count ||
            (!name.isEmpty() && name != canonical)) {
            status = "invalid_manifest";
            matches = false;
        } else if (window.value("budgetOmitted").toBool()) {
            status = "budget_omitted";
            budget = true;
        } else if (count == 0) {
            status = "not_retained_by_policy";
        } else if (name.isEmpty()) {
            status = "write_failed";
            writeFailure = true;
            matches = false;
        } else {
            expected.insert(name);
            if (count > (std::numeric_limits<qint64>::max)() - expectedRecords)
                matches = false;
            else
                expectedRecords += count;
            const auto file = files.value(name);
            detail.insert("exportFile", file.exportedName);
            detail.insert("exportRecords", qint64(file.selected));
            detail.insert("exportFilteredRecords", qint64(file.filtered));
            detail.insert("exportAfterCutRecords", qint64(file.afterCut));
            if (file.bytes < 0) status = "missing_or_unreadable";
            else if (file.bytes != bytes || file.formatError) status = "corrupt";
            else status = isTruncated ? "truncated" : "verified";
            matches &= file.bytes == bytes && !file.formatError;
        }
        ids.insert(id);
        detail.insert("status", status);
        result.windows.append(detail);
    }
    // A completed inventory must also account for every on-disk window.
    for (auto it = files.cbegin(); it != files.cend(); ++it)
        if (!expected.contains(it.key())) matches = false;
    if (summary.value("recordsWritten").isDouble())
        matches &= summary.value("recordsWritten").toInteger(-1) == expectedRecords;
    result.covered = matches;
    result.incomplete = !matches || budget || writeFailure || truncated;
    if (writeFailure) result.status = "write_failed";
    else if (budget) result.status = "budget_omitted";
    else if (!matches || truncated) result.status = "window_data_incomplete";
    else if (windows.isEmpty() && summary.value("burstMarkEpochs").toInteger() == 0) result.status = "no_burst";
    else if (files.isEmpty()) result.status = "not_retained_by_policy";
    else result.status = "windows_retained";
    return result;
}
}
void installTraceBundle(const QString& root,const QString& toolsDirectory){
    const auto traceRoot=QDir::cleanPath(QDir(root).absolutePath());
    DiagnosticRecorder::setBundleCapture([root=traceRoot,toolsDirectory](qint64 start,qint64 end){
        // Register before observing the live writers: a writer can stop as soon
        // as its fence is captured, making its run eligible for pruning.
        auto intent=std::make_shared<TraceExportIntent>();
        intent->pending=diagnostic_storage::registerExportIntent(root);
        if(!intent->pending){
            return DiagnosticRecorder::BundleExtension([](const DiagnosticRecorder::BundleSink&,QString* error){
                if(error)*error=QStringLiteral("无法保护 PAimage 诊断目录，请重试导出");
                return false;
            });
        }
        const auto capturedAt=QDateTime::currentMSecsSinceEpoch();
        const auto fixedEnd=qMin(end,capturedAt);
        // File reads, writer flush waits, filtering and hashing run in the exporter.
        auto cuts=TraceWriter::captureCuts();
        auto timingCuts=TimingWriter::captureCuts();
        auto loopCuts=LoopLog::captureCuts();
        return DiagnosticRecorder::BundleExtension([root,toolsDirectory,start,end=fixedEnd,capturedAt,intent=std::move(intent),cuts=std::move(cuts),timingCuts=std::move(timingCuts),loopCuts=std::move(loopCuts)]
            (const DiagnosticRecorder::BundleSink& sink,QString* error){
            auto pending=intent->take();
            if(!pending){
                if(error)*error=QStringLiteral("PAimage 诊断导出请求已使用，请重新发起导出");
                return false;
            }
            const QFileInfo rootInfo(root);
            const bool rootExists=rootInfo.exists();
            const auto lease=rootInfo.isDir()&&!rootInfo.isSymLink()&&!rootInfo.isJunction()
                ? diagnostic_storage::awaitExportLease(root) : nullptr;
            if(rootExists&&!lease){
                if(error)*error=QStringLiteral("无法锁定 PAimage 诊断目录，请重试导出");
                return false;
            }
            pending.reset();

            // Check captured identities independently of the directory listing.
            // Paths outside this root belong to a different trace bundle.
            QSet<QString> knownRuns;
            const auto rememberRuns=[&](const auto& streamCuts){
                for(const auto& cut:streamCuts){
                    const auto path=cutPath(cut.root);
                    if(QFileInfo(path).path()==root)knownRuns.insert(QFileInfo(path).fileName());
                }
            };
            rememberRuns(cuts);
            rememberRuns(timingCuts);
            rememberRuns(loopCuts);
            const QDir traceDir(root);
            const auto dirs=lease?traceDir.entryList(QDir::Dirs|QDir::NoDotAndDotDot,QDir::Name):QStringList{};
            QHash<QString,QJsonObject> knownMetadata;
            for(const auto& dir:knownRuns){
                const QString path=traceDir.filePath(dir);
                const QFileInfo runInfo(path),configInfo(QDir(path).filePath("run-config.json"));
                auto metadata=dirs.contains(dir)&&!runInfo.isSymLink()&&!runInfo.isJunction()&&
                    configInfo.isFile()&&!configInfo.isSymLink()&&!configInfo.isJunction()
                    ? object(configInfo.filePath()) : QJsonObject{};
                if(metadata.value("runId").toString()!=dir){
                    if(error)*error=QStringLiteral("无法读取已登记的 PAimage 诊断运行：%1").arg(dir);
                    return false;
                }
                knownMetadata.insert(dir,std::move(metadata));
            }
            QJsonArray runs,files,skippedRuns;bool bundleIncomplete=false;
            auto write=[&](const QString& name,const QByteArray& bytes){
                if(!sink(name,bytes)){if(error&&error->isEmpty())*error="PAimage trace ZIP write failed";return false;}
                files.append(QJsonObject{{"path",name},{"bytes",double(bytes.size())},
                    {"sha256",QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex())}});return true;
            };
            for(const auto& dir:dirs){
                const auto runInfo=QFileInfo(QDir(root).filePath(dir));
                if(runInfo.isSymLink()||runInfo.isJunction())continue;
                const QString path=traceDir.filePath(dir);
                const auto saved=knownMetadata.constFind(dir);
                auto metadata=saved==knownMetadata.cend()?object(QDir(path).filePath("run-config.json")):saved.value();
                if(metadata.isEmpty())continue;
                const auto enabledState=[&](const char* key){
                    const auto value=metadata.value(key);
                    return value.isBool()?value:QJsonValue(QJsonValue::Null);
                };
                const auto timingEnabled=enabledState("timingEnabled"),loopEnabled=enabledState("loopLogEnabled");
                const auto hasCut=[&](const auto& streamCuts){
                    for(const auto& cut:streamCuts)
                        if(cutPath(cut.root)==path)return true;
                    return false;
                };
                const bool timingEvidence=hasCut(timingCuts)||QFileInfo::exists(QDir(path).filePath("timing-summary.json"))||
                    !QDir(path).entryList({"timing-*.bin"},QDir::Files).isEmpty();
                const auto loopSummaryPath = QDir(path).filePath("looplog-summary.json");
                auto loopSummary = object(loopSummaryPath);
                const auto loopNamesUnsorted = QDir(path).entryList({"looplog-*.bin"},QDir::Files);
                const bool hasWindowFiles = std::any_of(loopNamesUnsorted.cbegin(), loopNamesUnsorted.cend(),
                    [](const QString &name) { return name.startsWith(QStringLiteral("looplog-window-")); });
                const bool loopWindowMode = loopSummary.value("mode") == QStringLiteral("rolling-burst-windows") ||
                    (loopSummary.value("mode").toString().isEmpty() && (hasCut(loopCuts) || hasWindowFiles));
                const bool loopEvidence=hasCut(loopCuts)||QFileInfo::exists(loopSummaryPath)||
                    !loopNamesUnsorted.isEmpty();
                const bool timingMissing=timingEnabled.isBool()&&timingEnabled.toBool()&&!timingEvidence;
                const bool loopMissing=loopEnabled.isBool()&&loopEnabled.toBool()&&!loopEvidence;
                const qint64 wall=metadata.value("wallAnchorMs").toVariant().toLongLong();
                if (wall > capturedAt) continue;
                const qint64 mono=metadata.value("monotonicAnchorNs").toString().toLongLong();
                bool anchorKnown=wall&&mono;
                if(!anchorKnown && start!=std::numeric_limits<qint64>::min()){
                    const auto binary=QDir(path).entryList({"trace-*.bin","timing-*.bin","looplog-*.bin"},QDir::Files);
                    const auto traceInfo=object(QDir(path).filePath("trace-summary.json"));
                    const auto timingInfo=object(QDir(path).filePath("timing-summary.json"));
                    const auto hasEvidence=[](const QJsonObject& info,const char* key){
                        return info.value(key).toBool()||info.value("recordsIssued").toDouble()>0;
                    };
                    const bool loopAnchorNeeded = loopWindowMode
                        ? loopSummary.value("loopLogIncomplete").toBool() || !loopSummary.value("windows").toArray().isEmpty()
                        : hasEvidence(loopSummary,"loopLogIncomplete");
                    if(!binary.isEmpty()||hasEvidence(traceInfo,"traceIncomplete")||
                       hasEvidence(timingInfo,"timingIncomplete")||loopAnchorNeeded||timingMissing||loopMissing){
                        skippedRuns.append(QJsonObject{{"runId",metadata.value("runId")},{"reason","missing_time_anchor"},
                            {"timingEnabled",timingEnabled},{"loopLogEnabled",loopEnabled},
                            {"timingMissing",timingMissing},{"loopLogMissing",loopMissing},
                            {"timingIncomplete",timingMissing||hasEvidence(timingInfo,"timingIncomplete")},
                            {"loopLogIncomplete",loopMissing||loopAnchorNeeded}});
                        bundleIncomplete=true;
                        continue;
                    }
                    if (!loopWindowMode) continue;
                }
                auto summary=object(QDir(path).filePath("trace-summary.json"));
                quint64 boundary=quint64(summary.value("recordsIssued").toDouble());bool flushed=!summary.isEmpty(),boundaryKnown=summary.contains("recordsIssued");
                for(const auto& cut:cuts)if(cutPath(cut.root)==path){boundary=cut.sequence;boundaryKnown=true;flushed=cut.flush();break;}
                bool traceBad=false;
                quint64 selected=0,filtered=0,maxSequence=0;int part=0;
                qint64 firstMs=std::numeric_limits<qint64>::max(),lastMs=std::numeric_limits<qint64>::min();
                QJsonArray runFiles;
                auto names=QDir(path).entryList({"trace-*.bin"},QDir::Files,QDir::Name);
                std::sort(names.begin(),names.end(),[](const QString& a,const QString& b){return a.mid(6).section('.',0,0).toInt()<b.mid(6).section('.',0,0).toInt();});
                for(const auto& name:names){
                    QByteArray bytes=read(QDir(path).filePath(name)),chosen;chosen.reserve(bytes.size());
                    if(bytes.size()%64)traceBad=true;
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
                    if(cutPath(cut.root)==path){
                        timingLive=true;timingBoundary=cut.sequence;timingFlushed=cut.flush();break;
                    }
                if(!timingLive)timingBoundary=quint64(timingSummary.value("recordsIssued").toDouble());
                const bool timingBoundaryKnown=timingLive||timingSummary.contains("recordsIssued");
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
                        if(timingBoundaryKnown&&r.sequence>timingBoundary)continue;
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
                for(const auto& cut:loopCuts)if(cutPath(cut.root)==path){
                    loopLive=true;loopBoundary=cut.sequence;loopDropped=cut.dropped;loopFreezeEpoch=cut.freezeEpoch;
                    loopIncomplete=cut.incomplete;loopBudgetExhausted=cut.budgetExhausted;loopWriteFailed=cut.writeFailed;
                    loopFlushed=cut.flush();break;}
                // An active rolling writer has no final inventory yet. Keep
                // coverage unknown instead of comparing its listener total.
                if (loopSummary.isEmpty() && loopLive) loopSummary = object(loopSummaryPath);
                if (loopSummary.isEmpty() && loopLive) loopSummary=QJsonObject{{"schemaVersion",2},
                    {"mode","rolling-burst-windows"},{"recordBytes",80},
                    {"recordsIssued",double(loopBoundary)},{"queueDropped",double(loopDropped)},
                    {"budgetExhausted",loopBudgetExhausted},{"writeFailed",loopWriteFailed},
                    {"burstMarkEpochs",double(loopFreezeEpoch)}};
                if(!loopLive)loopBoundary=quint64(loopSummary.value("recordsIssued").toDouble());
                const bool loopBoundaryKnown=loopLive||loopSummary.contains("recordsIssued");
                auto loopNames=QDir(path).entryList({"looplog-*.bin"},QDir::Files,QDir::Name);
                std::sort(loopNames.begin(),loopNames.end(),[](const QString& a,const QString& b){
                    return a.section('-',-1).section('.',0,0).toInt()<b.section('-',-1).section('.',0,0).toInt();
                });
                int loopPart=0; quint64 loopSelected=0,loopFiltered=0,loopAfterCut=0; bool loopBad=false;
                QHash<QString, LoopFileExport> loopFiles;
                for(const auto& name:loopNames){
                    auto &file = loopFiles[name];
                    const QFileInfo info(QDir(path).filePath(name));
                    QFile input(info.filePath());
                    if (info.isSymLink() || !input.open(QIODevice::ReadOnly)) { loopBad = true; continue; }
                    const auto bytes = input.readAll();
                    if (input.error() != QFileDevice::NoError) { loopBad = true; continue; }
                    file.bytes = bytes.size();
                    file.formatError = bytes.size() % sizeof(LoopRecord) != 0;
                    QByteArray chosen;
                    constexpr qsizetype recordBytes = sizeof(LoopRecord);
                    for(qsizetype at=0;at+recordBytes<=bytes.size();at+=recordBytes){LoopRecord r;std::memcpy(&r,bytes.constData()+at,recordBytes);
                        if (loopWindowMode && (!r.sequence || r.kind < uint16_t(LoopKind::Loop) ||
                                              r.kind > uint16_t(LoopKind::ControlMark))) {
                            file.formatError = true;
                            continue;
                        }
                        if(loopBoundaryKnown&&r.sequence>loopBoundary){++file.afterCut;continue;}
                        const qint64 time=anchorKnown?wall+(qint64(r.timeNs)-mono)/1000000:0;
                        if(anchorKnown&&(time<start||time>end)){++file.filtered;continue;}
                        chosen.append(bytes.constData()+at,recordBytes);++file.selected;}
                    loopSelected += file.selected; loopFiltered += file.filtered;
                    loopAfterCut += file.afterCut; loopBad |= file.formatError;
                    if(!chosen.isEmpty()){
                        file.exportedName = QString("paimage/%1/looplog-%2.bin").arg(dir).arg(loopPart++);
                        if(!write(file.exportedName,chosen))return false;
                        runFiles.append(file.exportedName);
                    }
                }
                const bool traceExpected=metadata.value("diagnosticTraceEnabled").toBool(true);
                if(!boundaryKnown)boundary=maxSequence;
                const bool traceCountCovered=!boundaryKnown||selected+filtered==boundary;
                const bool traceFailure=traceBad||(traceExpected&&(!flushed||!anchorKnown||
                    summary.value("traceIncomplete").toBool()||!traceCountCovered));
                const bool traceIncomplete=traceFailure||(traceExpected&&filtered>0);
                const bool timingCountCovered=!timingBoundaryKnown||timingSelected+timingFiltered==timingBoundary;
                const bool timingPresent=timingEvidence||timingMissing;
                const bool timingIncomplete=timingMissing||(timingPresent&&(timingSummary.value("timingIncomplete").toBool()||
                    !timingFlushed||timingBad||!anchorKnown||!timingCountCovered));
                auto coverage = loopWindowMode ? checkLoopWindows(loopSummary, loopFiles) : LoopWindowCoverage{};
                // Schema 1 used continuous files. Only that legacy contract
                // ties recordsIssued to the on-disk record count.
                const bool legacyCount = !loopWindowMode && loopSummary.value("mode").toString().isEmpty() &&
                    loopSummary.value("schemaVersion").toInt(1) == 1 && loopBoundaryKnown;
                if (legacyCount) coverage.covered = loopSelected + loopFiltered == loopBoundary;
                const bool loopPresent=loopEvidence||loopMissing;
                const bool loopStreamIncomplete=loopMissing||(loopPresent&&(loopSummary.value("loopLogIncomplete").toBool()||
                    loopIncomplete||loopBad||!loopFlushed||coverage.incomplete||
                    (coverage.covered.isBool()&&!coverage.covered.toBool())||
                    (!anchorKnown&&(loopSelected||loopFiltered))));
                const bool incomplete=traceIncomplete||timingIncomplete||loopStreamIncomplete;
                const bool noRecords = !selected && !timingSelected && !loopSelected;
                const bool skipped = noRecords && (traceFailure || timingIncomplete || loopStreamIncomplete);
                if(noRecords){
                    if(skipped){
                        skippedRuns.append(QJsonObject{{"runId",metadata.value("runId")},{"reason","no_complete_records"},
                            {"traceFormatError",traceBad},{"timingFormatError",timingBad},{"loopLogFormatError",loopBad},
                            {"traceIncomplete",traceFailure},{"timingIncomplete",timingIncomplete},{"loopLogIncomplete",loopStreamIncomplete},
                            {"timingEnabled",timingEnabled},{"loopLogEnabled",loopEnabled},{"timingMissing",timingMissing},{"loopLogMissing",loopMissing},
                            {"loopLogRetentionStatus",coverage.status}});
                        bundleIncomplete=true;
                    }
                    if (!loopWindowMode) continue;
                }
                if(!write(prefix+"run-config.json",QJsonDocument(metadata).toJson()))return false;
                if(traceExpected||traceBad){
                    auto exported=summary;
                    exported.insert("recordsIssued",double(boundary));
                    exported.insert("recordsWritten",double(selected));
                    exported.insert("traceIncomplete",traceIncomplete);
                    exported.insert("exportClipped",filtered>0);
                    exported.insert("exportFilteredRecords",double(filtered));
                    exported.insert("exportFlushCompleted",flushed);
                    exported.insert("exportFormatError",traceBad);
                    exported.insert("exportCountCovered",boundaryKnown?QJsonValue(traceCountCovered):QJsonValue());
                    if(!write(prefix+"trace-summary.json",QJsonDocument(exported).toJson()))return false;
                }
                if(timingPresent){
                    if(timingLive)timingSummary.insert("recordsIssued",double(timingBoundary));
                    timingSummary.insert("exportFlushCompleted",timingFlushed);
                    timingSummary.insert("recordsWritten",double(timingSelected));
                    timingSummary.insert("exportFilteredRecords",double(timingFiltered));
                    timingSummary.insert("exportFormatError",timingBad);
                    timingSummary.insert("exportCountCovered",timingBoundaryKnown?QJsonValue(timingCountCovered):QJsonValue());
                    timingSummary.insert("timingIncomplete",timingIncomplete);
                    timingSummary.insert("exportMissing",timingMissing);
                    if(!write(prefix+"timing-summary.json",QJsonDocument(timingSummary).toJson()))return false;
                }
                if(loopPresent){loopSummary.insert("exportFlushCompleted",loopFlushed);
                    if (loopSummary.contains("recordsWritten"))
                        loopSummary.insert("sourceRecordsWritten", loopSummary.value("recordsWritten"));
                    loopSummary.insert("recordsWritten",double(loopSelected));loopSummary.insert("exportFilteredRecords",double(loopFiltered));
                    loopSummary.insert("exportAfterCutRecords", qint64(loopAfterCut));
                    loopSummary.insert("exportClipped", loopFiltered > 0 || loopAfterCut > 0);
                    loopSummary.insert("exportFormatError",loopBad);
                    loopSummary.insert("exportCountCovered", coverage.covered);
                    if (loopWindowMode) {
                        loopSummary.insert("exportRetentionStatus", coverage.status);
                        loopSummary.insert("exportWindows", coverage.windows);
                    }
                    loopSummary.insert("loopLogIncomplete",loopStreamIncomplete);loopSummary.insert("exportMissing",loopMissing);
                    if(!write(prefix+"looplog-summary.json",QJsonDocument(loopSummary).toJson()))return false;}
                bundleIncomplete|=incomplete;
                if (skipped) continue;
                runs.append(QJsonObject{{"runId",metadata.value("runId")},{"recordBoundary",double(boundary)},
                    {"recordsIncluded",double(selected)},{"recordsOutsideWindow",double(filtered)},
                    {"traceIncomplete",incomplete},{"traceEnabled",traceExpected},{"timingEnabled",timingEnabled},{"loopLogEnabled",loopEnabled},
                    {"timingIncomplete",timingIncomplete},{"loopLogIncomplete",loopStreamIncomplete},{"timingMissing",timingMissing},{"loopLogMissing",loopMissing},{"timingRecordsIncluded",double(timingSelected)},{"anchorKnown",anchorKnown},{"files",runFiles},
                    {"loopLogRetentionStatus",coverage.status},{"loopLogRecordsIncluded",qint64(loopSelected)},
                    {"loopLogClipped",loopFiltered>0||loopAfterCut>0},
                    {"firstIncludedMs",anchorKnown&&selected?QJsonValue(double(firstMs)):QJsonValue()},
                    {"lastIncludedMs",anchorKnown&&selected?QJsonValue(double(lastMs)):QJsonValue()}});
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
