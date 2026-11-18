#include "ZipTestPayload.h"
#include <QTimer>
#include "DiagnosticRecorder.h"
#include "DiagnosticStorage.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTextStream>

#include <array>
#include <chrono>
#include <future>

namespace {

bool check(bool condition, const QString &message)
{
    if (!condition) qCritical().noquote() << message;
    return condition;
}

quint16 readU16(const QByteArray &bytes, int offset)
{
    return static_cast<quint16>(static_cast<unsigned char>(bytes.at(offset)))
        | (static_cast<quint16>(static_cast<unsigned char>(bytes.at(offset + 1))) << 8);
}

quint32 readU32(const QByteArray &bytes, int offset)
{
    return static_cast<quint32>(readU16(bytes, offset))
        | (static_cast<quint32>(readU16(bytes, offset + 2)) << 16);
}

quint32 crc32(const QByteArray &data)
{
    static const std::array<quint32, 256> table = [] {
        std::array<quint32, 256> values{};
        for (quint32 i = 0; i < values.size(); ++i) {
            quint32 value = i;
            for (int bit = 0; bit < 8; ++bit)
                value = (value & 1U) ? (0xedb88320U ^ (value >> 1)) : (value >> 1);
            values[i] = value;
        }
        return values;
    }();
    quint32 crc = 0xffffffffU;
    for (char byte : data)
        crc = table[(crc ^ static_cast<unsigned char>(byte)) & 0xffU] ^ (crc >> 8);
    return crc ^ 0xffffffffU;
}

QHash<QString, QByteArray> readZip(const QString &path, QString *error)
{
    QHash<QString, QByteArray> result;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = file.errorString();
        return result;
    }
    const QByteArray bytes = file.readAll();
    int offset = 0;
    while (offset + 4 <= bytes.size() && readU32(bytes, offset) == 0x04034b50U) {
        if (offset + 30 > bytes.size()) {
            if (error) *error = QStringLiteral("truncated local ZIP header");
            return {};
        }
        const quint32 expectedCrc = readU32(bytes, offset + 14);
        const quint32 compressedSize = readU32(bytes, offset + 18);
        const quint32 uncompressedSize = readU32(bytes, offset + 22);
        const quint16 nameSize = readU16(bytes, offset + 26);
        const quint16 extraSize = readU16(bytes, offset + 28);
        const int dataOffset = offset + 30 + nameSize + extraSize;
        if (dataOffset < 0 || dataOffset + static_cast<qint64>(compressedSize) > bytes.size()) {
            if (error) *error = QStringLiteral("truncated ZIP entry");
            return {};
        }
        const QString name = QString::fromUtf8(bytes.constData() + offset + 30, nameSize);
        QByteArray data;
        if (!decodeZipPayload(bytes.mid(dataOffset, static_cast<int>(compressedSize)),
                              readU16(bytes, offset + 8), uncompressedSize, data) || crc32(data) != expectedCrc) {
            if (error) *error = QStringLiteral("ZIP CRC, method or size mismatch for %1").arg(name);
            return {};
        }
        result.insert(name, data);
        offset = dataOffset + static_cast<int>(compressedSize);
    }
    if (result.isEmpty() || offset + 4 > bytes.size() || readU32(bytes, offset) != 0x02014b50U) {
        if (error) *error = QStringLiteral("ZIP central directory missing");
        return {};
    }
    return result;
}

QString iso(const QString &text)
{
    return QDateTime::fromString(text, QStringLiteral("yyyy-MM-dd HH:mm:ss"))
        .toString(Qt::ISODateWithMs);
}

void appendLine(QByteArray *file, const QJsonObject &object)
{
    *file += QJsonDocument(object).toJson(QJsonDocument::Compact);
    *file += '\n';
}

void writeBytes(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    file.write(bytes);
}

