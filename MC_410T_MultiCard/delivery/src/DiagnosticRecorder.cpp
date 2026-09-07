#include "DiagnosticRecorder.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QUuid>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>

namespace {
using Clock = std::chrono::steady_clock;
std::mutex g_instanceMutex;
DiagnosticRecorder *g_instance = nullptr;

QString defaultRootDirectoryLocal()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (base.isEmpty()) base = QDir::tempPath() + QStringLiteral("/MC410T_Receiver");
    return QDir(base).filePath(QStringLiteral("diagnostics"));
}

QString nowIso() { return QDateTime::currentDateTime().toString(Qt::ISODateWithMs); }

QString severityNameLocal(DiagnosticRecorder::Severity severity)
{
    switch (severity) {
    case DiagnosticRecorder::Severity::Trace: return QStringLiteral("trace");
    case DiagnosticRecorder::Severity::Debug: return QStringLiteral("debug");
    case DiagnosticRecorder::Severity::Info: return QStringLiteral("info");
    case DiagnosticRecorder::Severity::Warning: return QStringLiteral("warning");
    case DiagnosticRecorder::Severity::Error: return QStringLiteral("error");
    case DiagnosticRecorder::Severity::Critical: return QStringLiteral("critical");
    }
    return QStringLiteral("info");
}

bool isCritical(const DiagnosticRecorder::Event &event)
{
    return event.severity >= DiagnosticRecorder::Severity::Error
        || event.fields.value(QStringLiteral("critical")).toBool(false);
}

QByteArray jsonBytes(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }

bool writeAtomicBytes(const QString &path, const QByteArray &bytes, QString *error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = QStringLiteral("open %1: %2").arg(path, file.errorString());
        return false;
    }
    if (file.write(bytes) != bytes.size()) {
        if (error) *error = QStringLiteral("write %1: %2").arg(path, file.errorString());
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        if (error) *error = QStringLiteral("commit %1: %2").arg(path, file.errorString());
        return false;
    }
    return true;
}

bool readAllBytes(const QString &path, QByteArray *bytes, QString *error = nullptr)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = file.errorString();
        return false;
    }
    *bytes = file.readAll();
    if (file.error() != QFile::NoError) {
        if (error) *error = file.errorString();
        return false;
    }
    return true;
}

QJsonObject parseObject(const QByteArray &bytes, QString *error = nullptr)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (!document.isObject()) {
        if (error) *error = parseError.errorString();
        return QJsonObject();
    }
    return document.object();
}

QByteArray eventJsonBytes(const DiagnosticRecorder::Event &event)
{
    QJsonObject object;
    object.insert(QStringLiteral("sequence"), static_cast<double>(event.sequence));
    object.insert(QStringLiteral("runId"), event.runId);
    object.insert(QStringLiteral("timestamp"), event.isoTime);
    object.insert(QStringLiteral("monotonicMs"), static_cast<double>(event.monotonicMs));
    object.insert(QStringLiteral("category"), event.category);
    object.insert(QStringLiteral("message"), event.message);
    object.insert(QStringLiteral("severity"), severityNameLocal(event.severity));
    object.insert(QStringLiteral("fields"), event.fields);
    return jsonBytes(object) + '\n';
}

QByteArray runtimeLineBytes(const DiagnosticRecorder::Event &event)
{
    QString message = event.message;
    message.replace('\r', QStringLiteral("\\r"));
    message.replace('\n', QStringLiteral("\\n"));
    return QStringLiteral("[seq=%1] [%2] [%3] [%4] %5\n")
        .arg(event.sequence).arg(event.isoTime).arg(severityNameLocal(event.severity))
        .arg(event.category).arg(message).toUtf8();
}

QJsonObject snapshotJson(const DiagnosticRecorder::Snapshot &snapshot)
{
    QJsonObject object;
    object.insert(QStringLiteral("sequence"), static_cast<double>(snapshot.sequence));
    object.insert(QStringLiteral("runId"), snapshot.runId);
    object.insert(QStringLiteral("timestamp"), snapshot.isoTime);
    object.insert(QStringLiteral("monotonicMs"), static_cast<double>(snapshot.monotonicMs));
    object.insert(QStringLiteral("data"), snapshot.data);
    return object;
}

QJsonObject cardSnapshotJson(const DiagnosticRecorder::CardSnapshot &snapshot)
{
    QJsonObject object;
    object.insert(QStringLiteral("sequence"), static_cast<double>(snapshot.sequence));
    object.insert(QStringLiteral("runId"), snapshot.runId);
    object.insert(QStringLiteral("timestamp"), snapshot.isoTime);
    object.insert(QStringLiteral("monotonicMs"), static_cast<double>(snapshot.monotonicMs));
    object.insert(QStringLiteral("card"), snapshot.card);
    object.insert(QStringLiteral("ip"), snapshot.ip);
    object.insert(QStringLiteral("state"), snapshot.state);
    object.insert(QStringLiteral("fields"), snapshot.fields);
    for (auto it = snapshot.fields.constBegin(); it != snapshot.fields.constEnd(); ++it)
        if (!object.contains(it.key())) object.insert(it.key(), it.value());
    return object;
}

QJsonValue cardField(const QJsonObject &card, const QString &key)
{
    if (card.contains(key)) return card.value(key);
    return card.value(QStringLiteral("fields")).toObject().value(key);
}

QString jsonValueText(const QJsonValue &value)
{
    if (value.isUndefined() || value.isNull()) return QStringLiteral("未记录");
    if (value.isBool()) return value.toBool() ? QStringLiteral("是") : QStringLiteral("否");
    if (value.isString()) return value.toString().isEmpty() ? QStringLiteral("未记录") : value.toString();
    if (value.isDouble()) return QString::number(value.toDouble(), 'g', 15);
    if (value.isObject()) return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
    return QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
}

QString cardFieldText(const QJsonObject &card, const QStringList &keys)
{
    for (const QString &key : keys) {
        const QJsonValue value = cardField(card, key);
        if (!value.isUndefined() && !value.isNull()) return jsonValueText(value);
    }
    return QStringLiteral("未记录");
}

bool cardLooksFailed(const QJsonObject &card)
{
    const QString state = cardFieldText(card, {QStringLiteral("state"), QStringLiteral("result"), QStringLiteral("status")}).toLower();
    if (state.contains(QStringLiteral("fail")) || state.contains(QStringLiteral("error"))
        || state.contains(QStringLiteral("timeout")) || state.contains(QStringLiteral("失败"))
        || state.contains(QStringLiteral("错误")) || state.contains(QStringLiteral("超时"))) return true;
    const QJsonObject fields = card.value(QStringLiteral("fields")).toObject();
    for (const QString &key : {QStringLiteral("success"), QStringLiteral("ok"), QStringLiteral("ack"), QStringLiteral("configured")})
        if (fields.contains(key) && fields.value(key).isBool() && !fields.value(key).toBool()) return true;
    return false;
}

