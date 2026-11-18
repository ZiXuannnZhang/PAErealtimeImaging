#include "ZipTestPayload.h"
#include "DiagnosticRecorder.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QTemporaryDir>

#include <array>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

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
    for (char byte : data) crc = table[(crc ^ static_cast<unsigned char>(byte)) & 0xffU] ^ (crc >> 8);
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

DiagnosticRecorder::Options optionsFor(const QString &root)
{
    DiagnosticRecorder::Options options;
    options.rootDirectory = root;
    options.noiseBurst = 0;
    options.maxRetainedBytes = 32LL * 1024LL * 1024LL;
    return options;
}

bool testZipBoundaryAndSummary()
{
    QTemporaryDir temp;
    if (!check(temp.isValid(), QStringLiteral("temporary directory unavailable"))) return false;
    DiagnosticRecorder recorder(optionsFor(temp.path()));
    recorder.recordEvent(QStringLiteral("test"), QStringLiteral("first"));
    recorder.recordEvent(QStringLiteral("test"), QStringLiteral("multi\nline"), DiagnosticRecorder::Severity::Warning);
    recorder.recordNetworkSnapshot(QJsonObject{{QStringLiteral("listenId"), QStringLiteral("L1")} });
    recorder.recordSettingsSnapshot(QJsonObject{{QStringLiteral("configId"), QStringLiteral("C1")} });
    recorder.setCardSnapshot(5, QStringLiteral("192.168.0.5"), QStringLiteral("失败"),
                             QJsonObject{{QStringLiteral("listenId"), QStringLiteral("L1")},
                                         {QStringLiteral("configId"), QStringLiteral("C1")},
                                         {QStringLiteral("success"), false}});
    recorder.setCardSnapshot(5, QStringLiteral("192.168.0.5"), QStringLiteral("成功"),
                             QJsonObject{{QStringLiteral("listenId"), QStringLiteral("L1")},
                                         {QStringLiteral("configId"), QStringLiteral("C1")}});
    const quint64 boundary = recorder.captureBoundary();
    recorder.recordEvent(QStringLiteral("after"), QStringLiteral("must be excluded"));
    const QString zipPath = QDir(temp.path()).filePath(QStringLiteral("diagnostics.zip"));
    const DiagnosticRecorder::ExportResult result = recorder.exportRun(zipPath, boundary, QStringLiteral("现场说明"));
    if (!check(result.success, QStringLiteral("boundary export failed: %1").arg(result.error))) return false;
    QString error;
    const QHash<QString, QByteArray> files = readZip(zipPath, &error);
    if (!check(error.isEmpty(), error)) return false;
    const QStringList expected = {QStringLiteral("runtime.log"), QStringLiteral("events.jsonl"), QStringLiteral("network.json"),
                                  QStringLiteral("settings.json"), QStringLiteral("summary.txt"), QStringLiteral("manifest.json")};
    if (!check(files.size() == expected.size(), QStringLiteral("ZIP entry count mismatch"))) return false;
    for (const QString &name : expected) if (!check(files.contains(name), QStringLiteral("ZIP entry missing: %1").arg(name))) return false;
    if (!check(!files.value(QStringLiteral("events.jsonl")).contains("must be excluded"), QStringLiteral("post-boundary event leaked"))) return false;
    const QString summary = QString::fromUtf8(files.value(QStringLiteral("summary.txt")));
    return check(summary.contains(QStringLiteral("最终状态")) && summary.contains(QStringLiteral("失败请求"))
                     && summary.contains(QStringLiteral("现场说明")), QStringLiteral("summary grouping or failure history missing"));
}