void createRun(const QString &root,
               const QString &runId,
               const QString &first,
               const QString &last,
               bool active,
               const QVector<QPair<quint64, QString>> &events,
               const QJsonObject &integrity = {})
{
    const QString directory = QDir(root).filePath(QStringLiteral("run-") + runId);
    QDir().mkpath(directory);
    QByteArray eventBytes;
    QByteArray runtimeBytes;
    QByteArray networkBytes;
    QByteArray settingsBytes;
    QByteArray cardBytes;
    for (const auto &entry : events) {
        const quint64 sequence = entry.first;
        const QString timestamp = iso(entry.second);
        QJsonObject event{{QStringLiteral("sequence"), static_cast<double>(sequence)},
                          {QStringLiteral("runId"), runId},
                          {QStringLiteral("timestamp"), timestamp},
                          {QStringLiteral("category"), QStringLiteral("test")},
                          {QStringLiteral("message"), QStringLiteral("event-%1").arg(sequence)}};
        appendLine(&eventBytes, event);
        runtimeBytes += QStringLiteral("[seq=%1] [%2] [info] [test] event-%1\n")
                            .arg(sequence).arg(timestamp).toUtf8();

        const QJsonObject snapshot{{QStringLiteral("sequence"), static_cast<double>(sequence)},
                                   {QStringLiteral("runId"), runId},
                                   {QStringLiteral("timestamp"), timestamp},
                                   {QStringLiteral("data"), QJsonObject{{QStringLiteral("value"), static_cast<int>(sequence)}}}};
        appendLine(&networkBytes, snapshot);
        appendLine(&settingsBytes, snapshot);
        appendLine(&cardBytes, QJsonObject{{QStringLiteral("sequence"), static_cast<double>(sequence)},
                                           {QStringLiteral("runId"), runId},
                                           {QStringLiteral("timestamp"), timestamp},
                                           {QStringLiteral("card"), 1},
                                           {QStringLiteral("ip"), QStringLiteral("192.168.0.2")},
                                           {QStringLiteral("state"), QStringLiteral("ok")},
                                           {QStringLiteral("fields"), QJsonObject{{QStringLiteral("value"), static_cast<int>(sequence)}}}});
    }
    writeBytes(QDir(directory).filePath(QStringLiteral("events.jsonl")), eventBytes);
    writeBytes(QDir(directory).filePath(QStringLiteral("runtime.log")), runtimeBytes);
    writeBytes(QDir(directory).filePath(QStringLiteral("network_history.jsonl")), networkBytes);
    writeBytes(QDir(directory).filePath(QStringLiteral("settings_history.jsonl")), settingsBytes);
    writeBytes(QDir(directory).filePath(QStringLiteral("card_history.jsonl")), cardBytes);
    writeBytes(QDir(directory).filePath(QStringLiteral("network.json")), QByteArray("{}\n"));
    writeBytes(QDir(directory).filePath(QStringLiteral("settings.json")), QByteArray("{}\n"));
    writeBytes(QDir(directory).filePath(QStringLiteral("summary.txt")), QByteArray("synthetic\n"));
    QJsonObject manifest{{QStringLiteral("formatVersion"), 1},
                               {QStringLiteral("runId"), runId},
                               {QStringLiteral("startTime"), iso(first)},
                               {QStringLiteral("endTime"), active ? QString() : iso(last)},
                               {QStringLiteral("boundarySequence"), static_cast<double>(events.isEmpty() ? 0 : events.constLast().first)},
                               {QStringLiteral("active"), active},
                               {QStringLiteral("complete"), !active}};
    for (auto it = integrity.begin(); it != integrity.end(); ++it)
        manifest.insert(it.key(), it.value());
    writeBytes(QDir(directory).filePath(QStringLiteral("manifest.json")),
               QJsonDocument(manifest).toJson(QJsonDocument::Compact) + '\n');
}

DiagnosticRecorder::ExportRequest sourceFromRun(const DiagnosticRecorder::RunInfo &run,
                                                quint64 boundary,
                                                bool active,
                                                const std::function<bool()> &flush = {})
{
    DiagnosticRecorder::ExportRequest source;
    source.runId = run.runId;
    source.sourceDirectory = run.directory;
    source.startIsoTime = run.startIsoTime;
    source.endIsoTime = active ? QString() : run.endIsoTime;
    source.boundarySequence = boundary;
    source.sourceWasActive = active;
    source.flushBeforeExport = flush;
    return source;
}