QByteArray summaryBytes(const QString &runId,
                        const QString &startIso,
                        const QString &endIso,
                        quint64 boundary,
                        bool active,
                        bool complete,
                        bool writeError,
                        const QString &writeErrorText,
                        const DiagnosticRecorder::DropCounters &drops,
                        const QJsonObject &network,
                        const QJsonObject &settings,
                        quint64 eventCount,
                        const QString &note,
                        const QStringList &truncationReasons)
{
    QStringList lines;
    lines << QStringLiteral("MC410T 诊断导出摘要")
          << QStringLiteral("运行ID：%1").arg(runId)
          << QStringLiteral("开始时间：%1").arg(startIso.isEmpty() ? QStringLiteral("未记录") : startIso)
          << QStringLiteral("结束时间：%1").arg(endIso.isEmpty() ? QStringLiteral("进行中") : endIso)
          << QStringLiteral("截止事件序号：%1").arg(boundary)
          << QStringLiteral("运行状态：%1").arg(active ? QStringLiteral("进行中") : QStringLiteral("已结束"))
          << QStringLiteral("记录完整性：%1").arg(complete ? QStringLiteral("完整") : QStringLiteral("存在缺失或截断，详见 manifest.json"))
          << QStringLiteral("事件数量（截止范围）：%1").arg(eventCount)
          << QStringLiteral("丢弃计数：队列=%1，关键队列=%2，噪声限流=%3，写入回退=%4")
                .arg(drops.queueDropped).arg(drops.criticalQueueDropped).arg(drops.noiseDropped).arg(drops.writeFallbackDropped)
          << (writeError ? QStringLiteral("写入状态：失败（%1）").arg(writeErrorText.isEmpty() ? QStringLiteral("原因未记录") : writeErrorText)
                         : QStringLiteral("写入状态：正常"));
    if (!note.isEmpty()) lines << QStringLiteral("现场说明：%1").arg(note);
    if (!truncationReasons.isEmpty()) lines << QStringLiteral("截断说明：%1").arg(truncationReasons.join(QStringLiteral("；")));

    lines << QString() << QStringLiteral("逐卡状态（按监听ID/配置ID分组；最终状态与失败请求均保留）：");
    const QJsonArray cards = network.value(QStringLiteral("cards")).toArray();
    QHash<QString, QVector<QJsonObject>> groups;
    QStringList order;
    for (const QJsonValue &value : cards) {
        const QJsonObject card = value.toObject();
        const QString listenId = cardFieldText(card, {QStringLiteral("listenId"), QStringLiteral("listenID")});
        const QString configId = cardFieldText(card, {QStringLiteral("configId"), QStringLiteral("configID")});
        const QString cardNo = cardFieldText(card, {QStringLiteral("card")});
        const QString ip = cardFieldText(card, {QStringLiteral("ip"), QStringLiteral("targetIp"), QStringLiteral("targetIP")});
        QString key = listenId + QChar('\n') + configId + QChar('\n') + cardNo + QChar('\n') + ip;
        if (listenId == QStringLiteral("未记录") && configId == QStringLiteral("未记录")) key += QStringLiteral("\nseq=") + cardFieldText(card, {QStringLiteral("sequence")});
        if (!groups.contains(key)) order.append(key);
        groups[key].append(card);
    }
    if (order.isEmpty()) {
        lines << QStringLiteral("暂无逐卡快照，相关字段为未记录。");
    } else {
        for (const QString &key : order) {
            const QVector<QJsonObject> history = groups.value(key);
            const QJsonObject &last = history.constLast();
            lines << QStringLiteral("请求 listenId=%1 configId=%2 最终状态：卡%3 IP=%4 状态=%5 序号=%6")
                         .arg(cardFieldText(last, {QStringLiteral("listenId"), QStringLiteral("listenID")}))
                         .arg(cardFieldText(last, {QStringLiteral("configId"), QStringLiteral("configID")}))
                         .arg(cardFieldText(last, {QStringLiteral("card")}))
                         .arg(cardFieldText(last, {QStringLiteral("ip"), QStringLiteral("targetIp"), QStringLiteral("targetIP")}))
                         .arg(cardFieldText(last, {QStringLiteral("state"), QStringLiteral("result"), QStringLiteral("status")}))
                         .arg(cardFieldText(last, {QStringLiteral("sequence")}));
            for (const QJsonObject &entry : history) {
                if (!cardLooksFailed(entry)) continue;
                lines << QStringLiteral("  失败请求：卡%1 IP=%2 状态=%3 序号=%4")
                             .arg(cardFieldText(entry, {QStringLiteral("card")}))
                             .arg(cardFieldText(entry, {QStringLiteral("ip"), QStringLiteral("targetIp"), QStringLiteral("targetIP")}))
                             .arg(cardFieldText(entry, {QStringLiteral("state"), QStringLiteral("result"), QStringLiteral("status")}))
                             .arg(cardFieldText(entry, {QStringLiteral("sequence")}));
            }
        }
    }
    lines << QString() << QStringLiteral("监听/配置快照（完整 JSON）：");
    const QJsonArray settingSnapshots = settings.value(QStringLiteral("snapshots")).toArray();
    if (settingSnapshots.isEmpty()) lines << QStringLiteral("暂无监听或配置快照。");
    else for (const QJsonValue &value : settingSnapshots) lines << QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
    return (lines.join(QStringLiteral("\n")) + QStringLiteral("\n")).toUtf8();
}

QJsonArray readHistoryArray(const QString &path, quint64 boundary, QStringList *truncationReasons, bool *exists)
{
    QJsonArray result;
    QFile file(path);
    if (!file.exists()) {
        if (exists) *exists = false;
        return result;
    }
    if (exists) *exists = true;
    if (!file.open(QIODevice::ReadOnly)) {
        if (truncationReasons) truncationReasons->append(QStringLiteral("无法读取 %1：%2").arg(path, file.errorString()));
        return result;
    }
    while (!file.atEnd()) {
        const QByteArray line = file.readLine();
        if (line.trimmed().isEmpty()) continue;
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            if (truncationReasons) truncationReasons->append(QStringLiteral("%1 存在无法解析的记录").arg(path));
            continue;
        }
        const QJsonObject object = document.object();
        const quint64 sequence = static_cast<quint64>(object.value(QStringLiteral("sequence")).toDouble(0));
        if (sequence != 0 && sequence <= boundary) result.append(object);
    }
    return result;
}

void appendUniqueBySequence(QJsonArray *target, const QJsonArray &source, QSet<quint64> *seen, quint64 boundary)
{
    if (!target || !seen) return;
    for (const QJsonValue &value : source) {
        const quint64 sequence = static_cast<quint64>(value.toObject().value(QStringLiteral("sequence")).toDouble(0));
        if (sequence == 0 || sequence > boundary || seen->contains(sequence)) continue;
        seen->insert(sequence);
        target->append(value);
    }
}

QJsonObject makeSnapshotDocument(const QString &runId, quint64 boundary, const QJsonArray &snapshots, const QJsonArray &cards = QJsonArray())
{
    QJsonObject object;
    object.insert(QStringLiteral("formatVersion"), 1);
    object.insert(QStringLiteral("runId"), runId);
    object.insert(QStringLiteral("boundarySequence"), static_cast<double>(boundary));
    object.insert(QStringLiteral("snapshots"), snapshots);
    object.insert(QStringLiteral("cards"), cards);
    return object;
}

void appendDropReasons(const DiagnosticRecorder::DropCounters &drops, QStringList *reasons)
{
    if (!reasons) return;
    if (drops.queueDropped || drops.criticalQueueDropped) reasons->append(QStringLiteral("队列丢弃：普通=%1，关键=%2").arg(drops.queueDropped).arg(drops.criticalQueueDropped));
    if (drops.noiseDropped) reasons->append(QStringLiteral("噪声限流丢弃：%1").arg(drops.noiseDropped));
    if (drops.writeFallbackDropped) reasons->append(QStringLiteral("写入回退内存超限丢弃：%1").arg(drops.writeFallbackDropped));
}

struct FilteredEvents { QByteArray runtime; QByteArray events; quint64 count = 0; };

FilteredEvents readFilteredEvents(const QString &directory, quint64 boundary, const QVector<DiagnosticRecorder::Event> &fallback,
                                  QStringList *missing, QStringList *truncationReasons)
{
    FilteredEvents result;
    QMap<quint64, QByteArray> eventLines;
    QMap<quint64, QByteArray> runtimeLines;
    const QString eventPath = QDir(directory).filePath(QStringLiteral("events.jsonl"));
    QFile eventFile(eventPath);
    if (!eventFile.exists()) {
        if (missing) missing->append(QStringLiteral("events.jsonl"));
    } else if (!eventFile.open(QIODevice::ReadOnly)) {
        if (truncationReasons) truncationReasons->append(QStringLiteral("无法读取 events.jsonl：%1").arg(eventFile.errorString()));
    } else {
        while (!eventFile.atEnd()) {
            const QByteArray line = eventFile.readLine();
            if (line.trimmed().isEmpty()) continue;
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
                if (truncationReasons) truncationReasons->append(QStringLiteral("events.jsonl 存在无法解析的记录"));
                continue;
            }
            const QJsonObject object = document.object();
            const quint64 sequence = static_cast<quint64>(object.value(QStringLiteral("sequence")).toDouble(0));
            if (sequence != 0 && sequence <= boundary && !eventLines.contains(sequence)) eventLines.insert(sequence, QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n');
        }
    }
    const QString runtimePath = QDir(directory).filePath(QStringLiteral("runtime.log"));
    QFile runtimeFile(runtimePath);
    if (!runtimeFile.exists()) {
        if (missing) missing->append(QStringLiteral("runtime.log"));
    } else if (!runtimeFile.open(QIODevice::ReadOnly)) {
        if (truncationReasons) truncationReasons->append(QStringLiteral("无法读取 runtime.log：%1").arg(runtimeFile.errorString()));
    } else {
        const QRegularExpression expression(QStringLiteral("^\\[seq=(\\d+)\\]"));
        while (!runtimeFile.atEnd()) {
            const QByteArray line = runtimeFile.readLine();
            const QRegularExpressionMatch match = expression.match(QString::fromUtf8(line));
            if (!match.hasMatch()) {
                if (truncationReasons) truncationReasons->append(QStringLiteral("runtime.log 存在无序号记录"));
                continue;
            }
            bool ok = false;
            const quint64 sequence = match.captured(1).toULongLong(&ok);
            if (ok && sequence != 0 && sequence <= boundary && !runtimeLines.contains(sequence)) runtimeLines.insert(sequence, line);
        }
    }
    for (const DiagnosticRecorder::Event &event : fallback) {
        if (event.sequence == 0 || event.sequence > boundary) continue;
        if (!eventLines.contains(event.sequence)) eventLines.insert(event.sequence, eventJsonBytes(event));
        if (!runtimeLines.contains(event.sequence)) runtimeLines.insert(event.sequence, runtimeLineBytes(event));
    }
    for (auto it = eventLines.constBegin(); it != eventLines.constEnd(); ++it) {
        result.events += it.value();
        ++result.count;
    }
    for (auto it = runtimeLines.constBegin(); it != runtimeLines.constEnd(); ++it) result.runtime += it.value();
    return result;
}

struct SnapshotDocuments { QJsonObject network; QJsonObject settings; };