bool testRuntimeCardCountersExport()
{
    QTemporaryDir temp;
    if (!check(temp.isValid(), QStringLiteral("temporary directory unavailable"))) return false;
    DiagnosticRecorder recorder(optionsFor(temp.path()));
    recorder.recordNetworkSnapshot(QJsonObject{
        {QStringLiteral("kind"), QStringLiteral("runtime_ingress")},
        {QStringLiteral("relevantInterfaces"), QJsonArray{
            QJsonObject{{QStringLiteral("interfaceIndex"), 7},
                        {QStringLiteral("friendlyName"), QStringLiteral("capture")},
                        {QStringLiteral("counters"), QJsonObject{
                            {QStringLiteral("InDiscards"), QJsonObject{{QStringLiteral("absolute"), QStringLiteral("100")}, {QStringLiteral("delta"), QStringLiteral("3")}}},
                            {QStringLiteral("InErrors"), QJsonObject{{QStringLiteral("absolute"), QStringLiteral("4")}, {QStringLiteral("delta"), QStringLiteral("1")}}}}}}}},
        {QStringLiteral("systemUdpCounters"), QJsonObject{
            {QStringLiteral("scope"), QStringLiteral("system_ipv4")},
            {QStringLiteral("InErrors"), QJsonObject{{QStringLiteral("absolute"), QStringLiteral("2")}, {QStringLiteral("delta"), QStringLiteral("1")}}}}},
        {QStringLiteral("receiverGroups"), QJsonArray{QJsonObject{{QStringLiteral("maxDrainPackets"), 8}}}}
    });
    const QJsonObject fields{
        {QStringLiteral("socketPacketsReceived"), 1000},
        {QStringLiteral("processorPacketsDequeued"), 900},
        {QStringLiteral("batchBoundaryDiscards"), 0},
        {QStringLiteral("sameTriggerForwardGapEvents"), 2},
        {QStringLiteral("sameTriggerForwardGapPackets"), 4},
        {QStringLiteral("sameTriggerBackstepEvents"), 1},
        {QStringLiteral("sameTriggerDuplicateSeqEvents"), 3},
        {QStringLiteral("crossTriggerLateArrivalEvents"), 1},
        {QStringLiteral("staleTriggerPacketsDiscarded"), 5},
        {QStringLiteral("assemblyDuplicatePackets"), 6},
        {QStringLiteral("assemblyOffsetOutOfRangePackets"), 7},
        {QStringLiteral("packetsDropped"), 7},
        {QStringLiteral("triggersPartial"), 3},
        {QStringLiteral("inputQueueDepth"), 100},
        {QStringLiteral("saveQueueDepth"), 5}
    };
    recorder.setCardSnapshot(1, QStringLiteral("127.0.0.2"), QStringLiteral("runtime"), fields);
    const QString zipPath = QDir(temp.path()).filePath(QStringLiteral("runtime-counters.zip"));
    const DiagnosticRecorder::ExportResult result = recorder.exportRun(zipPath);
    if (!check(result.success, QStringLiteral("runtime counter export failed: %1").arg(result.error))) return false;
    QString error;
    const QHash<QString, QByteArray> files = readZip(zipPath, &error);
    if (!check(error.isEmpty(), error)) return false;
    const QJsonObject network = QJsonDocument::fromJson(files.value(QStringLiteral("network.json"))).object();
    bool ingressFound = false;
    for (const QJsonValue &value : network.value(QStringLiteral("snapshots")).toArray()) {
        const QJsonObject data = value.toObject().value(QStringLiteral("data")).toObject();
        if (data.value(QStringLiteral("kind")).toString() == QStringLiteral("runtime_ingress")) {
            ingressFound = data.value(QStringLiteral("relevantInterfaces")).toArray().size() == 1
                && data.value(QStringLiteral("systemUdpCounters")).toObject()
                       .value(QStringLiteral("scope")).toString() == QStringLiteral("system_ipv4")
                && data.value(QStringLiteral("receiverGroups")).toArray().size() == 1;
        }
    }
    bool ok = check(ingressFound, QStringLiteral("runtime ingress snapshot not exported"));
    const QString summary = QString::fromUtf8(files.value(QStringLiteral("summary.txt")));
    ok = check(summary.contains(QStringLiteral("网络入口观测汇总"))
                   && summary.contains(QStringLiteral("scope=system_ipv4")),
               QStringLiteral("network ingress summary not exported")) && ok;
    const QJsonArray cards = network.value(QStringLiteral("cards")).toArray();
    if (!check(cards.size() == 1, QStringLiteral("runtime card snapshot missing"))) return false;
    const QJsonObject card = cards.first().toObject();
    const QJsonObject cardFields = card.value(QStringLiteral("fields")).toObject();
    ok = check(cardFields.value(QStringLiteral("socketPacketsReceived")).toDouble() == 1000,
                    QStringLiteral("socket counter not exported"));
    ok = check(cardFields.value(QStringLiteral("processorPacketsDequeued")).toDouble() == 900,
               QStringLiteral("processor counter not exported")) && ok;
    ok = check(cardFields.value(QStringLiteral("batchBoundaryDiscards")).toDouble() == 0,
               QStringLiteral("batch boundary counter not exported")) && ok;
    ok = check(cardFields.value(QStringLiteral("sameTriggerForwardGapEvents")).toDouble() == 2
                   && cardFields.value(QStringLiteral("sameTriggerForwardGapPackets")).toDouble() == 4
                   && cardFields.value(QStringLiteral("sameTriggerBackstepEvents")).toDouble() == 1
                   && cardFields.value(QStringLiteral("sameTriggerDuplicateSeqEvents")).toDouble() == 3
                   && cardFields.value(QStringLiteral("crossTriggerLateArrivalEvents")).toDouble() == 1
                   && cardFields.value(QStringLiteral("staleTriggerPacketsDiscarded")).toDouble() == 5
                   && cardFields.value(QStringLiteral("assemblyDuplicatePackets")).toDouble() == 6
                   && cardFields.value(QStringLiteral("assemblyOffsetOutOfRangePackets")).toDouble() == 7,
               QStringLiteral("new ingress/rejection fields not exported")) && ok;
    ok = check(cardFields.value(QStringLiteral("packetsDropped")).toDouble() == 7
                   && cardFields.value(QStringLiteral("triggersPartial")).toDouble() == 3
                   && cardFields.value(QStringLiteral("inputQueueDepth")).toDouble() == 100
                   && cardFields.value(QStringLiteral("saveQueueDepth")).toDouble() == 5,
               QStringLiteral("legacy runtime fields not exported")) && ok;
    const QJsonObject manifest = QJsonDocument::fromJson(files.value(QStringLiteral("manifest.json"))).object();
    ok = check(!manifest.value(QStringLiteral("dropCounters")).toObject().contains(QStringLiteral("socketPacketsReceived"))
                   && !manifest.value(QStringLiteral("dropCounters")).toObject().contains(QStringLiteral("processorPacketsDequeued"))
                   && !manifest.value(QStringLiteral("dropCounters")).toObject().contains(QStringLiteral("batchBoundaryDiscards")),
               QStringLiteral("recorder drop counters were not polluted")) && ok;
    return ok;
}