bool testTimeWindowAcrossRuns()
{
    QTemporaryDir temp;
    if (!check(temp.isValid(), QStringLiteral("temporary directory unavailable"))) return false;
    createRun(temp.path(), QStringLiteral("A"), QStringLiteral("2026-09-08 09:00:00"),
              QStringLiteral("2026-09-08 09:01:00"), false,
              {{1, QStringLiteral("2026-09-08 09:00:00")}});
    createRun(temp.path(), QStringLiteral("B"), QStringLiteral("2026-09-08 10:03:00"),
              QStringLiteral("2026-09-08 10:09:00"), false,
              {{1, QStringLiteral("2026-09-08 10:03:00")},
               {2, QStringLiteral("2026-09-08 10:05:00")},
               {3, QStringLiteral("2026-09-08 10:07:00")},
               {4, QStringLiteral("2026-09-08 10:09:00")} });
    createRun(temp.path(), QStringLiteral("C"), QStringLiteral("2026-09-08 10:06:00"),
              QString(), true,
              {{1, QStringLiteral("2026-09-08 10:06:00")},
               {99, QStringLiteral("2026-09-08 10:07:00")},
               {100, QStringLiteral("2026-09-08 10:10:00")} });

    QLockFile activeLock(QDir(temp.path()).filePath(QStringLiteral("run-C/.active.lock")));
    if (!check(activeLock.tryLock(0), QStringLiteral("could not mark active run"))) return false;
    const QVector<DiagnosticRecorder::RunInfo> runs = DiagnosticRecorder::enumerateRuns(temp.path());
    if (!check(runs.size() == 3, QStringLiteral("retained run enumeration mismatch"))) return false;

    const QDateTime start = QDateTime::fromString(QStringLiteral("2026-09-08T10:04:00.000"), Qt::ISODateWithMs);
    const QDateTime end = QDateTime::fromString(QStringLiteral("2026-09-08T10:08:00.000"), Qt::ISODateWithMs);
    DiagnosticRecorder::TimeWindowRequest request;
    request.requestedStartTime = start.toString(Qt::ISODateWithMs);
    request.requestedEndTime = end.toString(Qt::ISODateWithMs);
    request.exportCapturedAt = QStringLiteral("2026-09-08T10:08:01.000");
    request.rootDirectory = temp.path();
    request.note = QStringLiteral("window regression");
    bool flushed = false;
    for (const auto &run : runs) {
        if (run.runId == QStringLiteral("A")) continue;
        if (run.runId == QStringLiteral("B"))
            request.sources.append(sourceFromRun(run, 4, false));
        else if (run.runId == QStringLiteral("C"))
            request.sources.append(sourceFromRun(run, 1, true, [&flushed] { flushed = true; return true; }));
    }
    const QString zipPath = QDir(temp.path()).filePath(QStringLiteral("window.zip"));
    const auto result = DiagnosticRecorder::exportTimeWindow(request, zipPath);
    if (!check(result.success, QStringLiteral("time-window export failed: %1").arg(result.error))) return false;
    if (!check(flushed, QStringLiteral("active source was not flushed before export"))) return false;
    if (!check(result.sourceRunCount == 2 && !result.noRecords, QStringLiteral("source run selection/no-record flag mismatch"))) return false;
    if (!check(result.formalWindowRecordCounts.value(QStringLiteral("events")).toInt() == 3,
               QStringLiteral("formal event count must exclude active post-boundary event"))) return false;
    if (!check(result.formalWindowRecordCounts.value(QStringLiteral("network")).toInt() == 3,
               QStringLiteral("formal network count mismatch"))) return false;
    if (!check(result.boundaryContextCounts.value(QStringLiteral("network")).toInt() >= 2,
               QStringLiteral("before/after boundary context missing"))) return false;

    QString error;
    const auto files = readZip(zipPath, &error);
    if (!check(error.isEmpty(), error)) return false;
    const QJsonObject manifest = QJsonDocument::fromJson(files.value(QStringLiteral("manifest.json"))).object();
    const QJsonArray ids = manifest.value(QStringLiteral("sourceRunIds")).toArray();
    if (!check(ids.size() == 2 && !QString::fromUtf8(QJsonDocument(ids).toJson()).contains('A'),
               QStringLiteral("non-overlapping run leaked into manifest"))) return false;
    if (!check(files.value(QStringLiteral("events.jsonl")).contains("event-99") == false,
               QStringLiteral("active records after immutable boundary leaked"))) return false;
    const QString summary = QString::fromUtf8(files.value(QStringLiteral("summary.txt")));
    if (!check(summary.startsWith(QStringLiteral("MC410T 时间窗诊断导出"))
                   && summary.contains(QStringLiteral("正式时间窗"))
                   && summary.contains(QStringLiteral("窗口外边界上下文")),
               QStringLiteral("time-window summary missing explicit sections"))) return false;

    request.requestedStartTime = QStringLiteral("2026-09-08T11:00:00.000");
    request.requestedEndTime = QStringLiteral("2026-09-08T11:01:00.000");
    const QString emptyZip = QDir(temp.path()).filePath(QStringLiteral("empty-window.zip"));
    const auto emptyResult = DiagnosticRecorder::exportTimeWindow(request, emptyZip);
    if (!check(emptyResult.success && emptyResult.noRecords, QStringLiteral("empty window not reported explicitly"))) return false;
    const auto emptyFiles = readZip(emptyZip, &error);
    if (!check(error.isEmpty() && QString::fromUtf8(emptyFiles.value(QStringLiteral("summary.txt"))).contains(QStringLiteral("该时间段没有日志")),
               QStringLiteral("empty window summary text missing"))) return false;
    return true;
}