SnapshotDocuments loadSnapshotDocuments(const QString &directory, const QString &runId, quint64 boundary,
                                        const DiagnosticRecorder::ExportRequest &request, QStringList *truncationReasons)
{
    bool networkExists = false, settingsExists = false, cardsExists = false;
    QJsonArray networkSnapshots = readHistoryArray(QDir(directory).filePath(QStringLiteral("network_history.jsonl")), boundary, truncationReasons, &networkExists);
    QJsonArray settingsSnapshots = readHistoryArray(QDir(directory).filePath(QStringLiteral("settings_history.jsonl")), boundary, truncationReasons, &settingsExists);
    QJsonArray cards = readHistoryArray(QDir(directory).filePath(QStringLiteral("card_history.jsonl")), boundary, truncationReasons, &cardsExists);
    QSet<quint64> networkSeen, settingsSeen, cardSeen;
    for (const QJsonValue &value : networkSnapshots) networkSeen.insert(static_cast<quint64>(value.toObject().value(QStringLiteral("sequence")).toDouble(0)));
    for (const QJsonValue &value : settingsSnapshots) settingsSeen.insert(static_cast<quint64>(value.toObject().value(QStringLiteral("sequence")).toDouble(0)));
    for (const QJsonValue &value : cards) cardSeen.insert(static_cast<quint64>(value.toObject().value(QStringLiteral("sequence")).toDouble(0)));
    QByteArray bytes;
    if (!networkExists && readAllBytes(QDir(directory).filePath(QStringLiteral("network.json")), &bytes)) {
        const QJsonObject old = parseObject(bytes);
        appendUniqueBySequence(&networkSnapshots, old.value(QStringLiteral("snapshots")).toArray(), &networkSeen, boundary);
        appendUniqueBySequence(&cards, old.value(QStringLiteral("cards")).toArray(), &cardSeen, boundary);
    }
    if (!settingsExists && readAllBytes(QDir(directory).filePath(QStringLiteral("settings.json")), &bytes)) {
        const QJsonObject old = parseObject(bytes);
        appendUniqueBySequence(&settingsSnapshots, old.value(QStringLiteral("snapshots")).toArray(), &settingsSeen, boundary);
    }
    for (const DiagnosticRecorder::Snapshot &snapshot : request.networkSnapshots)
        appendUniqueBySequence(&networkSnapshots, QJsonArray{snapshotJson(snapshot)}, &networkSeen, boundary);
    for (const DiagnosticRecorder::Snapshot &snapshot : request.settingsSnapshots)
        appendUniqueBySequence(&settingsSnapshots, QJsonArray{snapshotJson(snapshot)}, &settingsSeen, boundary);
    for (const DiagnosticRecorder::CardSnapshot &snapshot : request.cardSnapshots)
        appendUniqueBySequence(&cards, QJsonArray{cardSnapshotJson(snapshot)}, &cardSeen, boundary);
    auto bySequence = [](const QJsonValue &a, const QJsonValue &b) {
        return a.toObject().value(QStringLiteral("sequence")).toDouble() < b.toObject().value(QStringLiteral("sequence")).toDouble();
    };
    auto sortArray = [&](QJsonArray *array) {
        QVector<QJsonValue> values;
        values.reserve(array->size());
        for (const QJsonValue &value : *array) values.append(value);
        std::sort(values.begin(), values.end(), bySequence);
        *array = QJsonArray();
        for (const QJsonValue &value : values) array->append(value);
    };
    sortArray(&networkSnapshots);
    sortArray(&settingsSnapshots);
    sortArray(&cards);
    return {makeSnapshotDocument(runId, boundary, networkSnapshots, cards), makeSnapshotDocument(runId, boundary, settingsSnapshots)};
}

qint64 directoryBytes(const QString &directory)
{
    qint64 total = 0;
    QDirIterator iterator(directory, QDir::Files, QDirIterator::Subdirectories);
    while (iterator.hasNext()) { iterator.next(); total += iterator.fileInfo().size(); }
    return total;
}

class ZipWriter final
{
public:
    void cancel() { m_file.close(); }
    explicit ZipWriter(const QString &path) : m_file(path) {}
    bool open(QString *error)
    {
        if (m_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return true;
        if (error) *error = QStringLiteral("open ZIP: %1").arg(m_file.errorString());
        return false;
    }
    bool addStored(const QString &name, const QByteArray &data, QString *error)
    {
        const QByteArray nameBytes = name.toUtf8();
        if (nameBytes.isEmpty() || nameBytes.size() > std::numeric_limits<quint16>::max() || static_cast<quint64>(data.size()) > std::numeric_limits<quint32>::max()) {
            if (error) *error = QStringLiteral("invalid ZIP entry: %1").arg(name);
            return false;
        }
        CentralEntry entry;
        entry.name = nameBytes; entry.crc = crc32(data); entry.size = static_cast<quint32>(data.size()); entry.offset = static_cast<quint32>(m_file.pos());
        QByteArray header;
        appendU32(&header, 0x04034b50U); appendU16(&header, 20); appendU16(&header, 0x0800); appendU16(&header, 0); appendU16(&header, 0); appendU16(&header, 0);
        appendU32(&header, entry.crc); appendU32(&header, entry.size); appendU32(&header, entry.size); appendU16(&header, static_cast<quint16>(nameBytes.size())); appendU16(&header, 0);
        if (!write(header, error) || !write(nameBytes, error) || !write(data, error)) return false;
        m_entries.append(entry);
        return true;
    }
    bool close(QString *error)
    {
        const quint32 centralOffset = static_cast<quint32>(m_file.pos());
        for (const CentralEntry &entry : m_entries) {
            QByteArray central;
            appendU32(&central, 0x02014b50U); appendU16(&central, 20); appendU16(&central, 20); appendU16(&central, 0x0800); appendU16(&central, 0); appendU16(&central, 0); appendU16(&central, 0);
            appendU32(&central, entry.crc); appendU32(&central, entry.size); appendU32(&central, entry.size); appendU16(&central, static_cast<quint16>(entry.name.size()));
            appendU16(&central, 0); appendU16(&central, 0); appendU16(&central, 0); appendU16(&central, 0); appendU32(&central, 0); appendU32(&central, entry.offset);
            if (!write(central, error) || !write(entry.name, error)) return false;
        }
        const quint32 centralSize = static_cast<quint32>(m_file.pos()) - centralOffset;
        QByteArray end;
        appendU32(&end, 0x06054b50U); appendU16(&end, 0); appendU16(&end, 0); appendU16(&end, static_cast<quint16>(m_entries.size())); appendU16(&end, static_cast<quint16>(m_entries.size()));
        appendU32(&end, centralSize); appendU32(&end, centralOffset); appendU16(&end, 0);
        if (!write(end, error) || !m_file.flush()) {
            if (error && error->isEmpty()) *error = QStringLiteral("flush ZIP: %1").arg(m_file.errorString());
            return false;
        }
        m_file.close();
        return true;
    }
private:
    struct CentralEntry { QByteArray name; quint32 crc = 0; quint32 size = 0; quint32 offset = 0; };
    static void appendU16(QByteArray *bytes, quint16 value) { bytes->append(static_cast<char>(value & 0xff)); bytes->append(static_cast<char>((value >> 8) & 0xff)); }
    static void appendU32(QByteArray *bytes, quint32 value) { appendU16(bytes, static_cast<quint16>(value & 0xffff)); appendU16(bytes, static_cast<quint16>((value >> 16) & 0xffff)); }
    static quint32 crc32(const QByteArray &data)
    {
        static const std::array<quint32, 256> table = [] { std::array<quint32, 256> values{}; for (quint32 i = 0; i < values.size(); ++i) { quint32 value = i; for (int bit = 0; bit < 8; ++bit) value = (value & 1U) ? (0xedb88320U ^ (value >> 1)) : (value >> 1); values[i] = value; } return values; }();
        quint32 crc = 0xffffffffU;
        for (char byte : data) crc = table[(crc ^ static_cast<unsigned char>(byte)) & 0xffU] ^ (crc >> 8);
        return crc ^ 0xffffffffU;
    }
    bool write(const QByteArray &bytes, QString *error)
    {
        if (bytes.isEmpty()) return true;
        if (m_file.write(bytes) == bytes.size()) return true;
        if (error) *error = QStringLiteral("write ZIP: %1").arg(m_file.errorString());
        return false;
    }
    QFile m_file;
    QVector<CentralEntry> m_entries;
};

QJsonObject manifestObject(const QString &runId, const QString &startIso, const QString &endIso, quint64 boundary, bool active,
                           bool writeError, const QString &writeErrorText, const DiagnosticRecorder::DropCounters &drops,
                           const QStringList &missing, const QStringList &truncationReasons, const QString &note,
                           const QStringList &names, const QHash<QString, QByteArray> &contents)
{
    QJsonObject manifest;
    manifest.insert(QStringLiteral("formatVersion"), 1); manifest.insert(QStringLiteral("runId"), runId); manifest.insert(QStringLiteral("startTime"), startIso); manifest.insert(QStringLiteral("endTime"), endIso);
    manifest.insert(QStringLiteral("active"), active); manifest.insert(QStringLiteral("boundarySequence"), static_cast<double>(boundary)); manifest.insert(QStringLiteral("writeError"), writeError);
    if (writeError) manifest.insert(QStringLiteral("writeErrorText"), writeErrorText);
    manifest.insert(QStringLiteral("note"), note);
    QJsonObject dropObject; dropObject.insert(QStringLiteral("queueDropped"), static_cast<double>(drops.queueDropped)); dropObject.insert(QStringLiteral("criticalQueueDropped"), static_cast<double>(drops.criticalQueueDropped)); dropObject.insert(QStringLiteral("noiseDropped"), static_cast<double>(drops.noiseDropped)); dropObject.insert(QStringLiteral("writeFallbackDropped"), static_cast<double>(drops.writeFallbackDropped));
    manifest.insert(QStringLiteral("dropCounters"), dropObject);
    QJsonArray missingArray; for (const QString &value : missing) missingArray.append(value); manifest.insert(QStringLiteral("missingFiles"), missingArray);
    QJsonArray truncationArray; for (const QString &value : truncationReasons) truncationArray.append(value); manifest.insert(QStringLiteral("truncationReasons"), truncationArray);
    manifest.insert(QStringLiteral("truncated"), !truncationReasons.isEmpty() || !missing.isEmpty());
    QJsonArray files;
    for (const QString &name : names) {
        QJsonObject fileObject; fileObject.insert(QStringLiteral("name"), name);
        if (name == QStringLiteral("manifest.json")) { fileObject.insert(QStringLiteral("bytes"), 0); fileObject.insert(QStringLiteral("sha256"), QJsonValue()); fileObject.insert(QStringLiteral("hashNote"), QStringLiteral("manifest自身不计算自引用哈希")); }
        else { const QByteArray bytes = contents.value(name); fileObject.insert(QStringLiteral("bytes"), static_cast<double>(bytes.size())); fileObject.insert(QStringLiteral("sha256"), QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())); }
        files.append(fileObject);
    }
    manifest.insert(QStringLiteral("files"), files); manifest.insert(QStringLiteral("complete"), missing.isEmpty() && truncationReasons.isEmpty() && !writeError);
    return manifest;
}