bool testDiskHistoryAndWriteFallback()
{
    QTemporaryDir temp;
    if (!check(temp.isValid(), QStringLiteral("temporary directory unavailable"))) return false;
    DiagnosticRecorder::Options options = optionsFor(temp.path());
    options.maxSnapshotEntries = 1;
    DiagnosticRecorder recorder(options);
    for (int i = 0; i < 4; ++i) recorder.recordNetworkSnapshot(QJsonObject{{QStringLiteral("index"), i}});
    const QString historyZip = QDir(temp.path()).filePath(QStringLiteral("history.zip"));
    const DiagnosticRecorder::ExportResult historyResult = recorder.exportRun(historyZip);
    if (!check(historyResult.success, QStringLiteral("history export failed: %1").arg(historyResult.error))) return false;
    QString error;
    const QHash<QString, QByteArray> historyFiles = readZip(historyZip, &error);
    if (!check(error.isEmpty(), error)) return false;
    const QJsonObject network = QJsonDocument::fromJson(historyFiles.value(QStringLiteral("network.json"))).object();
    if (!check(network.value(QStringLiteral("snapshots")).toArray().size() == 4, QStringLiteral("disk snapshot history was truncated"))) return false;

    DiagnosticRecorder::Options failOptions = optionsFor(QDir(temp.path()).filePath(QStringLiteral("writefail")));
    failOptions.maxRetainedBytes = 1;
    DiagnosticRecorder failing(failOptions);
    failing.recordEvent(QStringLiteral("fallback"), QStringLiteral("kept in memory"), DiagnosticRecorder::Severity::Error);
    const QString failZip = QDir(temp.path()).filePath(QStringLiteral("writefail.zip"));
    const DiagnosticRecorder::ExportResult failResult = failing.exportRun(failZip);
    if (!check(failing.status().writeError, QStringLiteral("write capacity failure was not reported"))) return false;
    if (!check(failResult.success, QStringLiteral("write-fallback export failed: %1").arg(failResult.error))) return false;
    const QHash<QString, QByteArray> failFiles = readZip(failZip, &error);
    return check(QString::fromUtf8(failFiles.value(QStringLiteral("events.jsonl"))).contains("kept in memory"),
                 QStringLiteral("fallback event missing from export"));
}