bool testHistoricalRunIntegrity()
{
    QTemporaryDir temp;
    if (!temp.isValid()) return false;
    const auto now = QDateTime::currentDateTime();
    const auto first = now.addSecs(-90).toString("yyyy-MM-dd HH:mm:ss");
    const auto last = now.addSecs(-60).toString("yyyy-MM-dd HH:mm:ss");
    createRun(temp.path(), "failed", first, last, false, {{101, first}},
              {{"dropCounters", QJsonObject{{"queueDropped", 7}, {"writeFallbackDropped", 2}}},
               {"complete", false}, {"writeError", true}, {"writeErrorText", "disk fault"},
               {"truncationReasons", QJsonArray{"historical records truncated"}}});
    createRun(temp.path(), "healthy", first, last, false, {{202, first}});
    DiagnosticRecorder::Options options;
    options.rootDirectory = temp.path();
    DiagnosticRecorder fresh(options);
    fresh.recordEvent("history-integrity", "fresh recorder outside the historical window");
    if (!check(fresh.flush(3000), QStringLiteral("new recorder setup failed: %1").arg(fresh.status().writeErrorText))) return false;
    auto request = fresh.captureTimeWindowRequest(now.addSecs(-100), now.addSecs(-45));
    for (const auto &source : std::as_const(request.sources))
        if (source.runId == "failed") {
            const auto duplicate = source;
            request.sources.append(duplicate);
            break;
        }
    const auto zipPath = temp.filePath("historical-integrity.zip");
    const auto result = DiagnosticRecorder::exportTimeWindow(request, zipPath);
    if (!check(result.success, result.error)) return false;
    QString error;
    const auto zip = readZip(zipPath, &error);
    const auto manifest = QJsonDocument::fromJson(zip.value("manifest.json")).object();
    const auto runs = manifest.value("sourceRunIntegrity").toArray();
    QJsonObject failed, healthy;
    for (const auto &entry : runs) {
        const auto info = entry.toObject();
        if (info.value("runId") == "failed") failed = info;
        if (info.value("runId") == "healthy") healthy = info;
    }
    const auto aggregate = manifest.value("recorderDropCounters").toObject();
    return check(error.isEmpty() && result.sourceRunCount == 2 && runs.size() == 2 &&
        manifest.value("recorderWriteError").toBool() &&
        aggregate.value("queueDropped").toInt() == 7 && aggregate.value("writeFallbackDropped").toInt() == 2 &&
        !manifest.value("recorderDropCountersScope").toString().isEmpty() &&
        failed.value("writeErrorText").toString() == "disk fault" &&
        failed.value("truncationReasons").toArray().contains(QJsonValue("historical records truncated")) &&
        healthy.value("writeError").isBool() && !healthy.value("writeError").toBool() &&
        healthy.value("dropCounters").toObject().value("queueDropped").toInt(-1) == 0 &&
        zip.value("events.jsonl").count("event-101") == 1 &&
        zip.value("summary.txt").contains("historical records truncated"),
        "historical integrity must follow the failing run and aggregate once");
}