QJsonObject manifestForFiles(const QString &runId, const QString &startIso, const QString &endIso, quint64 boundary, bool active,
                             bool writeError, const QString &writeErrorText, const DiagnosticRecorder::DropCounters &drops,
                             QStringList missing, const QStringList &truncationReasons, const QString &note, const QString &directory)
{
    const QStringList names = {QStringLiteral("runtime.log"), QStringLiteral("events.jsonl"), QStringLiteral("network.json"), QStringLiteral("settings.json"), QStringLiteral("summary.txt"), QStringLiteral("manifest.json")};
    QHash<QString, QByteArray> contents;
    for (const QString &name : names) {
        if (name == QStringLiteral("manifest.json")) continue;
        QByteArray bytes;
        if (readAllBytes(QDir(directory).filePath(name), &bytes)) contents.insert(name, bytes);
        else if (!missing.contains(name)) missing.append(name);
    }
    return manifestObject(runId, startIso, endIso, boundary, active, writeError, writeErrorText, drops, missing, truncationReasons, note, names, contents);
}

bool runIsActive(const QString &directory)
{
    // The marker is deliberately only informational. A process crash leaves
    // it behind, while QLockFile can validate/reclaim the stale PID lock.
    QLockFile lock(QDir(directory).filePath(QStringLiteral(".active.lock")));
    if (!lock.tryLock(0)) return true;
    lock.unlock();
    return false;
}

void pruneRuns(const QString &rootDirectory, const DiagnosticRecorder::Options &options)
{
    QDir root(rootDirectory);
    if (!root.exists()) return;
    QLockFile retentionLock(root.filePath(QStringLiteral(".retention.lock")));
    if (!retentionLock.tryLock(0)) return;
    const QString rootCanonical = root.canonicalPath();
    struct Run { QString path; qint64 bytes = 0; QDateTime modified; bool active = false; };
    QVector<Run> runs;
    const QFileInfoList entries = root.entryInfoList({QStringLiteral("run-*")}, QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time);
    for (const QFileInfo &info : entries) {
        QByteArray manifestBytes;
        const QString canonical = info.canonicalFilePath();
        if (info.isSymLink() || info.isJunction() || canonical.isEmpty() || !canonical.startsWith(rootCanonical + QDir::separator())
            || !readAllBytes(QDir(info.absoluteFilePath()).filePath(QStringLiteral("manifest.json")), &manifestBytes)) continue;
        const QJsonObject manifest = parseObject(manifestBytes);
        const QString id = manifest.value(QStringLiteral("runId")).toString();
        if (manifest.value(QStringLiteral("formatVersion")).toInt(0) != 1 || id.isEmpty()
            || !info.fileName().endsWith(QLatin1Char('-') + id)) continue;
        // Recorder directories contain only files. Leave unexpected user content alone.
        bool unexpected = false;
        for (const auto &child : QDir(info.absoluteFilePath()).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot))
            if (child.isDir() || child.isSymLink() || child.isJunction()) { unexpected = true; break; }
        if (unexpected) continue;
        runs.append({info.absoluteFilePath(), directoryBytes(info.absoluteFilePath()), info.lastModified(), runIsActive(info.absoluteFilePath())});
    }
    std::sort(runs.begin(), runs.end(), [](const Run &a, const Run &b) { return a.modified < b.modified; });
    qint64 total = 0;
    int ended = 0;
    for (const Run &run : runs) { total += run.bytes; if (!run.active) ++ended; }
    for (const Run &run : runs) {
        if (run.active) continue;
        if (ended <= options.maxRetainedRuns && total <= options.maxRetainedBytes) break;
        QLockFile runLock(QDir(run.path).filePath(QStringLiteral(".active.lock")));
        if (!runLock.tryLock(0)) continue;
        bool removed = true;
        const QFileInfoList children = QDir(run.path).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot);
        for (const QFileInfo &child : children) {
            if (child.fileName() == QStringLiteral(".active.lock")) continue;
            if (child.isSymLink()) {
                if (!QFile::remove(child.absoluteFilePath())) { removed = false; break; }
            } else if (child.isDir()) {
                if (!QDir(child.absoluteFilePath()).removeRecursively()) { removed = false; break; }
            } else if (!QFile::remove(child.absoluteFilePath())) {
                removed = false;
                break;
            }
        }
        runLock.unlock();
        if (removed && QDir(run.path).removeRecursively()) { total -= run.bytes; --ended; }
    }
}

quint64 maxSequenceOnDisk(const QString &directory)
{
    quint64 maximum = 0;
    const QStringList lineFiles = {QStringLiteral("events.jsonl"), QStringLiteral("network_history.jsonl"),
                                   QStringLiteral("settings_history.jsonl"), QStringLiteral("card_history.jsonl")};
    for (const QString &name : lineFiles) {
        QFile file(QDir(directory).filePath(name));
        if (!file.open(QIODevice::ReadOnly)) continue;
        while (!file.atEnd()) {
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(file.readLine(), &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject()) continue;
            maximum = qMax(maximum, static_cast<quint64>(document.object().value(QStringLiteral("sequence")).toDouble(0)));
        }
    }
    for (const QString &name : {QStringLiteral("network.json"), QStringLiteral("settings.json")}) {
        QByteArray bytes;
        if (!readAllBytes(QDir(directory).filePath(name), &bytes)) continue;
        const QJsonObject document = parseObject(bytes);
        for (const QString &arrayName : {QStringLiteral("snapshots"), QStringLiteral("cards")}) {
            for (const QJsonValue &value : document.value(arrayName).toArray())
                maximum = qMax(maximum, static_cast<quint64>(value.toObject().value(QStringLiteral("sequence")).toDouble(0)));
        }
        maximum = qMax(maximum, static_cast<quint64>(document.value(QStringLiteral("boundarySequence")).toDouble(0)));
    }
    return maximum;
}
} // namespace

struct DiagnosticRecorder::State
{
    struct NoiseBucket { qint64 windowStart = 0; qint64 lastSeen = 0; int accepted = 0; };
    struct QueueItem {
        enum class Kind { Event, NetworkSnapshot, SettingsSnapshot, CardSnapshot };
        Kind kind = Kind::Event; bool critical = false; Event event; Snapshot snapshot; CardSnapshot card;
        quint64 sequence() const { switch (kind) { case Kind::Event: return event.sequence; case Kind::NetworkSnapshot: case Kind::SettingsSnapshot: return snapshot.sequence; case Kind::CardSnapshot: return card.sequence; } return 0; }
    };