bool testQueueRetention()
{
    QTemporaryDir temp;
    if (!check(temp.isValid(), QStringLiteral("temporary directory unavailable"))) return false;
    DiagnosticRecorder::Options options = optionsFor(temp.path());
    options.maxQueueEntries = 2;
    options.maxRetainedRuns = 2;
    {
        DiagnosticRecorder recorder(options);
        for (int i = 0; i < 1000; ++i) recorder.recordEvent(QStringLiteral("burst"), QString::number(i), DiagnosticRecorder::Severity::Debug);
        const QString zipPath = QDir(temp.path()).filePath(QStringLiteral("queue.zip"));
        const DiagnosticRecorder::ExportResult result = recorder.exportRun(zipPath);
        if (!check(result.success, QStringLiteral("queue export failed: %1").arg(result.error))) return false;
        recorder.flush(5000);
    }
    const QVector<DiagnosticRecorder::RunInfo> runs = DiagnosticRecorder::enumerateRuns(temp.path());
    return check(runs.size() <= 2, QStringLiteral("retention limit was not enforced"));
}

bool testStaticHistoricalExportAndProducers()
{
    QTemporaryDir temp;
    if (!check(temp.isValid(), QStringLiteral("temporary directory unavailable"))) return false;
    DiagnosticRecorder::Options options = optionsFor(temp.path());
    options.maxQueueEntries = 64;
    options.maxFallbackEntries = 4096;
    QString historicalRoot;
    {
        DiagnosticRecorder recorder(options);
        historicalRoot = recorder.runDirectory();
        for (int i = 0; i < 8; ++i) {
            recorder.recordEvent(QStringLiteral("historical"), QString::number(i));
            recorder.recordNetworkSnapshot(QJsonObject{{QStringLiteral("sample"), i}});
        }
        recorder.flush(5000);
    }
    const QVector<DiagnosticRecorder::RunInfo> runs = DiagnosticRecorder::enumerateRuns(temp.path());
    const auto historical = std::find_if(runs.cbegin(), runs.cend(), [&](const DiagnosticRecorder::RunInfo &run) {
        return run.directory == historicalRoot;
    });
    if (!check(historical != runs.cend() && !historical->active, QStringLiteral("historical run was not enumerated after shutdown"))) return false;
    DiagnosticRecorder::ExportRequest request;
    request.runId = historical->runId;
    request.sourceDirectory = historical->directory;
    request.startIsoTime = historical->startIsoTime;
    request.endIsoTime = historical->endIsoTime;
    request.boundarySequence = historical->lastSequence;
    const QString zipPath = QDir(temp.path()).filePath(QStringLiteral("historical-static.zip"));
    const DiagnosticRecorder::ExportResult result = DiagnosticRecorder::exportRequest(request, zipPath);
    if (!check(result.success, QStringLiteral("static historical export failed: %1").arg(result.error))) return false;
    QString error;
    const QHash<QString, QByteArray> files = readZip(zipPath, &error);
    if (!check(error.isEmpty(), error)) return false;
    if (!check(files.value(QStringLiteral("events.jsonl")).count('\n') == 8, QStringLiteral("historical event count mismatch"))) return false;
    const QJsonObject network = QJsonDocument::fromJson(files.value(QStringLiteral("network.json"))).object();
    return check(network.value(QStringLiteral("snapshots")).toArray().size() == 8, QStringLiteral("historical snapshot count mismatch"));
}

bool testConcurrentProducers()
{
    QTemporaryDir temp;
    if (!check(temp.isValid(), QStringLiteral("temporary directory unavailable"))) return false;
    DiagnosticRecorder::Options options = optionsFor(temp.path());
    options.maxQueueEntries = 256;
    options.maxFallbackEntries = 4096;
    options.noiseBurst = 0;
    DiagnosticRecorder recorder(options);
    constexpr int producerCount = 4;
    constexpr int eventsPerProducer = 250;
    std::vector<std::thread> producers;
    for (int producer = 0; producer < producerCount; ++producer) {
        producers.emplace_back([&recorder, producer] {
            for (int i = 0; i < eventsPerProducer; ++i)
                recorder.recordEvent(QStringLiteral("producer"), QStringLiteral("%1:%2").arg(producer).arg(i), DiagnosticRecorder::Severity::Error);
        });
    }
    for (std::thread &producer : producers) producer.join();
    const QString zipPath = QDir(temp.path()).filePath(QStringLiteral("concurrent.zip"));
    const DiagnosticRecorder::ExportResult result = recorder.exportRun(zipPath);
    if (!check(result.success, QStringLiteral("concurrent export failed: %1").arg(result.error))) return false;
    QString error;
    const QHash<QString, QByteArray> files = readZip(zipPath, &error);
    if (!check(error.isEmpty(), error)) return false;
    return check(files.value(QStringLiteral("events.jsonl")).count('\n') == producerCount * eventsPerProducer,
                 QStringLiteral("concurrent producer events were lost before export"));
}