bool testActiveImmutableBoundary()
{
    QTemporaryDir temp;
    if (!check(temp.isValid(), QStringLiteral("temporary directory unavailable"))) return false;
    DiagnosticRecorder::Options options;
    options.rootDirectory = temp.path();
    DiagnosticRecorder recorder(options);
    recorder.logText(QStringLiteral("before-boundary"));
    if (!check(recorder.flush(2000), QStringLiteral("pre-boundary recorder flush failed"))) return false;

    const QDateTime captureTime = QDateTime::currentDateTime();
    DiagnosticRecorder::TimeWindowRequest request = recorder.captureTimeWindowRequest(
        captureTime.addSecs(-10), captureTime.addSecs(30), QStringLiteral("active-boundary"));
    if (!check(!request.sources.isEmpty() && request.sources.first().capturedLive,
               QStringLiteral("active run was not captured as a live source"))) return false;
    recorder.logText(QStringLiteral("after-boundary"));

    const QString zipPath = QDir(temp.path()).filePath(QStringLiteral("active-boundary.zip"));
    const auto result = DiagnosticRecorder::exportTimeWindow(request, zipPath);
    if (!check(result.success, QStringLiteral("active immutable export failed: %1").arg(result.error))) return false;
    QString error;
    const auto files = readZip(zipPath, &error);
    if (!check(error.isEmpty(), error)) return false;
    const QByteArray events = files.value(QStringLiteral("events.jsonl"));
    return check(events.contains("before-boundary") && !events.contains("after-boundary"),
                 QStringLiteral("post-capture active event crossed immutable export boundary"));
}

bool testMixedWindowReleasesRetentionPin()
{
    QTemporaryDir temp;
    if (!check(temp.isValid(), "temporary directory unavailable")) return false;
    DiagnosticRecorder::Options options;
    options.rootDirectory = temp.path();
    const QDateTime start = QDateTime::currentDateTime().addSecs(-10);
    {
        DiagnosticRecorder historical(options);
        historical.recordEvent("window-pin", "ended-run-event");
        if (!check(historical.flush(3000), "historical flush failed")) return false;
    }
    DiagnosticRecorder active(options);
    active.recordEvent("window-pin", "active-run-event");
    if (!check(active.flush(3000), "active flush failed")) return false;
    const QDateTime end = QDateTime::currentDateTime().addSecs(10);
    {
        auto abandoned = active.captureTimeWindowRequest(start, end);
        if (!check(abandoned.validationError.isEmpty() && abandoned.sources.size() == 2,
                   "abandoned mixed window was not captured")) return false;
    }
    {
        const auto lease = diagnostic_storage::lease(temp.path());
        if (!check(bool(lease), "abandoned window retained the root pin")) return false;
    }
    auto request = active.captureTimeWindowRequest(start, end);
    if (!check(request.validationError.isEmpty() && request.sources.size() == 2,
               "mixed window was not captured")) return false;
    std::promise<void> enteredPromise, releasePromise;
    auto entered = enteredPromise.get_future();
    const auto release = releasePromise.get_future().share();
    request.bundleExtension = [&](const DiagnosticRecorder::BundleSink &, QString *) {
        enteredPromise.set_value(); release.wait(); return true;
    };
    const QString zipPath = temp.filePath("mixed-window.zip");
    auto worker = std::async(std::launch::async, [&] {
        return DiagnosticRecorder::exportTimeWindow(request, zipPath);
    });
    const bool blocked = entered.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    bool rootAvailable = false;
    if (blocked) rootAvailable = bool(diagnostic_storage::lease(temp.path()));
    releasePromise.set_value();
    const auto result = worker.get();
    QString error;
    const auto files = readZip(zipPath, &error);
    const auto events = files.value("events.jsonl");
    return check(blocked && rootAvailable, "mixed export retained root pin during ZIP extension")
        && check(result.success && error.isEmpty() && result.sourceRunCount == 2
                     && events.count("ended-run-event") == 1 && events.count("active-run-event") == 1,
                 "mixed window did not export both runs exactly once");
}