    explicit State(const Options &input) : options(input), monoStart(Clock::now())
    {
        options.maxQueueEntries = qMax(1, options.maxQueueEntries); options.maxFallbackEntries = qMax(1, options.maxFallbackEntries); options.maxSnapshotEntries = qMax(1, options.maxSnapshotEntries);
        options.noiseWindowMs = qMax(1, options.noiseWindowMs); options.noiseBurst = qMax(0, options.noiseBurst); options.maxNoiseBuckets = qMax(1, options.maxNoiseBuckets);
        options.maxRetainedRuns = qMax(1, options.maxRetainedRuns); options.maxRetainedBytes = qMax<qint64>(1, options.maxRetainedBytes);
        rootDirectory = options.rootDirectory.trimmed(); if (rootDirectory.isEmpty()) rootDirectory = defaultRootDirectoryLocal();
        runId = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
        const QDateTime start = QDateTime::currentDateTime(); startIsoTime = start.toString(Qt::ISODateWithMs);
        runDirectory = QDir(rootDirectory).filePath(QStringLiteral("run-%1-%2").arg(start.toString(QStringLiteral("yyyyMMdd'T'HHmmsszzz")), runId));
        startRun();
    }
    ~State() { stopRun(); }

    void setWriteError(const QString &text) { std::lock_guard<std::mutex> guard(mutex); setWriteErrorLocked(text); }
    void setWriteErrorLocked(const QString &text) { if (!writeError) { writeError = true; writeErrorText = text; } }
    void ensureFile(const QString &path, const QString &label) { QFile file(path); if (!file.open(QIODevice::ReadWrite | QIODevice::Append)) setWriteError(QStringLiteral("创建 %1 失败：%2").arg(label, file.errorString())); else file.close(); }

    void startRun()
    {
        if (!QDir().mkpath(runDirectory)) setWriteError(QStringLiteral("创建运行目录失败：%1").arg(runDirectory));
        activeMarkerPath = QDir(runDirectory).filePath(QStringLiteral(".active"));
        QFile marker(activeMarkerPath);
        if (marker.open(QIODevice::WriteOnly | QIODevice::Truncate)) { marker.write(runId.toUtf8()); marker.close(); } else setWriteError(QStringLiteral("创建运行标记失败：%1").arg(marker.errorString()));
        activeLock = std::make_unique<QLockFile>(activeMarkerPath + QStringLiteral(".lock"));
        if (!activeLock->tryLock(0)) setWriteError(QStringLiteral("获取运行锁失败：%1").arg(activeLock->error()));
        runtimeFile.setFileName(QDir(runDirectory).filePath(QStringLiteral("runtime.log"))); eventsFile.setFileName(QDir(runDirectory).filePath(QStringLiteral("events.jsonl"))); networkHistoryFile.setFileName(QDir(runDirectory).filePath(QStringLiteral("network_history.jsonl"))); settingsHistoryFile.setFileName(QDir(runDirectory).filePath(QStringLiteral("settings_history.jsonl"))); cardHistoryFile.setFileName(QDir(runDirectory).filePath(QStringLiteral("card_history.jsonl")));
        ensureFile(runtimeFile.fileName(), QStringLiteral("runtime.log")); ensureFile(eventsFile.fileName(), QStringLiteral("events.jsonl")); ensureFile(networkHistoryFile.fileName(), QStringLiteral("network_history.jsonl")); ensureFile(settingsHistoryFile.fileName(), QStringLiteral("settings_history.jsonl")); ensureFile(cardHistoryFile.fileName(), QStringLiteral("card_history.jsonl"));
        { std::lock_guard<std::mutex> guard(mutex); active = true; accepting = true; }
        writeInitialArtifacts(); worker = std::thread([this] { workerLoop(); }); pruneRuns(rootDirectory, options);
    }

    void stopRun()
    {
        { std::lock_guard<std::mutex> guard(mutex); if (!accepting && !worker.joinable()) return; accepting = false; stopping = true; flushRequested = true; flushTarget = nextSequence ? nextSequence - 1 : 0; }
        condition.notify_all(); if (worker.joinable()) worker.join();
        quint64 boundary = 0; { std::lock_guard<std::mutex> guard(mutex); active = false; endIsoTime = nowIso(); boundary = nextSequence ? nextSequence - 1 : 0; }
        writeRunArtifacts(boundary, false, QString());
        if (!activeMarkerPath.isEmpty()) QFile::remove(activeMarkerPath);
        if (activeLock) activeLock->unlock();
        pruneRuns(rootDirectory, options);
    }