bool testUnavailableDirectoryAndExistingExport()
{
    QTemporaryDir temp;
    const QString blocked = QDir(temp.path()).filePath("blocked");
    QFile blocker(blocked);
    if (!blocker.open(QIODevice::WriteOnly)) return false;
    blocker.write("do not overwrite");
    blocker.close();
    DiagnosticRecorder recorder(optionsFor(blocked));
    recorder.recordEvent("fallback", "memory-only evidence", DiagnosticRecorder::Severity::Error);
    const auto request = recorder.captureExportRequest();
    const QString target = QDir(temp.path()).filePath("memory.zip");
    auto result = DiagnosticRecorder::exportRequest(request, target);
    if (!check(result.success, "memory-only export failed: " + result.error)) return false;
    QString error;
    const auto files = readZip(target, &error);
    if (!check(files.value("events.jsonl").contains("memory-only evidence"), "memory evidence missing")) return false;
    if (!check(!QJsonDocument::fromJson(files.value("manifest.json")).object().value("complete").toBool(), "missing directory marked complete")) return false;
    QFile original(target);
    original.open(QIODevice::ReadOnly);
    const QByteArray bytes = original.readAll();
    original.close();
    if (!check(!DiagnosticRecorder::exportRequest(request, target).success, "existing ZIP was overwritten")) return false;
    original.open(QIODevice::ReadOnly);
    return check(original.readAll() == bytes, "existing ZIP changed");
}

bool testAbnormalHistory()
{
    QTemporaryDir temp;
    QString directory;
    {
        DiagnosticRecorder recorder(optionsFor(temp.path()));
        directory = recorder.runDirectory();
        recorder.recordEvent("crash-fixture", "last persisted event");
    }
    QFile manifestFile(QDir(directory).filePath("manifest.json"));
    manifestFile.open(QIODevice::ReadOnly);
    QJsonObject manifest = QJsonDocument::fromJson(manifestFile.readAll()).object();
    manifestFile.close();
    manifest.insert("active", true);
    manifest.insert("endTime", "");
    manifest.insert("boundarySequence", 0);
    manifestFile.open(QIODevice::WriteOnly | QIODevice::Truncate);
    manifestFile.write(QJsonDocument(manifest).toJson());
    manifestFile.close();
    const auto runs = DiagnosticRecorder::enumerateRuns(temp.path());
    if (!check(runs.size() == 1 && runs.first().lastSequence > 0, "abnormal cutoff not recovered")) return false;
    DiagnosticRecorder::ExportRequest request;
    request.sourceDirectory = directory;
    const QString target = QDir(temp.path()).filePath("abnormal.zip");
    const auto result = DiagnosticRecorder::exportRequest(request, target);
    QString error;
    const auto files = readZip(target, &error);
    return check(result.success && files.value("events.jsonl").contains("last persisted event")
        && !QJsonDocument::fromJson(files.value("manifest.json")).object().value("complete").toBool(),
        "abnormal history evidence/completeness incorrect");
}