bool testPendingWindowProtectsEndedRuns()
{
    QTemporaryDir temp;
    if (!check(temp.isValid(), "temporary directory unavailable")) return false;
    DiagnosticRecorder::Options options;
    options.rootDirectory = temp.path();
    options.maxRetainedRuns = 1;
    DiagnosticRecorder recorder(options);
    auto cleaner = std::make_unique<DiagnosticRecorder>(options);
    if (!check(recorder.flush(3000) && cleaner->flush(3000), "recorder setup failed")) return false;

    const auto now = QDateTime::currentDateTime();
    const auto start = now.addSecs(-20);
    const auto end = now.addSecs(20);
    const QString first = now.addSecs(-10).toString("yyyy-MM-dd HH:mm:ss");
    const QString last = now.addSecs(-5).toString("yyyy-MM-dd HH:mm:ss");
    createRun(temp.path(), "protected-A", first, last, false, {{101, first}});
    createRun(temp.path(), "protected-B", first, last, false, {{202, first}});
    const QString runA = temp.filePath("run-protected-A/events.jsonl");
    const QString runB = temp.filePath("run-protected-B/events.jsonl");
    if (!check(QFile::exists(runA) && QFile::exists(runB), "ended run fixtures missing")) return false;

    auto pending = recorder.registerTimeWindowRequest(start, end);
    auto request = pending;
    if (!check(request.validationError.isEmpty() && request.discoverSources && !request.retentionPin,
               "registration should defer historical discovery and filesystem locking")) return false;

    std::promise<void> releaseExport, zipEntered, finishZip;
    const auto released = releaseExport.get_future();
    auto zipStarted = zipEntered.get_future();
    const auto zipRelease = finishZip.get_future().share();
    request.bundleExtension = [&](const DiagnosticRecorder::BundleSink &, QString *) {
        zipEntered.set_value();
        zipRelease.wait();
        return true;
    };
    const QString target = temp.filePath("retained-window.zip");
    auto worker = std::async(std::launch::async, [&] {
        released.wait();
        return DiagnosticRecorder::exportTimeWindow(request, target);
    });
    bool eventLoopAdvanced = false;
    {
        const auto rootLease = diagnostic_storage::lease(temp.path());
        QTimer::singleShot(0, [&] { eventLoopAdvanced = true; });
        QCoreApplication::processEvents();
        eventLoopAdvanced = eventLoopAdvanced && bool(rootLease);
    }
    cleaner.reset(); // Drops the active lease and runs the production quota cleaner synchronously.
    const bool survived = QFile::exists(runA) && QFile::exists(runB);
    releaseExport.set_value();
    const bool zipPaused = zipStarted.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    bool prunedDuringZip = false, wroteDuringZip = false, duplicateRejected = false;
    if (zipPaused) {
        const auto duplicate = DiagnosticRecorder::exportTimeWindow(pending, temp.filePath("duplicate.zip"));
        duplicateRejected = !duplicate.success && !QFileInfo::exists(temp.filePath("duplicate.zip"));
        DiagnosticRecorder cleanup(options);
        cleanup.recordEvent("retention-handoff", "writer resumed during ZIP");
        wroteDuringZip = cleanup.flush(3000) && !cleanup.status().writeError;
        prunedDuringZip = !QFile::exists(runA) || !QFile::exists(runB);
    }
    finishZip.set_value();
    const auto result = worker.get();

    QString error;
    const auto files = readZip(target, &error);
    const auto events = files.value("events.jsonl");
    const auto ids = QJsonDocument::fromJson(files.value("manifest.json")).object()
                         .value("sourceRunIds").toArray();
    return check(eventLoopAdvanced, "pending registration took a root file lock or blocked Qt events")
        && check(survived, "quota cleaner deleted a historical source before background discovery")
        && check(result.success && error.isEmpty()
                     && ids.contains(QJsonValue(QStringLiteral("protected-A")))
                     && ids.contains(QJsonValue(QStringLiteral("protected-B")))
                     && events.count("event-101") == 1 && events.count("event-202") == 1,
                 QStringLiteral("historical source missing from ZIP: %1 %2").arg(result.error, error))
        && check(zipPaused && prunedDuringZip && wroteDuringZip && duplicateRejected,
                 "slow ZIP must allow quota cleanup and writing while copied requests still exist");
}