    qint64 monotonicMs() const { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - monoStart).count(); }
    void completeLocked(quint64 sequence)
    {
        if (!sequence || sequence <= processedSequence) return;
        completed.insert(sequence);
        while (completed.remove(processedSequence + 1)) ++processedSequence;
    }
    void appendFallbackLocked(const Event &event)
    {
        if (fallbackEvents.size() < options.maxFallbackEntries) { fallbackEvents.append(event); return; }
        auto replace = std::find_if(fallbackEvents.begin(), fallbackEvents.end(), [](const Event &oldEvent) { return !isCritical(oldEvent); });
        if (replace == fallbackEvents.end()) replace = fallbackEvents.begin();
        fallbackEvents.erase(replace); fallbackEvents.append(event); ++drops.writeFallbackDropped;
    }
    bool enqueueLocked(QueueItem item)
    {
        if (static_cast<int>(queue.size()) < options.maxQueueEntries) { queue.push_back(std::move(item)); return true; }
        if (!item.critical) { ++drops.queueDropped; completeLocked(item.sequence()); return false; }
        auto replace = std::find_if(queue.begin(), queue.end(), [](const QueueItem &queued) { return !queued.critical; });
        QueueItem removed = replace == queue.end() ? std::move(queue.front()) : std::move(*replace);
        if (replace == queue.end()) { queue.pop_front(); ++drops.criticalQueueDropped; } else { queue.erase(replace); ++drops.queueDropped; }
        if (removed.kind == QueueItem::Kind::Event) appendFallbackLocked(removed.event); completeLocked(removed.sequence()); queue.push_back(std::move(item)); return true;
    }
    void pruneNoiseLocked(qint64 current)
    {
        for (auto it = noiseBuckets.begin(); it != noiseBuckets.end();) { if (current - it.value().lastSeen > options.noiseWindowMs * 2LL) it = noiseBuckets.erase(it); else ++it; }
        while (noiseBuckets.size() > options.maxNoiseBuckets) { auto oldest = noiseBuckets.begin(); for (auto it = noiseBuckets.begin(); it != noiseBuckets.end(); ++it) if (it.value().lastSeen < oldest.value().lastSeen) oldest = it; noiseBuckets.erase(oldest); }
    }
    quint64 recordEvent(const QString &category, const QString &message, Severity severity, const QJsonObject &fields)
    {
        Event event; bool notify = false;
        { std::lock_guard<std::mutex> guard(mutex); if (!accepting) return 0; event.sequence = nextSequence++; event.runId = runId; event.isoTime = nowIso(); event.monotonicMs = monotonicMs(); event.category = category; event.message = message; event.severity = severity; event.fields = fields;
            if ((event.sequence & 0x3fU) == 0) pruneNoiseLocked(event.monotonicMs);
            if (severity <= Severity::Info && options.noiseBurst > 0) { const QString key = category + QChar('\n') + message; NoiseBucket &bucket = noiseBuckets[key]; if (!bucket.windowStart || event.monotonicMs - bucket.windowStart >= options.noiseWindowMs) { bucket.windowStart = event.monotonicMs; bucket.accepted = 0; } bucket.lastSeen = event.monotonicMs; if (bucket.accepted >= options.noiseBurst) { ++drops.noiseDropped; completeLocked(event.sequence); return event.sequence; } ++bucket.accepted; }
            if (writeError) { appendFallbackLocked(event); completeLocked(event.sequence); return event.sequence; }
            QueueItem item; item.kind = QueueItem::Kind::Event; item.critical = isCritical(event); item.event = event; notify = enqueueLocked(std::move(item)); }
        if (notify) condition.notify_one(); return event.sequence;
    }
    template <typename T> void appendSnapshot(QVector<T> &target, const T &value) { target.append(value); while (target.size() > options.maxSnapshotEntries) target.removeFirst(); }
    quint64 recordNetworkSnapshot(const QJsonObject &data)
    {
        Snapshot snapshot; bool notify = false; { std::lock_guard<std::mutex> guard(mutex); if (!accepting) return 0; snapshot.sequence = nextSequence++; snapshot.runId = runId; snapshot.isoTime = nowIso(); snapshot.monotonicMs = monotonicMs(); snapshot.data = data; appendSnapshot(networkSnapshots, snapshot); if (writeError) { completeLocked(snapshot.sequence); return snapshot.sequence; } QueueItem item; item.kind = QueueItem::Kind::NetworkSnapshot; item.critical = true; item.snapshot = snapshot; notify = enqueueLocked(std::move(item)); } if (notify) condition.notify_one(); return snapshot.sequence;
    }
    quint64 recordSettingsSnapshot(const QJsonObject &data)
    {
        Snapshot snapshot; bool notify = false; { std::lock_guard<std::mutex> guard(mutex); if (!accepting) return 0; snapshot.sequence = nextSequence++; snapshot.runId = runId; snapshot.isoTime = nowIso(); snapshot.monotonicMs = monotonicMs(); snapshot.data = data; appendSnapshot(settingsSnapshots, snapshot); if (writeError) { completeLocked(snapshot.sequence); return snapshot.sequence; } QueueItem item; item.kind = QueueItem::Kind::SettingsSnapshot; item.critical = true; item.snapshot = snapshot; notify = enqueueLocked(std::move(item)); } if (notify) condition.notify_one(); return snapshot.sequence;
    }
    quint64 setCardSnapshot(int card, const QString &ip, const QString &state, const QJsonObject &fields)
    {
        CardSnapshot snapshot; bool notify = false; { std::lock_guard<std::mutex> guard(mutex); if (!accepting) return 0; snapshot.sequence = nextSequence++; snapshot.runId = runId; snapshot.isoTime = nowIso(); snapshot.monotonicMs = monotonicMs(); snapshot.card = card; snapshot.ip = ip; snapshot.state = state; snapshot.fields = fields; appendSnapshot(cardSnapshots, snapshot); if (writeError) { completeLocked(snapshot.sequence); return snapshot.sequence; } QueueItem item; item.kind = QueueItem::Kind::CardSnapshot; item.critical = true; item.card = snapshot; notify = enqueueLocked(std::move(item)); } if (notify) condition.notify_one(); return snapshot.sequence;
    }
    qint64 diskBytes() const
    {
        qint64 total = 0; for (const QFile *file : {&runtimeFile, &eventsFile, &networkHistoryFile, &settingsHistoryFile, &cardHistoryFile}) { const QFileInfo info(file->fileName()); if (info.exists()) total += info.size(); } return total;
    }
    qint64 itemBytes(const QueueItem &item) const
    {
        switch (item.kind) { case QueueItem::Kind::Event: return eventJsonBytes(item.event).size() + runtimeLineBytes(item.event).size(); case QueueItem::Kind::NetworkSnapshot: case QueueItem::Kind::SettingsSnapshot: return snapshotJson(item.snapshot).size() + 1; case QueueItem::Kind::CardSnapshot: return cardSnapshotJson(item.card).size() + 1; } return 0;
    }
    bool writeEvent(const Event &event, QString *error)
    {
        const QByteArray json = eventJsonBytes(event); const QByteArray runtime = runtimeLineBytes(event);
        if (!eventsFile.isOpen() || eventsFile.write(json) != json.size()) { if (error) *error = QStringLiteral("写入 events.jsonl 失败：%1").arg(eventsFile.errorString()); return false; }
        if (!runtimeFile.isOpen() || runtimeFile.write(runtime) != runtime.size()) { if (error) *error = QStringLiteral("写入 runtime.log 失败：%1").arg(runtimeFile.errorString()); return false; } return true;
    }
    bool writeSnapshotHistory(QFile &file, const QJsonObject &object, const QString &label, QString *error)
    {
        const QByteArray bytes = jsonBytes(object) + '\n'; if (!file.isOpen() || file.write(bytes) != bytes.size()) { if (error) *error = QStringLiteral("写入 %1 失败：%2").arg(label, file.errorString()); return false; } return true;
    }
    bool writeItem(const QueueItem &item, QString *error)
    {
        { std::lock_guard<std::mutex> guard(mutex); if (writeError) { if (error) *error = writeErrorText; return false; } }
        if (diskBytes() + itemBytes(item) > options.maxRetainedBytes) { if (error) *error = QStringLiteral("单次运行日志达到容量上限，后续记录转入内存回退"); return false; }
        switch (item.kind) { case QueueItem::Kind::Event: return writeEvent(item.event, error); case QueueItem::Kind::NetworkSnapshot: return writeSnapshotHistory(networkHistoryFile, snapshotJson(item.snapshot), QStringLiteral("network_history.jsonl"), error); case QueueItem::Kind::SettingsSnapshot: return writeSnapshotHistory(settingsHistoryFile, snapshotJson(item.snapshot), QStringLiteral("settings_history.jsonl"), error); case QueueItem::Kind::CardSnapshot: return writeSnapshotHistory(cardHistoryFile, cardSnapshotJson(item.card), QStringLiteral("card_history.jsonl"), error); } if (error) *error = QStringLiteral("未知诊断队列项"); return false;
    }
    bool flushFiles(QString *error)
    {
        for (QFile *file : {&runtimeFile, &eventsFile, &networkHistoryFile, &settingsHistoryFile, &cardHistoryFile}) if (file->isOpen() && !file->flush()) { if (error) *error = QStringLiteral("刷新诊断日志失败：%1").arg(file->errorString()); return false; } return true;
    }
    void openAppend(QFile &file, const QString &label) { if (!file.open(QIODevice::ReadWrite | QIODevice::Append)) setWriteError(QStringLiteral("打开 %1 失败：%2").arg(label, file.errorString())); }
    void workerLoop()
    {
        openAppend(runtimeFile, QStringLiteral("runtime.log")); openAppend(eventsFile, QStringLiteral("events.jsonl")); openAppend(networkHistoryFile, QStringLiteral("network_history.jsonl")); openAppend(settingsHistoryFile, QStringLiteral("settings_history.jsonl")); openAppend(cardHistoryFile, QStringLiteral("card_history.jsonl"));
        Clock::time_point lastFlush = Clock::now();
        for (;;) {
            QueueItem item; bool haveItem = false, flushNow = false, finish = false;
            { std::unique_lock<std::mutex> lock(mutex); if (queue.empty() && !stopping && !flushRequested) condition.wait_for(lock, std::chrono::milliseconds(1000)); if (!queue.empty()) { item = queue.front(); queue.pop_front(); inFlight = item; hasInFlight = true; haveItem = true; } else { const bool targetReached = flushRequested && processedSequence >= flushTarget; const bool timedFlush = Clock::now() - lastFlush >= std::chrono::seconds(1); if (stopping || targetReached || timedFlush) { flushNow = true; finish = stopping; if (targetReached || stopping) flushRequested = false; } } }
            if (haveItem) { QString error; const bool ok = writeItem(item, &error); { std::lock_guard<std::mutex> guard(mutex); if (!ok) { setWriteErrorLocked(error.isEmpty() ? QStringLiteral("诊断日志写入失败") : error); if (item.kind == QueueItem::Kind::Event) appendFallbackLocked(item.event); } else writtenSequence = qMax(writtenSequence, item.sequence()); completeLocked(item.sequence()); hasInFlight = false; if (flushRequested && processedSequence >= flushTarget) { flushNow = true; flushRequested = false; } if (stopping && queue.empty()) { flushNow = true; finish = true; flushRequested = false; } } progress.notify_all(); if (Clock::now() - lastFlush >= std::chrono::seconds(1)) flushNow = true; }
            if (flushNow) { QString error; const bool ok = flushFiles(&error); { std::lock_guard<std::mutex> guard(mutex); if (!ok) setWriteErrorLocked(error); flushedSequence = processedSequence; } lastFlush = Clock::now(); progress.notify_all(); if (finish) break; }
        }
        runtimeFile.close(); eventsFile.close(); networkHistoryFile.close(); settingsHistoryFile.close(); cardHistoryFile.close(); progress.notify_all();
    }
    bool flushTo(quint64 target, int timeoutMs)
    {
        { std::lock_guard<std::mutex> guard(mutex); if (!worker.joinable()) return flushedSequence >= target && !writeError; flushRequested = true; flushTarget = qMax(flushTarget, target); } condition.notify_all(); std::unique_lock<std::mutex> lock(mutex); const auto ready = [this, target] { return flushedSequence >= target || !worker.joinable(); }; bool reached = timeoutMs < 0 ? (progress.wait(lock, ready), true) : progress.wait_for(lock, std::chrono::milliseconds(timeoutMs), ready); return reached && flushedSequence >= target && !writeError;
    }
    void requestFlush() { { std::lock_guard<std::mutex> guard(mutex); if (!worker.joinable()) return; flushRequested = true; flushTarget = qMax(flushTarget, nextSequence ? nextSequence - 1 : 0); } condition.notify_all(); }
    void writeInitialArtifacts() { writeRunArtifacts(0, true, QString()); }
    void loadLiveSnapshotDocuments(quint64 boundary, QJsonObject *network, QJsonObject *settings, QStringList *truncationReasons) const
    {
        ExportRequest request; { std::lock_guard<std::mutex> guard(mutex); request.runId = runId; request.sourceDirectory = runDirectory; request.boundarySequence = boundary; request.networkSnapshots = networkSnapshots; request.settingsSnapshots = settingsSnapshots; request.cardSnapshots = cardSnapshots; }
        const SnapshotDocuments documents = loadSnapshotDocuments(runDirectory, runId, boundary, request, truncationReasons); if (network) *network = documents.network; if (settings) *settings = documents.settings;
    }
    void writeRunArtifacts(quint64 boundary, bool activeState, const QString &note)
    {
        QStringList missing, truncationReasons; QJsonObject network, settings; loadLiveSnapshotDocuments(boundary, &network, &settings, &truncationReasons); QString error;
        if (!writeAtomicBytes(QDir(runDirectory).filePath(QStringLiteral("network.json")), jsonBytes(network) + '\n', &error)) { missing.append(QStringLiteral("network.json")); setWriteError(error); }
        if (!writeAtomicBytes(QDir(runDirectory).filePath(QStringLiteral("settings.json")), jsonBytes(settings) + '\n', &error)) { missing.append(QStringLiteral("settings.json")); setWriteError(error); }
        QVector<Event> fallback; DropCounters dropCopy; bool writeErrorCopy = false; QString writeErrorTextCopy, startCopy, endCopy;
        { std::lock_guard<std::mutex> guard(mutex); fallback = fallbackEvents; dropCopy = drops; writeErrorCopy = writeError; writeErrorTextCopy = writeErrorText; startCopy = startIsoTime; endCopy = endIsoTime; }
        appendDropReasons(dropCopy, &truncationReasons); const FilteredEvents events = readFilteredEvents(runDirectory, boundary, fallback, &missing, &truncationReasons); const bool complete = missing.isEmpty() && truncationReasons.isEmpty() && !writeErrorCopy;
        if (!writeAtomicBytes(QDir(runDirectory).filePath(QStringLiteral("summary.txt")), summaryBytes(runId, startCopy, endCopy, boundary, activeState, complete, writeErrorCopy, writeErrorTextCopy, dropCopy, network, settings, events.count, note, truncationReasons), &error)) { missing.append(QStringLiteral("summary.txt")); setWriteError(error); }
        { std::lock_guard<std::mutex> guard(mutex); writeErrorCopy = writeError; writeErrorTextCopy = writeErrorText; dropCopy = drops; }
        const QJsonObject manifest = manifestForFiles(runId, startCopy, endCopy, boundary, activeState, writeErrorCopy, writeErrorTextCopy, dropCopy, missing, truncationReasons, note, runDirectory);
        if (!writeAtomicBytes(QDir(runDirectory).filePath(QStringLiteral("manifest.json")), jsonBytes(manifest) + '\n', &error)) setWriteError(error);
    }
    void appendRequestStateLocked(ExportRequest *request) const
    {
        if (!request) return;
        auto addEvent = [&](const Event &event) { if (event.sequence <= request->boundarySequence && std::none_of(request->fallbackEvents.cbegin(), request->fallbackEvents.cend(), [&](const Event &old) { return old.sequence == event.sequence; })) request->fallbackEvents.append(event); };
        for (const Event &event : fallbackEvents) addEvent(event);
        auto addItem = [&](const QueueItem &item) { if (!item.sequence() || item.sequence() > request->boundarySequence) return; switch (item.kind) { case QueueItem::Kind::Event: addEvent(item.event); break; case QueueItem::Kind::NetworkSnapshot: request->networkSnapshots.append(item.snapshot); break; case QueueItem::Kind::SettingsSnapshot: request->settingsSnapshots.append(item.snapshot); break; case QueueItem::Kind::CardSnapshot: request->cardSnapshots.append(item.card); break; } };
        for (const QueueItem &item : queue) addItem(item); if (hasInFlight) addItem(inFlight);
        request->drops = drops;
        request->writeError = writeError;
        request->writeErrorText = writeErrorText;
        // A bounded memory cache is not a loss when its append-only history
        // file exists. Mark truncation only when the history is unavailable.
        const bool networkHistoryMissing = !QFileInfo::exists(QDir(runDirectory).filePath(QStringLiteral("network_history.jsonl")));
        const bool settingsHistoryMissing = !QFileInfo::exists(QDir(runDirectory).filePath(QStringLiteral("settings_history.jsonl")));
        const bool cardHistoryMissing = !QFileInfo::exists(QDir(runDirectory).filePath(QStringLiteral("card_history.jsonl")));
        request->snapshotsTruncated = (networkHistoryMissing && networkSnapshots.size() >= options.maxSnapshotEntries)
            || (settingsHistoryMissing && settingsSnapshots.size() >= options.maxSnapshotEntries)
            || (cardHistoryMissing && cardSnapshots.size() >= options.maxSnapshotEntries);
    }
    ExportRequest captureRequest(quint64 requestedBoundary, const QString &note) const
    {
        ExportRequest request; std::lock_guard<std::mutex> guard(mutex); request.runId = runId; request.sourceDirectory = runDirectory; request.startIsoTime = startIsoTime; request.endIsoTime = endIsoTime; const quint64 current = nextSequence ? nextSequence - 1 : 0; request.boundarySequence = requestedBoundary ? qMin(requestedBoundary, current) : current; request.sourceWasActive = active; request.note = note; request.networkSnapshots = networkSnapshots; request.settingsSnapshots = settingsSnapshots; request.cardSnapshots = cardSnapshots; appendRequestStateLocked(&request); return request;
    }
    Status statusCopy() const { std::lock_guard<std::mutex> guard(mutex); Status result; result.active = active; result.writeError = writeError; result.writeErrorText = writeErrorText; result.nextSequence = nextSequence; result.writtenSequence = writtenSequence; result.drops = drops; result.queuedEntries = static_cast<int>(queue.size()); result.fallbackEntries = fallbackEvents.size(); return result; }

    Options options; QString rootDirectory, runId, runDirectory, activeMarkerPath, startIsoTime, endIsoTime; Clock::time_point monoStart; std::unique_ptr<QLockFile> activeLock;
    mutable std::mutex mutex; std::condition_variable condition, progress; std::deque<QueueItem> queue; QVector<Event> fallbackEvents; QVector<Snapshot> networkSnapshots, settingsSnapshots; QVector<CardSnapshot> cardSnapshots; QHash<QString, NoiseBucket> noiseBuckets; QSet<quint64> completed; DropCounters drops; QueueItem inFlight; bool hasInFlight = false;
    QFile runtimeFile, eventsFile, networkHistoryFile, settingsHistoryFile, cardHistoryFile; std::thread worker; bool active = false, accepting = false, stopping = false, flushRequested = false, writeError = false; QString writeErrorText; quint64 nextSequence = 1, writtenSequence = 0, processedSequence = 0, flushedSequence = 0, flushTarget = 0;
};

