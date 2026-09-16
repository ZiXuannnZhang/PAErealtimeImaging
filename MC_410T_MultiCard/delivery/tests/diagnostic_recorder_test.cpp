#include "DiagnosticRecorder.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <array>
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

QHash<QString, QByteArray> readStoredZip(const QString &path, QString *error)
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
        const QByteArray data = bytes.mid(dataOffset, static_cast<int>(compressedSize));
        if (compressedSize != uncompressedSize || crc32(data) != expectedCrc) {
            if (error) *error = QStringLiteral("ZIP CRC or stored size mismatch for %1").arg(name);
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
    const QHash<QString, QByteArray> files = readStoredZip(zipPath, &error);
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
    const QHash<QString, QByteArray> files = readStoredZip(zipPath, &error);
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
    const QHash<QString, QByteArray> historyFiles = readStoredZip(historyZip, &error);
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
    const QHash<QString, QByteArray> failFiles = readStoredZip(failZip, &error);
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
    const QHash<QString, QByteArray> files = readStoredZip(zipPath, &error);
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
    const QHash<QString, QByteArray> files = readStoredZip(zipPath, &error);
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
    const auto files = readStoredZip(target, &error);
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
    const auto files = readStoredZip(target, &error);
    return check(result.success && files.value("events.jsonl").contains("last persisted event")
        && !QJsonDocument::fromJson(files.value("manifest.json")).object().value("complete").toBool(),
        "abnormal history evidence/completeness incorrect");
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
    return ok ? 0 : 1;
}