bool testWindowIntentCancellationAndPruneConflict()
{
    QTemporaryDir temp;
    if (!check(temp.isValid(), "temporary directory unavailable")) return false;
    DiagnosticRecorder::Options options;
    options.rootDirectory = temp.path();
    DiagnosticRecorder recorder(options);
    if (!check(recorder.flush(3000), "recorder setup failed")) return false;
    const auto now = QDateTime::currentDateTime();
    const auto start = now.addSecs(-10);
    auto pending = recorder.registerTimeWindowRequest(start, now);
    {
        const auto abandoned = pending;
        pending = {};
        if (!check(abandoned.validationError.isEmpty() && !diagnostic_storage::tryPrune(temp.path()),
                   "an outstanding request copy must block pruning")) return false;
    }
    std::promise<bool> acquiredPermit;
    auto acquired = acquiredPermit.get_future();
    std::promise<void> releasePermit;
    const auto released = releasePermit.get_future();
    auto pruner = std::async(std::launch::async, [&] {
        const auto permit = diagnostic_storage::tryPrune(temp.path());
        acquiredPermit.set_value(bool(permit));
        released.wait();
    });
    const bool permitAcquired = acquired.get();
    const auto rejected = recorder.registerTimeWindowRequest(start, now);
    releasePermit.set_value();
    pruner.get();
    if (!check(permitAcquired, "abandoned request did not release the export intent")) return false;
    const QString target = temp.filePath("conflicted-window.zip");
    const auto result = DiagnosticRecorder::exportTimeWindow(rejected, target);
    return check(!rejected.validationError.isEmpty() && !result.success
                     && result.error == rejected.validationError && !QFileInfo::exists(target),
                 "a request registered during deletion must fail without publishing a ZIP");
}