DiagnosticRecorder *DiagnosticRecorder::instance() { std::lock_guard<std::mutex> guard(g_instanceMutex); return g_instance; }
DiagnosticRecorder *DiagnosticRecorder::initialize(const Options &options, QString *error) { std::lock_guard<std::mutex> guard(g_instanceMutex); if (g_instance) return g_instance; DiagnosticRecorder *recorder = new DiagnosticRecorder(options); if (error && recorder->status().writeError) *error = recorder->status().writeErrorText; g_instance = recorder; return recorder; }
DiagnosticRecorder *DiagnosticRecorder::initialize(QString *error) { return initialize(Options(), error); }
void DiagnosticRecorder::shutdown() { DiagnosticRecorder *recorder = nullptr; { std::lock_guard<std::mutex> guard(g_instanceMutex); recorder = g_instance; g_instance = nullptr; } delete recorder; }
DiagnosticRecorder::DiagnosticRecorder() : DiagnosticRecorder(Options()) {}
DiagnosticRecorder::DiagnosticRecorder(const Options &options) : m_state(std::make_shared<State>(options)) {}
DiagnosticRecorder::~DiagnosticRecorder() = default;
bool DiagnosticRecorder::isActive() const { return m_state && m_state->statusCopy().active; }
QString DiagnosticRecorder::runId() const { return m_state ? m_state->runId : QString(); }
QString DiagnosticRecorder::runDirectory() const { return m_state ? m_state->runDirectory : QString(); }
DiagnosticRecorder::Status DiagnosticRecorder::status() const { return m_state ? m_state->statusCopy() : Status(); }
quint64 DiagnosticRecorder::recordEvent(const QString &category, const QString &message, Severity severity, const QJsonObject &fields) { return m_state ? m_state->recordEvent(category, message, severity, fields) : 0; }
quint64 DiagnosticRecorder::logText(const QString &message, Severity severity, const QJsonObject &fields) { return recordEvent(QStringLiteral("runtime"), message, severity, fields); }
quint64 DiagnosticRecorder::recordNetworkSnapshot(const QJsonObject &snapshot) { return m_state ? m_state->recordNetworkSnapshot(snapshot) : 0; }
quint64 DiagnosticRecorder::recordSettingsSnapshot(const QJsonObject &snapshot) { return m_state ? m_state->recordSettingsSnapshot(snapshot) : 0; }
quint64 DiagnosticRecorder::setCardSnapshot(int card, const QString &ip, const QString &state, const QJsonObject &fields) { return m_state ? m_state->setCardSnapshot(card, ip, state, fields) : 0; }
void DiagnosticRecorder::requestFlush() { if (m_state) m_state->requestFlush(); }
bool DiagnosticRecorder::flush(int timeoutMs) { if (!m_state) return false; const quint64 boundary = m_state->captureRequest(0, QString()).boundarySequence; return m_state->flushTo(boundary, timeoutMs); }
quint64 DiagnosticRecorder::captureBoundary() const { if (!m_state) return 0; std::lock_guard<std::mutex> lock(m_state->mutex); return m_state->nextSequence - 1; }
DiagnosticRecorder::ExportRequest DiagnosticRecorder::captureExportRequest(quint64 boundarySequence, const QString &note) const
{
    if (!m_state) return ExportRequest();
    ExportRequest request = m_state->captureRequest(boundarySequence, note);
    request.capturedLive = true;
    request.flushBeforeExport = [state = m_state, boundary = request.boundarySequence] {
        return state->flushTo(boundary, 5000);
    };
    return request;
}
DiagnosticRecorder::ExportResult DiagnosticRecorder::exportRun(const QString &targetZipPath, quint64 boundarySequence, const QString &note) { return exportRequest(captureExportRequest(boundarySequence, note), targetZipPath); }
std::future<DiagnosticRecorder::ExportResult> DiagnosticRecorder::exportRunAsync(const QString &targetZipPath, quint64 boundarySequence, const QString &note) { const ExportRequest request = captureExportRequest(boundarySequence, note); return std::async(std::launch::async, [request, targetZipPath] { return DiagnosticRecorder::exportRequest(request, targetZipPath); }); }

