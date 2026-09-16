#include "DiagnosticRecorder.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QTemporaryDir>
#include <QTextStream>

#include <array>

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
               const QVector<QPair<quint64, QString>> &events)
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
    const QJsonObject manifest{{QStringLiteral("formatVersion"), 1},
                               {QStringLiteral("runId"), runId},
                               {QStringLiteral("startTime"), iso(first)},
                               {QStringLiteral("endTime"), active ? QString() : iso(last)},
                               {QStringLiteral("boundarySequence"), static_cast<double>(events.isEmpty() ? 0 : events.constLast().first)},
                               {QStringLiteral("active"), active},
                               {QStringLiteral("complete"), !active}};
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
    const auto files = readStoredZip(zipPath, &error);
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
    const auto emptyFiles = readStoredZip(emptyZip, &error);
    if (!check(error.isEmpty() && QString::fromUtf8(emptyFiles.value(QStringLiteral("summary.txt"))).contains(QStringLiteral("该时间段没有日志")),
               QStringLiteral("empty window summary text missing"))) return false;
    return true;
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
    const auto files = readStoredZip(zipPath, &error);
    if (!check(error.isEmpty(), error)) return false;
    const QByteArray events = files.value(QStringLiteral("events.jsonl"));
    return check(events.contains("before-boundary") && !events.contains("after-boundary"),
                 QStringLiteral("post-capture active event crossed immutable export boundary"));
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    bool ok = testTimeWindowAcrossRuns();
    ok = testActiveImmutableBoundary() && ok;
    QTextStream(stdout) << (ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"))
                        << QStringLiteral(" diagnostic time-window multi-run/active-boundary/empty-window export")
                        << Qt::endl;
    return ok ? 0 : 1;
}