bool testManualActiveSnapshotDoesNotBlockRotation()
{
    QTemporaryDir temp;
    DiagnosticRecorder::Options options;
    options.rootDirectory = temp.path();
    options.maxHistorySegmentBytes = 6144;
    options.maxFallbackEntries = 256;
    options.noiseBurst = 1000;
    DiagnosticRecorder recorder(options);
    const QString padding(512, QLatin1Char('x'));
    auto batch = [&](const QString &prefix, int count) {
        for (int i = 0; i < count; ++i)
            recorder.recordEvent("manual-snapshot", prefix + QString::number(i) + ":" + padding,
                                 DiagnosticRecorder::Severity::Warning);
        return recorder.flush(5000);
    };
    if (!check(batch("before-", 12), "initial rotation failed")) return false;
    const QDir run(recorder.runDirectory());
    QByteArray retained;
    for (const QString &name : {QStringLiteral("events.jsonl.previous"), QStringLiteral("events.jsonl")}) {
        QFile file(run.filePath(name));
        if (!check(file.open(QIODevice::ReadOnly), "initial segment missing")) return false;
        retained += file.readAll();
    }
    const auto runs = DiagnosticRecorder::enumerateRuns(temp.path());
    if (!check(runs.size() == 1, "active run missing")) return false;
    const auto now = QDateTime::currentDateTime();
    auto request = recorder.registerTimeWindowRequest(now.addSecs(-60), now.addSecs(60));
    if (!check(request.pendingHistory && request.sources.size() == 1 && !request.sources.first().historySnapshot,
               "click handler did not return a lightweight request")) return false;
    // Evict all of the original named segments before starting the export.
    if (!check(batch("between-", 36), "rotation before export failed")) return false;
    std::promise<void> enteredPromise, releasePromise;
    auto entered = enteredPromise.get_future();
    auto release = releasePromise.get_future().share();
    DiagnosticRecorder::setSnapshotReadHookForTest([&] { enteredPromise.set_value(); release.wait(); });
    const QString target = temp.filePath("manual-window.zip");
    auto worker = std::async(std::launch::async, [&] { return DiagnosticRecorder::exportTimeWindow(request, target); });
    const bool paused = entered.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    bool eventLoopAdvanced = false;
    QTimer::singleShot(0, [&] { eventLoopAdvanced = true; });
    QCoreApplication::processEvents();
    const bool flushed = paused && batch("after-", 36);
    const auto status = recorder.status();
    QByteArray tail;
    for (const QString &name : {QStringLiteral("events.jsonl.previous"), QStringLiteral("events.jsonl")}) {
        QFile file(run.filePath(name));
        if (file.open(QIODevice::ReadOnly)) tail += file.readAll();
    }
    releasePromise.set_value();
    const auto result = worker.get();
    DiagnosticRecorder::setSnapshotReadHookForTest({});
    QString error;
    const QByteArray exported = readZip(target, &error).value("events.jsonl");
    bool ok = check(paused && eventLoopAdvanced && flushed, QStringLiteral("registered export blocked the event loop or writer: paused=%1 writeError=%2 exportError=%3")
                        .arg(paused).arg(status.writeErrorText, result.error))
        && check(!status.writeError && status.drops.queueDropped == 0 && status.drops.criticalQueueDropped == 0,
                 QStringLiteral("manual snapshot caused writer loss: %1").arg(status.writeErrorText))
        && check(tail.contains("after-35:") && !tail.contains("after-0:"), "multiple rotations or formal tail missing")
        && check(result.success && error.isEmpty() && !exported.contains("after-"), "manual export boundary failed");
    for (int i = 0; i < 12; ++i) {
        const QByteArray marker = (QString("before-%1:").arg(i)).toUtf8();
        ok = check(exported.count(marker) == retained.count(marker), "pinned segment content changed") && ok;
    }
    return ok;
}

#ifdef Q_OS_WIN
bool testCrossProcessIntent()
{
    QTemporaryDir temp;
    if (!check(temp.isValid(), "temporary directory unavailable")) return false;
    auto probe = [&] {
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(), {"--probe-prune", temp.path()});
        if (!child.waitForStarted(5000) || !child.waitForFinished(5000)
            || child.exitStatus() != QProcess::NormalExit) return -1;
        return child.exitCode();
    };
    auto intent = diagnostic_storage::registerExportIntent(temp.path());
    if (!check(bool(intent), "could not register cross-process test intent")) return false;
    const int protectedResult = probe();
    intent.reset();
    return check(protectedResult == 1, "another process acquired a prune permit during export intent")
        && check(probe() == 0, "another process could not prune after intent release");
}
#endif

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
#ifdef Q_OS_WIN
    const auto args = app.arguments();
    if (args.size() == 3 && args.at(1) == "--probe-prune")
        return diagnostic_storage::tryPrune(args.at(2)) ? 0 : 1;
#endif
    bool ok = testTimeWindowAcrossRuns();
    ok = testHistoricalRunIntegrity() && ok;
#ifdef Q_OS_WIN
    ok = testCrossProcessIntent() && ok;
#endif
    ok = testActiveImmutableBoundary() && ok;
    ok = testMixedWindowReleasesRetentionPin() && ok;
    ok = testPendingWindowProtectsEndedRuns() && ok;
    ok = testWindowIntentCancellationAndPruneConflict() && ok;
    ok = testManualActiveSnapshotDoesNotBlockRotation() && ok;
    QTextStream(stdout) << (ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"))
                        << QStringLiteral(" diagnostic time-window export and retention")
                        << Qt::endl;
    return ok ? 0 : 1;
}