DiagnosticRecorder::ExportResult DiagnosticRecorder::exportRequest(const ExportRequest &request, const QString &targetZipPath)
{
    ExportRequest effective = request;
    QStringList sourceWarnings;
    if (effective.flushBeforeExport && !effective.flushBeforeExport())
        sourceWarnings.append(QStringLiteral("导出前刷新未完成或发生写入错误，已合并点击时的内存记录"));
    // Historical callers normally provide only RunInfo. Merge the persisted
    // manifest so drops, write failures, and the original run identity are
    // not lost when exporting after the recorder object has gone away.
    QByteArray manifestBytes;
    if (!request.capturedLive && !request.sourceDirectory.isEmpty()
        && readAllBytes(QDir(request.sourceDirectory).filePath(QStringLiteral("manifest.json")), &manifestBytes)) {
        const QJsonObject sourceManifest = parseObject(manifestBytes);
        if (effective.runId.isEmpty()) effective.runId = sourceManifest.value(QStringLiteral("runId")).toString();
        if (effective.startIsoTime.isEmpty()) effective.startIsoTime = sourceManifest.value(QStringLiteral("startTime")).toString();
        if (effective.endIsoTime.isEmpty()) effective.endIsoTime = sourceManifest.value(QStringLiteral("endTime")).toString();
        if (effective.boundarySequence == 0)
            effective.boundarySequence = static_cast<quint64>(sourceManifest.value(QStringLiteral("boundarySequence")).toDouble(0));
        effective.sourceWasActive = runIsActive(effective.sourceDirectory);
        if (sourceManifest.value(QStringLiteral("active")).toBool(false) && !effective.sourceWasActive)
            sourceWarnings.append(QStringLiteral("该运行未正常结束，仅恢复已落盘记录，末尾日志可能缺失"));
        effective.writeError = effective.writeError || sourceManifest.value(QStringLiteral("writeError")).toBool(false);
        if (effective.writeErrorText.isEmpty()) effective.writeErrorText = sourceManifest.value(QStringLiteral("writeErrorText")).toString();
        const QJsonObject sourceDrops = sourceManifest.value(QStringLiteral("dropCounters")).toObject();
        effective.drops.queueDropped = qMax(effective.drops.queueDropped, static_cast<quint64>(sourceDrops.value(QStringLiteral("queueDropped")).toDouble(0)));
        effective.drops.criticalQueueDropped = qMax(effective.drops.criticalQueueDropped, static_cast<quint64>(sourceDrops.value(QStringLiteral("criticalQueueDropped")).toDouble(0)));
        effective.drops.noiseDropped = qMax(effective.drops.noiseDropped, static_cast<quint64>(sourceDrops.value(QStringLiteral("noiseDropped")).toDouble(0)));
        effective.drops.writeFallbackDropped = qMax(effective.drops.writeFallbackDropped, static_cast<quint64>(sourceDrops.value(QStringLiteral("writeFallbackDropped")).toDouble(0)));
    }
    if (!effective.capturedLive && effective.boundarySequence == 0) effective.boundarySequence = maxSequenceOnDisk(effective.sourceDirectory);
    ExportResult result; result.targetPath = targetZipPath; result.boundarySequence = effective.boundarySequence;
    if (targetZipPath.isEmpty()) { result.error = QStringLiteral("target ZIP path is empty"); return result; }
    if (effective.sourceDirectory.isEmpty()) { result.error = QStringLiteral("source diagnostic directory is empty"); return result; }
    if (QFileInfo::exists(targetZipPath)) { result.error = QStringLiteral("目标文件已存在，请选择新的文件名"); return result; }
    QStringList missing, truncationReasons = sourceWarnings; if (effective.snapshotsTruncated) truncationReasons.append(QStringLiteral("内存快照已达到上限；若磁盘历史缺失，早期快照可能无法恢复")); appendDropReasons(effective.drops, &truncationReasons);
    const FilteredEvents events = readFilteredEvents(effective.sourceDirectory, effective.boundarySequence, effective.fallbackEvents, &missing, &truncationReasons);
    const SnapshotDocuments snapshots = loadSnapshotDocuments(effective.sourceDirectory, effective.runId, effective.boundarySequence, effective, &truncationReasons);
    const bool complete = missing.isEmpty() && truncationReasons.isEmpty() && !effective.writeError;
    const QByteArray summary = summaryBytes(effective.runId, effective.startIsoTime, effective.endIsoTime, effective.boundarySequence, effective.sourceWasActive, complete, effective.writeError, effective.writeErrorText, effective.drops, snapshots.network, snapshots.settings, events.count, effective.note, truncationReasons);
    const QStringList names = {QStringLiteral("runtime.log"), QStringLiteral("events.jsonl"), QStringLiteral("network.json"), QStringLiteral("settings.json"), QStringLiteral("summary.txt"), QStringLiteral("manifest.json")};
    QHash<QString, QByteArray> contents; contents.insert(QStringLiteral("runtime.log"), events.runtime); contents.insert(QStringLiteral("events.jsonl"), events.events); contents.insert(QStringLiteral("network.json"), jsonBytes(snapshots.network) + '\n'); contents.insert(QStringLiteral("settings.json"), jsonBytes(snapshots.settings) + '\n'); contents.insert(QStringLiteral("summary.txt"), summary);
    const QJsonObject manifest = manifestObject(effective.runId, effective.startIsoTime, effective.endIsoTime, effective.boundarySequence, effective.sourceWasActive, effective.writeError, effective.writeErrorText, effective.drops, missing, truncationReasons, effective.note, names, contents); contents.insert(QStringLiteral("manifest.json"), jsonBytes(manifest) + '\n');
    const QFileInfo targetInfo(targetZipPath); if (!QDir().mkpath(targetInfo.absolutePath())) { result.error = QStringLiteral("create target directory failed: %1").arg(targetInfo.absolutePath()); result.missingFiles = missing; result.truncationReasons = truncationReasons; return result; }
    const QString temporaryPath = targetZipPath + QStringLiteral(".part-") + QUuid::createUuid().toString(QUuid::WithoutBraces); QString error; ZipWriter writer(temporaryPath); bool ok = writer.open(&error); if (ok) for (const QString &name : names) if (!writer.addStored(name, contents.value(name), &error)) { ok = false; break; } if (ok) ok = writer.close(&error);
    if (!ok) { writer.cancel(); QFile::remove(temporaryPath); result.error = error; result.missingFiles = missing; result.truncationReasons = truncationReasons; return result; }
    if (!QFile::rename(temporaryPath, targetZipPath)) { QFile::remove(temporaryPath); result.error = QStringLiteral("publish target ZIP failed"); return result; }
    result.success = true; result.missingFiles = missing; result.truncationReasons = truncationReasons; return result;
}

QVector<DiagnosticRecorder::RunInfo> DiagnosticRecorder::enumerateRuns(const QString &rootDirectory)
{
    const QString rootPath = rootDirectory.trimmed().isEmpty() ? defaultRootDirectoryLocal() : rootDirectory; QVector<RunInfo> result; QDir root(rootPath);
    const QFileInfoList entries = root.entryInfoList({QStringLiteral("run-*")}, QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time);
    for (const QFileInfo &info : entries) {
        RunInfo run; run.directory = info.absoluteFilePath(); run.active = runIsActive(run.directory); run.bytes = directoryBytes(run.directory); QByteArray bytes;
        if (readAllBytes(QDir(run.directory).filePath(QStringLiteral("manifest.json")), &bytes)) {
            const QJsonObject object = parseObject(bytes);
            run.runId = object.value(QStringLiteral("runId")).toString();
            run.startIsoTime = object.value(QStringLiteral("startTime")).toString();
            run.endIsoTime = object.value(QStringLiteral("endTime")).toString();
            run.lastSequence = static_cast<quint64>(object.value(QStringLiteral("boundarySequence")).toDouble(0));
            run.manifestComplete = object.value(QStringLiteral("complete")).toBool(false)
                && !run.active && !object.value(QStringLiteral("active")).toBool(false);
        }
        // An abnormal exit can leave the initial manifest at boundary zero.
        // Recover the best safe cutoff from append-only files so a historical
        // export remains useful instead of silently exporting an empty run.
        run.lastSequence = qMax(run.lastSequence, maxSequenceOnDisk(run.directory));
        if (run.runId.isEmpty()) run.runId = info.fileName(); result.append(run);
    }
    return result;
}

QString DiagnosticRecorder::defaultRootDirectory() { return defaultRootDirectoryLocal(); }
QString DiagnosticRecorder::severityName(Severity severity) { return severityNameLocal(severity); }