bool testStableRetentionAge()
{
    QTemporaryDir temp;
    if (!temp.isValid()) return false;
    auto options = optionsFor(temp.path());
    const auto now = QDateTime::currentDateTime();
    const auto create = [&](const QString &id, int days, bool legacy) {
        const auto path = temp.filePath("run-" + id);
        if (!QDir().mkpath(path)) return QString{};
        QFile manifest(QDir(path).filePath("manifest.json"));
        if (!manifest.open(QIODevice::WriteOnly)) return QString{};
        const QJsonObject meta{{"formatVersion", 1}, {"runId", id}, {"active", legacy},
            {"endTime", legacy ? QString() : now.addDays(-days).toString(Qt::ISODateWithMs)}};
        manifest.write(QJsonDocument(meta).toJson());
        manifest.close();
        QFile events(QDir(path).filePath("events.jsonl"));
        if (!events.open(QIODevice::ReadWrite) || events.write("event") != 5 || !events.flush() ||
            (legacy && !events.setFileTime(now.addDays(-days), QFileDevice::FileModificationTime))) return QString{};
        return path;
    };
    const auto expired = create("expired", 9, false);
    const auto expiredLegacy = create("expired-legacy", 8, true);
    const auto older = create("older", 5, true);
    const auto newer = create("newer", 2, false);
    const auto active = create("active", 10, false);
    const auto unknown = create("unknown", 10, false);
    if (expired.isEmpty() || expiredLegacy.isEmpty() || older.isEmpty() || newer.isEmpty() ||
        active.isEmpty() || unknown.isEmpty()) return false;
    QFile sentinel(QDir(unknown).filePath("user-file"));
    if (!sentinel.open(QIODevice::WriteOnly)) return false;
    sentinel.close();
    QLockFile activeLock(QDir(active).filePath(".active.lock"));
    if (!activeLock.tryLock(0)) return false;
    for (int i = 0; i < 4; ++i)
        DiagnosticRecorder::enumerateRuns(temp.path());
    DiagnosticRecorder ageOnly(options);
    ageOnly.recordEvent("stable-retention", "age-pruned");
    if (!check(ageOnly.flush(3000) && !QDir(expired).exists() && !QDir(expiredLegacy).exists() &&
               QDir(older).exists() && QDir(newer).exists(),
               "age-only pruning expires old ended and legacy runs after repeated enumeration"))
        return false;
    options.maxRetainedRuns = 1;
    DiagnosticRecorder current(options);
    current.recordEvent("stable-retention", "admitted");
    return check(current.flush(3000) && !QDir(older).exists() && QDir(newer).exists() && QDir(active).exists() &&
                 QFile::exists(sentinel.fileName()),
                 "retention uses the end time and legacy data age while preserving active and unknown contents");
}

bool testRetentionAdmissionRecovery()
{
    QTemporaryDir temp;
    auto options = optionsFor(temp.path());
    options.maxRetainedBytes = 65536;
    options.maxHistorySegmentBytes = 8192;
    QString oldPath;
    {
        DiagnosticRecorder old(options);
        old.recordEvent("old", "historical", DiagnosticRecorder::Severity::Warning);
        if (!check(old.flush(), "old run flush failed")) return false;
        oldPath = old.runDirectory();
    }
    // Enlarge a known historical file so that admission requires its deletion.
    QFile oldFile(QDir(oldPath).filePath("runtime.log"));
    if (!oldFile.open(QIODevice::Append) || oldFile.write(QByteArray(48000, 'x')) != 48000) return false;
    oldFile.close();
    QLockFile pin(QDir(oldPath).filePath(".history.lock"));
    pin.setStaleLockTime(0);
    if (!check(pin.tryLock(0), "history pin failed")) return false;
    DiagnosticRecorder current(options);
    current.recordEvent("new", "after release", DiagnosticRecorder::Severity::Warning);
    const auto now = QDateTime::currentDateTime();
    auto request = current.captureTimeWindowRequest(now.addSecs(-60), now.addSecs(60));
    if (!check(request.validationError.isEmpty() && request.retentionPin, "historical request did not retain root")) return false;
    pin.unlock();
    const auto lockDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!current.status().writeErrorText.contains(QStringLiteral("保留锁繁忙")) && std::chrono::steady_clock::now() < lockDeadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto pending = current.status();
    if (!check(pending.writeErrorText.contains(QStringLiteral("保留锁繁忙")), "retry did not observe root retention pin")) return false;
    if (!check(pending.writeError && pending.queuedEntries > 0 && !current.flush(100), "blocked admission not visible")
        || !check(QFileInfo::exists(oldFile.fileName()) && !QFileInfo::exists(QDir(current.runDirectory()).filePath("events.jsonl")), "blocked admission wrote formal files")) return false;
    request = {};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (current.status().writeError && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (!check(!current.status().writeError && current.flush(3000), "admission did not recover")) return false;
    QFile newFile(QDir(current.runDirectory()).filePath("events.jsonl"));
    if (!newFile.open(QIODevice::ReadOnly)) return false;
    qint64 total = 0;
    for (const auto &dir : QDir(temp.path()).entryInfoList({"run-*"}, QDir::Dirs | QDir::NoDotAndDotDot))
        for (const auto &file : QDir(dir.filePath()).entryInfoList(QDir::Files | QDir::Hidden | QDir::System)) total += file.size();
    return check(newFile.readAll().contains("after release") && !QFileInfo::exists(oldPath) && total <= options.maxRetainedBytes,
                 "recovery did not prune or retain the queued event within quota");
}

bool testSerializedSnapshotQuota()
{
    QTemporaryDir temp;
    auto options = optionsFor(temp.path());
    // Give both records identical top-level shape but very different UTF-8 sizes.
    options.maxHistorySegmentBytes = 2048;
    DiagnosticRecorder recorder(options);
    const QDir run(recorder.runDirectory());
    const auto size = [&](const QString &name) { return QFileInfo(run.filePath(name)).size(); };
    const auto payload = [](int length) {
        return QJsonObject{{"nested", QJsonObject{{"content", QString(length, QChar(0x4e2d))}}}};
    };
    recorder.recordNetworkSnapshot(payload(5));
    if (!check(recorder.flush(), "small snapshot failed")) return false;
    const qint64 first = size("network_history.jsonl");
    recorder.recordNetworkSnapshot(payload(490));
    if (!check(recorder.flush(), "large snapshot failed")) return false;
    const qint64 total = size("network_history.jsonl");
    if (!check(first > 15 && total - first > 1470 && total < 2048,
               "snapshot increments did not reflect UTF-8 serialization")) return false;
    recorder.recordSettingsSnapshot(payload(490));
    if (!check(recorder.flush(), "cross-file snapshot rotation failed")) return false;
    const qint64 current = size("settings_history.jsonl");
    if (!check(size("network_history.jsonl.previous") == total && size("network_history.jsonl") == 0
                   && current > 1470 && current <= 2048,
               "aggregate snapshot bytes did not drive rotation")) return false;
    recorder.recordSettingsSnapshot(payload(700));
    if (!check(!recorder.flush() && recorder.status().writeErrorText.contains(QStringLiteral("单条"))
                   && size("settings_history.jsonl") == current,
               "oversize snapshot reached disk or did not report failure")) return false;
    return check(size(".reservation") > 0 && !recorder.captureExportRequest().captureError.size(),
                 "rotated snapshots did not fit the unchanged export admission limit");
}

bool testLiveHistoryRotation()
{
    QTemporaryDir temp;
    DiagnosticRecorder::Options options;
    options.rootDirectory=temp.path();
    options.maxHistorySegmentBytes=4096;
    options.noiseBurst=1000;
    DiagnosticRecorder recorder(options);
    for(int i=0;i<64;++i)
        recorder.recordEvent("rotation",QString("%1:%2").arg(i).arg(QString(256,'x')));
    if(!check(recorder.flush()&&!recorder.status().writeError,"rotation stopped logging"))return false;
    const QDir run(recorder.runDirectory());
    if(!check(run.exists("events.jsonl.previous")&&run.exists("history-retention.json"),"rotation receipt missing"))return false;
    const auto target=temp.filePath("rotated.zip");
    auto cut=recorder.captureExportRequest();
    const auto result=DiagnosticRecorder::exportRequest(cut,target);
    QString error;
    const auto events=readZip(target,&error).value("events.jsonl");
    return check(result.success&&!result.truncationReasons.isEmpty()&&events.contains("63:xxx")&&
        !events.contains("\"message\":\"0:xxx"),"latest evidence or eviction indication lost");
}

bool testExportDoesNotBlockRotation()
{
    QTemporaryDir temp;
    if (!check(temp.isValid(), "temporary directory unavailable")) return false;
    auto options = optionsFor(temp.path());
    options.maxHistorySegmentBytes = 6144;
    options.maxFallbackEntries = 256;
    DiagnosticRecorder recorder(options);
    const QString padding(512, QLatin1Char('x'));
    auto writeBatch = [&](const QString &prefix, int count) {
        for (int i = 0; i < count; ++i)
            recorder.recordEvent("rotation-export", prefix + QString::number(i) + ":" + padding,
                                 DiagnosticRecorder::Severity::Warning);
        return recorder.flush(3000);
    };
    if (!check(writeBatch("early-", 4), "initial flush failed")) return false;
    auto first = recorder.captureExportRequest();
    // Hold a captured request without starting the exporter while its source rotates.
    if (!check(writeBatch("middle-", 24), "pre-export rotation flush failed")) return false;
    auto second = recorder.captureExportRequest();
    { auto abandoned = recorder.captureExportRequest(); Q_UNUSED(abandoned); }

    std::promise<void> releasePromise, enteredFirst, enteredSecond;
    const auto release = releasePromise.get_future().share();
    auto firstEntered = enteredFirst.get_future();
    auto secondEntered = enteredSecond.get_future();
    first.bundleExtension = [&](const DiagnosticRecorder::BundleSink &, QString *) {
        enteredFirst.set_value(); release.wait(); return true;
    };
    second.bundleExtension = [&](const DiagnosticRecorder::BundleSink &, QString *) {
        enteredSecond.set_value(); release.wait(); return true;
    };
    const QString firstPath = temp.filePath("first.zip");
    const QString secondPath = temp.filePath("second.zip");
    auto firstFuture = std::async(std::launch::async, [&] { return DiagnosticRecorder::exportRequest(first, firstPath); });
    auto secondFuture = std::async(std::launch::async, [&] { return DiagnosticRecorder::exportRequest(second, secondPath); });
    const bool bothEntered = firstEntered.wait_for(std::chrono::seconds(5)) == std::future_status::ready
        && secondEntered.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    bool flushed = false;
    if (bothEntered) flushed = writeBatch("late-", 24);
    const auto status = recorder.status();
    const QDir run(recorder.runDirectory());
    QFile current(run.filePath("events.jsonl")), previous(run.filePath("events.jsonl.previous"));
    QByteArray retained;
    if (current.open(QIODevice::ReadOnly)) retained += current.readAll();
    if (previous.open(QIODevice::ReadOnly)) retained += previous.readAll();
    current.close();
    previous.close();
    const bool receipt = run.exists("history-retention.json");
    // Always release both workers before reporting a failure or destroying futures.
    releasePromise.set_value();
    const auto firstResult = firstFuture.get();
    const auto secondResult = secondFuture.get();
    bool ok = check(bothEntered, "concurrent export extensions did not start")
        && check(flushed, "flush timed out during exports")
        && check(status.drops.queueDropped == 0 && status.drops.criticalQueueDropped == 0 && !status.writeError,
                 "export caused dropped or failed writes")
        && check(receipt && retained.contains("late-23:") && !retained.contains("late-0:"),
                 "multiple rotations or retained formal events missing")
        && check(firstResult.success && secondResult.success, "concurrent exports failed");
    QString error;
    const auto firstFiles = readZip(firstPath, &error);
    ok = check(error.isEmpty(), error) && ok;
    const QByteArray firstEvents = firstFiles.value("events.jsonl");
    for (int i = 0; i < 4; ++i)
        ok = check(firstEvents.count((QString("early-%1:").arg(i)).toUtf8()) == 1,
                   "first capture lost or duplicated an event") && ok;
    ok = check(!firstEvents.contains("middle-") && !firstEvents.contains("late-"),
               "post-boundary event crossed first capture") && ok;
    error.clear();
    const auto secondFiles = readZip(secondPath, &error);
    ok = check(error.isEmpty() && secondFiles.value("events.jsonl").contains("middle-23:")
                   && !secondFiles.value("events.jsonl").contains("late-"),
               "second capture lost its boundary") && ok;

    auto failure = recorder.captureExportRequest();
    failure.bundleExtension = [](const DiagnosticRecorder::BundleSink &sink, QString *error) {
        sink("partial.txt", "partial");
        if (error) *error = "intentional extension failure";
        return false;
    };
    const QString failedPath = temp.filePath("failed.zip");
    const auto failed = DiagnosticRecorder::exportRequest(failure, failedPath);
    ok = check(!failed.success && !QFileInfo::exists(failedPath)
                   && QDir(temp.path()).entryList({"failed.zip.part-*"}, QDir::Files).isEmpty(),
               "failed export left output behind") && ok;
    failure = {};
    first = {};
    second = {};
    ok = check(writeBatch("after-", 12), "logging did not recover after export requests were released") && ok;
    const QString laterPath = temp.filePath("later.zip");
    const auto laterResult = recorder.exportRun(laterPath);
    error.clear();
    const auto laterFiles = readZip(laterPath, &error);
    return check(laterResult.success && error.isEmpty() && laterFiles.value("events.jsonl").contains("after-11:"),
                 "later export missing new records") && ok;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    bool ok = true;
    ok = testZipBoundaryAndSummary() && ok;
    ok = testRuntimeCardCountersExport() && ok;
    ok = testDiskHistoryAndWriteFallback() && ok;
    ok = testQueueRetention() && ok;
    ok = testStaticHistoricalExportAndProducers() && ok;
    ok = testConcurrentProducers() && ok;
    ok = testUnavailableDirectoryAndExistingExport() && ok;
    ok = testAbnormalHistory() && ok;
    ok = testRetentionAdmissionRecovery() && ok;
    ok = testStableRetentionAge() && ok;
    ok = testSerializedSnapshotQuota() && ok;
    ok = testLiveHistoryRotation() && ok;
    ok = testExportDoesNotBlockRotation() && ok;
    return ok ? 0 : 1;
}
