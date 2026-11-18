#include <cstdio>
#include "DiagnosticRecorder.h"
#include "DiagnosticStorage.h"

#include <QBuffer>
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
#include <zlib.h>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

// Shared ownership keeps captured bytes and their admission reservation alive for retries.
struct DiagnosticRecorder::HistorySnapshot {
    QHash<QString, QByteArray> files;
    qint64 reserved = 0;
    ~HistorySnapshot();
};

struct DiagnosticRecorder::RetentionPin {
    std::mutex mutex;
    std::shared_ptr<void> lease;
    std::shared_ptr<void> take() {
        std::lock_guard<std::mutex> lock(mutex);
        return std::move(lease);
    }
};

namespace {
std::mutex g_snapshotBudgetMutex;
qint64 g_snapshotBytes = 0;
constexpr qint64 kSnapshotBudget = 80LL * 1024 * 1024;
constexpr qint64 kSnapshotMetadataLimit = 8LL * 1024 * 1024;
constexpr qint64 kSnapshotPerRunLimit = 32LL * 1024 * 1024;
using Clock = std::chrono::steady_clock;
std::mutex g_instanceMutex;
DiagnosticRecorder *g_instance = nullptr;
#ifdef PAIMAGE_DIAGNOSTIC_TEST_SEAM
std::mutex g_snapshotHookMutex;
std::function<void()> g_snapshotReadHook;
#endif
std::mutex g_bundleMutex;
DiagnosticRecorder::BundleCapture g_bundleCapture;
DiagnosticRecorder::BundleExtension captureBundle(qint64 start,qint64 end){
    std::lock_guard<std::mutex> lock(g_bundleMutex);
    return g_bundleCapture?g_bundleCapture(start,end):DiagnosticRecorder::BundleExtension{};
}

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
    const QFileInfo info(path);
    if (info.isSymLink() || info.isJunction() || !info.isFile()) {
        if (error) *error = QStringLiteral("不安全的日志文件：%1").arg(path);
        return false;
    }
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

bool safeDirectory(const QString &path)
{
    const QFileInfo info(path);
    return !path.isEmpty() && !info.isSymLink() && !info.isJunction()
        && (!info.exists() || info.isDir());
}

bool safeLockFile(const QString &directory, const QString &name)
{
    const QFileInfo info(QDir(directory).filePath(name));
    return !info.isSymLink() && !info.isJunction() && (!info.exists() || info.isFile());
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

    lines << QString() << QStringLiteral("网络入口观测汇总（完整历史见 network.json）：");
    QJsonObject latestIngress;
    const QJsonArray networkSnapshots = network.value(QStringLiteral("snapshots")).toArray();
    for (const QJsonValue &value : networkSnapshots) {
        const QJsonObject wrapper = value.toObject();
        const QJsonObject data = wrapper.value(QStringLiteral("data")).toObject();
        if (data.value(QStringLiteral("kind")).toString() == QStringLiteral("runtime_ingress"))
            latestIngress = data;
    }
    if (latestIngress.isEmpty()) {
        lines << QStringLiteral("暂无 runtime_ingress 快照。");
    } else {
        const QJsonArray interfaces = latestIngress.value(QStringLiteral("relevantInterfaces")).toArray();
        if (interfaces.isEmpty()) {
            lines << QStringLiteral("采集接口：未解析（详见 network.json 的 targetMappings/errors）。");
        } else {
            for (const QJsonValue &value : interfaces) {
                const QJsonObject interfaceObject = value.toObject();
                const QJsonObject counters = interfaceObject.value(QStringLiteral("counters")).toObject();
                const QJsonObject inDiscards = counters.value(QStringLiteral("InDiscards")).toObject();
                const QJsonObject inErrors = counters.value(QStringLiteral("InErrors")).toObject();
                lines << QStringLiteral("Windows interface：%1 (index=%2) InDiscards delta=%3，InErrors delta=%4")
                             .arg(interfaceObject.value(QStringLiteral("friendlyName")).toString(QStringLiteral("未记录")))
                             .arg(interfaceObject.value(QStringLiteral("interfaceIndex")).toInt())
                             .arg(inDiscards.value(QStringLiteral("delta")).toString(QStringLiteral("未记录")))
                             .arg(inErrors.value(QStringLiteral("delta")).toString(QStringLiteral("未记录")));
            }
        }
        const QJsonObject udp = latestIngress.value(QStringLiteral("systemUdpCounters")).toObject();
        const QJsonObject udpInErrors = udp.value(QStringLiteral("InErrors")).toObject();
        lines << QStringLiteral("system IPv4/UDP InErrors delta=%1（scope=system_ipv4）")
                     .arg(udpInErrors.value(QStringLiteral("delta")).toString(QStringLiteral("未记录")));
        lines << QStringLiteral("以上 Windows interface/system counters 用于定位 ingress 层级，不等同于具体 NIC hardware 丢包。");
    }

    QHash<int, QJsonObject> latestCards;
    for (const QJsonValue &value : network.value(QStringLiteral("cards")).toArray()) {
        const QJsonObject card = value.toObject();
        const int cardNo = cardField(card, QStringLiteral("card")).toInt(
            cardField(card, QStringLiteral("cardIndex")).toInt());
        if (cardNo > 0) latestCards.insert(cardNo, card);
    }
    for (auto it = latestCards.constBegin(); it != latestCards.constEnd(); ++it) {
        const QJsonObject &card = it.value();
        lines << QStringLiteral("卡%1 socket=%2 dequeued=%3 rawGapEvents=%4 rawGapPackets=%5 stale=%6 assemblyDuplicate=%7 assemblyOffsetOutOfRange=%8")
                     .arg(it.key())
                     .arg(cardFieldText(card, {QStringLiteral("socketPacketsReceived")}))
                     .arg(cardFieldText(card, {QStringLiteral("processorPacketsDequeued")}))
                     .arg(cardFieldText(card, {QStringLiteral("sameTriggerForwardGapEvents")}))
                     .arg(cardFieldText(card, {QStringLiteral("sameTriggerForwardGapPackets")}))
                     .arg(cardFieldText(card, {QStringLiteral("staleTriggerPacketsDiscarded")}))
                     .arg(cardFieldText(card, {QStringLiteral("assemblyDuplicatePackets")}))
                     .arg(cardFieldText(card, {QStringLiteral("assemblyOffsetOutOfRangePackets")}));
    }

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

// Open fixed file identities under the rotation lease, then copy their fixed
// prefixes after releasing it. Missing keys must never be reopened later.
struct PinnedHistory {
    struct File { QString path; qint64 size; std::unique_ptr<QFile> file; };
    std::vector<File> files;
    std::shared_ptr<DiagnosticRecorder::HistorySnapshot> snapshot =
        std::make_shared<DiagnosticRecorder::HistorySnapshot>();
};

std::unique_ptr<PinnedHistory> pinHistory(const QString &directory, QString *error, bool timeWindow)
{
    auto pinned = std::make_unique<PinnedHistory>();
    auto &snapshot = pinned->snapshot;
    const QFileInfo runInfo(directory), rootInfo(QFileInfo(directory).absolutePath());
    if (runInfo.isSymLink() || runInfo.isJunction()
        || rootInfo.isSymLink() || rootInfo.isJunction()) {
        *error = QStringLiteral("不安全的日志目录：%1").arg(directory); return {};
    }
    // A failed recorder startup can leave the root absent or blocked by a
    // regular file. Keep an empty immutable cut so captured memory evidence
    // can still be exported with missing files and the recorded write error.
    if (!runInfo.isDir() || !rootInfo.isDir()) return pinned;
    const QStringList names = {QStringLiteral("runtime.log"), QStringLiteral("events.jsonl"),
        QStringLiteral("network_history.jsonl"), QStringLiteral("settings_history.jsonl"),
        QStringLiteral("card_history.jsonl")};
    QStringList paths;
    for (const QString &name : names) {
        paths << QDir(directory).filePath(name + QStringLiteral(".previous"))
              << QDir(directory).filePath(name);
    }
    const int historyPathCount = paths.size();
    const QString receiptPath = QDir(directory).filePath(QStringLiteral("history-retention.json"));
    paths << receiptPath;
    // Manual time-window exports use only the retention receipt; the other
    // metadata files are not read by that path.
    if (!timeWindow) {
        paths << QDir(directory).filePath(QStringLiteral("network.json"))
              << QDir(directory).filePath(QStringLiteral("settings.json"))
              << QDir(directory).filePath(QStringLiteral("manifest.json"));
    }
    qint64 bytes = 0, historyBytes = 0, metadataBytes = 0;
    for (int i = 0; i < paths.size(); ++i) {
        const QString &path = paths.at(i);
        const QFileInfo info(path);
        if (info.isSymLink() || info.isJunction() || (info.exists() && !info.isFile())) {
            *error = QStringLiteral("不安全的日志文件：%1").arg(path); return {};
        }
        if (!info.exists()) continue;
        auto file = std::make_unique<QFile>();
        qint64 size = -1;
#ifdef Q_OS_WIN
        HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            *error = QStringLiteral("无法安全读取日志快照：%1").arg(path); return {};
        }
        BY_HANDLE_FILE_INFORMATION attributes;
        LARGE_INTEGER length;
        if (!GetFileInformationByHandle(handle, &attributes)
            || (attributes.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))
            || GetFileType(handle) != FILE_TYPE_DISK || !GetFileSizeEx(handle, &length)) {
            CloseHandle(handle);
            *error = QStringLiteral("无法安全读取日志快照：%1").arg(path); return {};
        }
        size = length.QuadPart;
        const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_RDONLY | _O_BINARY);
        if (fd == -1) { CloseHandle(handle); *error = QStringLiteral("无法打开日志快照：%1").arg(path); return {}; }
        if (!file->open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
            _close(fd); *error = QStringLiteral("无法打开日志快照：%1").arg(path); return {};
        }
#else
        const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_NOFOLLOW);
        struct stat attributes;
        if (fd < 0) {
            *error = QStringLiteral("无法安全读取日志快照：%1").arg(path); return {};
        }
        if (::fstat(fd, &attributes) != 0 || !S_ISREG(attributes.st_mode)) {
            ::close(fd); *error = QStringLiteral("无法安全读取日志快照：%1").arg(path); return {};
        }
        size = attributes.st_size;
        if (!file->open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
            ::close(fd); *error = QStringLiteral("无法打开日志快照：%1").arg(path); return {};
        }
#endif
        qint64 &part = i < historyPathCount ? historyBytes : metadataBytes;
        const qint64 limit = i < historyPathCount ? kSnapshotPerRunLimit : kSnapshotMetadataLimit;
        if (size < 0 || size > limit - part) {
            *error = i < historyPathCount ? QStringLiteral("活动日志快照超过 32 MiB 限额")
                : QStringLiteral("活动日志元数据超过 8 MiB 限额"); return {};
        }
        part += size;
        bytes += size;
        pinned->files.push_back({path, size, std::move(file)});
    }
    {
        std::lock_guard<std::mutex> lock(g_snapshotBudgetMutex);
        if (bytes > kSnapshotBudget - g_snapshotBytes) {
            *error = QStringLiteral("并发导出快照超过 80 MiB 限额，请稍后重试"); return {};
        }
        g_snapshotBytes += bytes;
        snapshot->reserved = bytes;
    }
    if (timeWindow) {
        // QSaveFile replaces this small receipt during rotation. Close its
        // handle before releasing coordination so Windows can replace it.
        for (auto it = pinned->files.begin(); it != pinned->files.end(); ++it) {
            if (it->path != receiptPath) continue;
            if (it->size > 64 * 1024) {
                *error = QStringLiteral("活动日志轮转记录超过 64 KiB 限额"); return {};
            }
            const QByteArray data = it->file->read(it->size);
            if (data.size() != it->size || it->file->error() != QFile::NoError) {
                *error = QStringLiteral("读取日志快照失败：%1").arg(it->path); return {};
            }
            snapshot->files.insert(it->path, data);
            pinned->files.erase(it);
            break;
        }
    }
    return pinned;
}

std::shared_ptr<const DiagnosticRecorder::HistorySnapshot> readPinnedHistory(
    std::unique_ptr<PinnedHistory> pinned, QString *error, bool timeWindow)
{
    if (!pinned) return {};
#ifdef PAIMAGE_DIAGNOSTIC_TEST_SEAM
    if (timeWindow) {
        std::function<void()> hook;
        { std::lock_guard<std::mutex> lock(g_snapshotHookMutex); hook = g_snapshotReadHook; }
        if (hook) hook();
    }
#else
    Q_UNUSED(timeWindow);
#endif
    for (auto &entry : pinned->files) {
        QByteArray data = entry.file->read(entry.size);
        if (data.size() != entry.size || entry.file->error() != QFile::NoError) {
            *error = QStringLiteral("读取日志快照失败：%1").arg(entry.path); return {};
        }
        pinned->snapshot->files.insert(entry.path, data);
    }
    return pinned->snapshot;
}

std::shared_ptr<const DiagnosticRecorder::HistorySnapshot> captureHistory(
    const QString &directory, QString *error, std::shared_ptr<QLockFile> rotationLease = {})
{
    const bool timeWindow = bool(rotationLease);
    auto pinned = pinHistory(directory, error, timeWindow);
    rotationLease.reset();
    return readPinnedHistory(std::move(pinned), error, timeWindow);
}

class HistoryReader
{
public:
    explicit HistoryReader(const QString &path, const DiagnosticRecorder::HistorySnapshot *snapshot = nullptr)
        : previous(path+QStringLiteral(".previous")), current(path), snap(snapshot), base(path) {}
    bool exists() const { return snap ? snap->files.contains(base) || snap->files.contains(base+QStringLiteral(".previous")) : current.exists() || previous.exists(); }
    bool open(QIODevice::OpenMode mode) {
        if (snap) {
            const QString old = base+QStringLiteral(".previous");
            if (snap->files.contains(old)) { oldBuffer.setData(snap->files.value(old)); oldBuffer.open(mode); }
            if (snap->files.contains(base)) { newBuffer.setData(snap->files.value(base)); newBuffer.open(mode); }
            return oldBuffer.isOpen() || newBuffer.isOpen();
        }
        const QFileInfo oldInfo(previous.fileName()), currentInfo(current.fileName());
        if (oldInfo.isSymLink() || oldInfo.isJunction() || currentInfo.isSymLink() || currentInfo.isJunction()
            || (oldInfo.exists() && !oldInfo.isFile()) || (currentInfo.exists() && !currentInfo.isFile())) {
            error = QStringLiteral("不安全的历史日志文件"); return false;
        }
        if (previous.exists() && !previous.open(mode)) { error = previous.errorString(); return false; }
        if (current.exists() && !current.open(mode)) { error = current.errorString(); return false; }
        return previous.isOpen() || current.isOpen();
    }
    bool atEnd() const { return snap ? (!oldBuffer.isOpen() || oldBuffer.atEnd()) && (!newBuffer.isOpen() || newBuffer.atEnd()) : (!previous.isOpen() || previous.atEnd()) && (!current.isOpen() || current.atEnd()); }
    QByteArray readLine() { if (snap) return oldBuffer.isOpen() && !oldBuffer.atEnd() ? oldBuffer.readLine() : newBuffer.readLine(); return previous.isOpen() && !previous.atEnd() ? previous.readLine() : current.readLine(); }
    QString errorString() const { return error; }
private:
    QFile previous, current;
    QBuffer oldBuffer, newBuffer;
    const DiagnosticRecorder::HistorySnapshot *snap;
    QString base, error;
};

bool capturedBytes(const QString &path, const DiagnosticRecorder::HistorySnapshot *snapshot, QByteArray *bytes)
{
    if (!snapshot) return readAllBytes(path, bytes);
    const auto it = snapshot->files.constFind(path);
    if (it == snapshot->files.constEnd()) return false;
    *bytes = it.value(); return true;
}

void appendRetentionReason(const QString &directory, QStringList *reasons, qint64 start = std::numeric_limits<qint64>::min(), const DiagnosticRecorder::HistorySnapshot *snapshot = nullptr)
{
    QByteArray bytes;
    if (!capturedBytes(QDir(directory).filePath(QStringLiteral("history-retention.json")), snapshot, &bytes)) return;
    const auto stamp = parseObject(bytes);
    const auto until = QDateTime::fromString(stamp.value(QStringLiteral("evictedAt")).toString(),Qt::ISODateWithMs);
    if (!until.isValid() || start <= until.toMSecsSinceEpoch())
        reasons->append(QStringLiteral("部分旧结构化日志已按容量轮转，仅保留近期分段"));
}

QJsonArray readHistoryArray(const QString &path, quint64 boundary, QStringList *truncationReasons, bool *exists, const DiagnosticRecorder::HistorySnapshot *snapshot = nullptr)
{
    QJsonArray result;
    HistoryReader file(path, snapshot);
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

QJsonObject dropCountersObject(const DiagnosticRecorder::DropCounters &drops)
{
    return {{QStringLiteral("queueDropped"), static_cast<double>(drops.queueDropped)},
            {QStringLiteral("criticalQueueDropped"), static_cast<double>(drops.criticalQueueDropped)},
            {QStringLiteral("noiseDropped"), static_cast<double>(drops.noiseDropped)},
            {QStringLiteral("writeFallbackDropped"), static_cast<double>(drops.writeFallbackDropped)}};
}

// Merge only persisted run facts; a time window may deliberately scan past
// the old manifest's last checkpoint after an abnormal exit.
void mergeRunManifest(DiagnosticRecorder::ExportRequest &source, const QJsonObject &manifest,
                      bool adoptBoundary, QStringList &reasons)
{
    if (source.runId.isEmpty()) source.runId = manifest.value(QStringLiteral("runId")).toString();
    if (source.startIsoTime.isEmpty()) source.startIsoTime = manifest.value(QStringLiteral("startTime")).toString();
    if (source.endIsoTime.isEmpty()) source.endIsoTime = manifest.value(QStringLiteral("endTime")).toString();
    if (adoptBoundary && !source.boundarySequence)
        source.boundarySequence = qMax<qint64>(0, manifest.value(QStringLiteral("boundarySequence")).toInteger());
    if (manifest.value(QStringLiteral("active")).toBool() && !source.sourceWasActive)
        reasons.append(QStringLiteral("该运行未正常结束，仅恢复已落盘记录，末尾日志可能缺失"));
    source.writeError |= manifest.value(QStringLiteral("writeError")).toBool();
    if (source.writeErrorText.isEmpty())
        source.writeErrorText = manifest.value(QStringLiteral("writeErrorText")).toString();
    const auto counters = manifest.value(QStringLiteral("dropCounters")).toObject();
    const auto merge = [&](quint64 &value, const QString &name) {
        value = qMax(value, static_cast<quint64>(qMax<qint64>(0, counters.value(name).toInteger())));
    };
    merge(source.drops.queueDropped, QStringLiteral("queueDropped"));
    merge(source.drops.criticalQueueDropped, QStringLiteral("criticalQueueDropped"));
    merge(source.drops.noiseDropped, QStringLiteral("noiseDropped"));
    merge(source.drops.writeFallbackDropped, QStringLiteral("writeFallbackDropped"));
    for (const auto &value : manifest.value(QStringLiteral("truncationReasons")).toArray()) {
        if (value.isString() && !value.toString().isEmpty()) reasons.append(value.toString());
    }
}

struct FilteredEvents { QByteArray runtime; QByteArray events; quint64 count = 0; };

FilteredEvents readFilteredEvents(const QString &directory, quint64 boundary, const QVector<DiagnosticRecorder::Event> &fallback,
                                  QStringList *missing, QStringList *truncationReasons, const DiagnosticRecorder::HistorySnapshot *snapshot = nullptr)
{
    FilteredEvents result;
    QMap<quint64, QByteArray> eventLines;
    QMap<quint64, QByteArray> runtimeLines;
    const QString eventPath = QDir(directory).filePath(QStringLiteral("events.jsonl"));
    HistoryReader eventFile(eventPath, snapshot);
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
    HistoryReader runtimeFile(runtimePath, snapshot);
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
    QJsonArray networkSnapshots = readHistoryArray(QDir(directory).filePath(QStringLiteral("network_history.jsonl")), boundary, truncationReasons, &networkExists, request.historySnapshot.get());
    QJsonArray settingsSnapshots = readHistoryArray(QDir(directory).filePath(QStringLiteral("settings_history.jsonl")), boundary, truncationReasons, &settingsExists, request.historySnapshot.get());
    QJsonArray cards = readHistoryArray(QDir(directory).filePath(QStringLiteral("card_history.jsonl")), boundary, truncationReasons, &cardsExists, request.historySnapshot.get());
    QSet<quint64> networkSeen, settingsSeen, cardSeen;
    for (const QJsonValue &value : networkSnapshots) networkSeen.insert(static_cast<quint64>(value.toObject().value(QStringLiteral("sequence")).toDouble(0)));
    for (const QJsonValue &value : settingsSnapshots) settingsSeen.insert(static_cast<quint64>(value.toObject().value(QStringLiteral("sequence")).toDouble(0)));
    for (const QJsonValue &value : cards) cardSeen.insert(static_cast<quint64>(value.toObject().value(QStringLiteral("sequence")).toDouble(0)));
    QByteArray bytes;
    if (!networkExists && capturedBytes(QDir(directory).filePath(QStringLiteral("network.json")), request.historySnapshot.get(), &bytes)) {
        const QJsonObject old = parseObject(bytes);
        appendUniqueBySequence(&networkSnapshots, old.value(QStringLiteral("snapshots")).toArray(), &networkSeen, boundary);
        appendUniqueBySequence(&cards, old.value(QStringLiteral("cards")).toArray(), &cardSeen, boundary);
    }
    if (!settingsExists && capturedBytes(QDir(directory).filePath(QStringLiteral("settings.json")), request.historySnapshot.get(), &bytes)) {
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
    bool add(const QString &name, const QByteArray &data, QString *error)
    {
        const QByteArray nameBytes = name.toUtf8();
        constexpr auto limit = std::numeric_limits<quint32>::max();
        if (nameBytes.isEmpty() || nameBytes.size() > std::numeric_limits<quint16>::max()
            || static_cast<quint64>(data.size()) >= limit || m_file.pos() >= limit
            || m_entries.size() >= std::numeric_limits<quint16>::max() - 1) {
            if (error) *error = QStringLiteral("诊断 ZIP 超出格式上限：%1；请缩短导出时间范围").arg(name);
            return false;
        }
        CentralEntry entry;
        entry.name = nameBytes;
        entry.crc = ::crc32(0, reinterpret_cast<const Bytef *>(data.constData()), static_cast<uInt>(data.size()));
        entry.size = static_cast<quint32>(data.size());
        entry.offset = static_cast<quint32>(m_file.pos());
        entry.method = data.size() >= 1024 ? 8 : 0;
        QByteArray header;
        appendU32(&header, 0x04034b50U); appendU16(&header, 20); appendU16(&header, 0x0800); appendU16(&header, entry.method); appendU16(&header, 0); appendU16(&header, 0);
        appendU32(&header, entry.crc); appendU32(&header, 0); appendU32(&header, entry.size); appendU16(&header, static_cast<quint16>(nameBytes.size())); appendU16(&header, 0);
        if (!write(header, error) || !write(nameBytes, error)) return false;
        const qint64 payload = m_file.pos();
        if (entry.method ? !writeDeflate(data, error) : !write(data, error)) return false;
        const qint64 end = m_file.pos();
        if (end >= limit || end < payload) {
            if (error) *error = QStringLiteral("诊断 ZIP 超出格式上限，请缩短导出时间范围");
            return false;
        }
        entry.compressedSize = static_cast<quint32>(end - payload);
        QByteArray compressed;
        appendU32(&compressed, entry.compressedSize);
        if (!m_file.seek(qint64(entry.offset) + 18) || !write(compressed, error) || !m_file.seek(end)) {
            if (error && error->isEmpty()) *error = QStringLiteral("更新诊断 ZIP 文件头失败");
            return false;
        }
        m_entries.append(entry);
        return true;
    }
    bool close(QString *error)
    {
        const quint32 centralOffset = static_cast<quint32>(m_file.pos());
        for (const CentralEntry &entry : m_entries) {
            QByteArray central;
            appendU32(&central, 0x02014b50U); appendU16(&central, 20); appendU16(&central, 20); appendU16(&central, 0x0800); appendU16(&central, entry.method); appendU16(&central, 0); appendU16(&central, 0);
            appendU32(&central, entry.crc); appendU32(&central, entry.compressedSize); appendU32(&central, entry.size); appendU16(&central, static_cast<quint16>(entry.name.size()));
            appendU16(&central, 0); appendU16(&central, 0); appendU16(&central, 0); appendU16(&central, 0); appendU32(&central, 0); appendU32(&central, entry.offset);
            if (!write(central, error) || !write(entry.name, error)) return false;
        }
        if (m_file.pos() + 22 >= std::numeric_limits<quint32>::max()) {
            if (error) *error = QStringLiteral("诊断 ZIP 超出格式上限，请缩短导出时间范围");
            return false;
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
    struct CentralEntry { QByteArray name; quint32 crc = 0, size = 0, compressedSize = 0, offset = 0; quint16 method = 0; };
    static void appendU16(QByteArray *bytes, quint16 value) { bytes->append(static_cast<char>(value & 0xff)); bytes->append(static_cast<char>((value >> 8) & 0xff)); }
    static void appendU32(QByteArray *bytes, quint32 value) { appendU16(bytes, static_cast<quint16>(value & 0xffff)); appendU16(bytes, static_cast<quint16>((value >> 16) & 0xffff)); }
    bool writeDeflate(const QByteArray &data, QString *error)
    {
        z_stream stream{};
        if (deflateInit2(&stream, Z_BEST_SPEED, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
            if (error) *error = QStringLiteral("初始化 ZIP 压缩失败");
            return false;
        }
        stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.constData()));
        stream.avail_in = static_cast<uInt>(data.size());
        std::array<Bytef, 64 * 1024> buffer{};
        int result;
        bool ok = true;
        do {
            stream.next_out = buffer.data();
            stream.avail_out = static_cast<uInt>(buffer.size());
            result = deflate(&stream, Z_FINISH);
            const auto count = static_cast<qint64>(buffer.size() - stream.avail_out);
            if ((result != Z_OK && result != Z_STREAM_END) ||
                m_file.write(reinterpret_cast<const char *>(buffer.data()), count) != count) {
                ok = false; break;
            }
        } while (result != Z_STREAM_END);
        deflateEnd(&stream);
        if (!ok && error) *error = QStringLiteral("ZIP 压缩或写入失败：%1").arg(m_file.errorString());
        return ok;
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
    manifest.insert(QStringLiteral("dropCounters"), dropCountersObject(drops));
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
    if (directory.isEmpty() || !QDir(directory).exists()) return false;
    // The marker is deliberately only informational. A process crash leaves
    // it behind, while QLockFile can validate/reclaim the stale PID lock.
    QLockFile lock(QDir(directory).filePath(QStringLiteral(".active.lock")));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) return true;
    lock.unlock();
    return false;
}

bool readRetentionManifest(const QString &directory, QByteArray *bytes)
{
    const QString path = QDir(directory).filePath(QStringLiteral("manifest.json"));
    const QFileInfo info(path);
    return info.size() <= 8LL * 1024 * 1024 && readAllBytes(path, bytes);
}

void pruneRuns(const QString &rootDirectory, const DiagnosticRecorder::Options &options, qint64 reserve=0, bool locked=false)
{
    QDir root(rootDirectory);
    if (!root.exists()) return;
    if (QFileInfo(rootDirectory).isSymLink() || QFileInfo(rootDirectory).isJunction()) return;
    const auto retentionLock = locked ? std::shared_ptr<QLockFile>() : diagnostic_storage::lease(rootDirectory);
    if (!locked && !retentionLock) return;
    const QString rootCanonical = root.canonicalPath();
    struct Run { QString path; qint64 bytes = 0; QDateTime modified; bool active = false; };
    QVector<Run> runs;
    static const QRegularExpression known(QStringLiteral(
        "^(\\.active\\.lock|\\.history\\.lock|\\.active|\\.reservation|manifest\\.json|summary\\.txt|network\\.json|settings\\.json|"
        "history-retention\\.json|(?:runtime\\.log|events\\.jsonl|(?:network|settings|card)_history\\.jsonl)"
        "(?:\\.previous(?:\\.retired-[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})?)?)$"));
    const QFileInfoList entries = root.entryInfoList({QStringLiteral("run-*")}, QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time);
    for (const QFileInfo &info : entries) {
        QByteArray manifestBytes;
        const QString canonical = info.canonicalFilePath();
        if (info.isSymLink() || info.isJunction() || canonical.isEmpty() || QFileInfo(canonical).absolutePath() != rootCanonical
            || !readRetentionManifest(info.absoluteFilePath(), &manifestBytes)) continue;
        const QJsonObject manifest = parseObject(manifestBytes);
        const QString id = manifest.value(QStringLiteral("runId")).toString();
        if (manifest.value(QStringLiteral("formatVersion")).toInt(0) != 1 || id.isEmpty()
            || !info.fileName().endsWith(QLatin1Char('-') + id)) continue;
        // Do not prune historical captures or unexpected user files.
        bool unexpected = false;
        for (const auto &child : QDir(info.absoluteFilePath()).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot))
            if (!child.isFile() || child.isSymLink() || child.isJunction() || !known.match(child.fileName()).hasMatch()) { unexpected = true; break; }
        if (unexpected) continue;
        runs.append({info.absoluteFilePath(), directoryBytes(info.absoluteFilePath()), info.lastModified(), runIsActive(info.absoluteFilePath())});
    }
    std::sort(runs.begin(), runs.end(), [](const Run &a, const Run &b) { return a.modified < b.modified; });
    qint64 total = 0;
    int ended = 0;
    for (const Run &run : runs) { total += run.bytes; if (!run.active) ++ended; }
    for (const Run &run : runs) {
        if (run.active) continue;
        if (ended <= options.maxRetainedRuns && total <= qMax<qint64>(0,options.maxRetainedBytes-reserve) &&
            run.modified >= QDateTime::currentDateTimeUtc().addDays(-diagnostic_storage::RetainDays)) break;
        QLockFile runLock(QDir(run.path).filePath(QStringLiteral(".active.lock")));
        runLock.setStaleLockTime(0);
        if (!runLock.tryLock(0)) continue;
        const auto historyLock = diagnostic_storage::historyLease(run.path, 0);
        if (!historyLock) continue;
        bool removed = true;
        const auto children = QDir(run.path).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
        for (const auto &child : children) {
            if (!child.isFile() || child.isSymLink() || child.isJunction() || !known.match(child.fileName()).hasMatch()) { removed = false; break; }
        }
        if (!removed) continue;
        const auto prunePermit = diagnostic_storage::tryPrune(rootDirectory);
        if (!prunePermit) continue;
        for (const auto &child : children) {
            if (child.fileName() == QStringLiteral(".active.lock") || child.fileName() == QStringLiteral(".history.lock")) continue;
            if (!QFile::remove(child.absoluteFilePath())) { removed = false; break; }
        }
        historyLock->unlock();
        runLock.unlock();
        if (removed && root.rmdir(QFileInfo(run.path).fileName())) { total -= run.bytes; --ended; }
    }
}

quint64 maxSequenceOnDisk(const QString &directory)
{
    quint64 maximum = 0;
    const QStringList lineFiles = {QStringLiteral("events.jsonl"), QStringLiteral("network_history.jsonl"),
                                   QStringLiteral("settings_history.jsonl"), QStringLiteral("card_history.jsonl")};
    for (const QString &name : lineFiles) {
        HistoryReader file(QDir(directory).filePath(name));
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

struct DiagnosticRecorder::PendingHistoryCut {
    using Pin = std::pair<std::unique_ptr<PinnedHistory>, QString>;
    std::promise<Pin> ready;
    std::future<Pin> files = ready.get_future();
    std::mutex readMutex;
    std::shared_ptr<const HistorySnapshot> snapshot;
    QString error;

    void materialize(ExportRequest &source)
    {
        std::lock_guard<std::mutex> lock(readMutex);
        if (files.valid()) {
            auto [pinned, pinError] = files.get();
            error = std::move(pinError);
            snapshot = readPinnedHistory(std::move(pinned), &error, true);
        }
        source.historySnapshot = snapshot;
        source.captureError = error;
    }
};

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
        options.maxHistorySegmentBytes=qMax<qint64>(1,options.maxHistorySegmentBytes);
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
        if (!safeDirectory(rootDirectory)) {
            setWriteError(QStringLiteral("不安全的诊断根目录：%1").arg(rootDirectory));
            { std::lock_guard<std::mutex> guard(mutex); active = true; accepting = true; }
            return;
        }
        if (!QDir().mkpath(runDirectory)) setWriteError(QStringLiteral("创建运行目录失败：%1").arg(runDirectory));
        activeMarkerPath = QDir(runDirectory).filePath(QStringLiteral(".active"));
        QFile marker(activeMarkerPath);
        if (marker.open(QIODevice::WriteOnly | QIODevice::Truncate)) { marker.write(runId.toUtf8()); marker.close(); } else setWriteError(QStringLiteral("创建运行标记失败：%1").arg(marker.errorString()));
        activeLock = std::make_unique<QLockFile>(activeMarkerPath + QStringLiteral(".lock"));
        activeLock->setStaleLockTime(0);
        if (!activeLock->tryLock(0)) setWriteError(QStringLiteral("获取运行锁失败：%1").arg(activeLock->error()));
        runtimeFile.setFileName(QDir(runDirectory).filePath(QStringLiteral("runtime.log"))); eventsFile.setFileName(QDir(runDirectory).filePath(QStringLiteral("events.jsonl"))); networkHistoryFile.setFileName(QDir(runDirectory).filePath(QStringLiteral("network_history.jsonl"))); settingsHistoryFile.setFileName(QDir(runDirectory).filePath(QStringLiteral("settings_history.jsonl"))); cardHistoryFile.setFileName(QDir(runDirectory).filePath(QStringLiteral("card_history.jsonl")));
        { std::lock_guard<std::mutex> guard(mutex); active = true; accepting = true; quotaPending = true; quotaError = QStringLiteral("诊断日志等待容量准入"); }
        // Admission is attempted synchronously so callers can observe a blocked run immediately.
        tryAdmission();
        canPinHistory = true;
        worker = std::thread([this] { workerLoop(); });
    }

    qint64 activeReservation(const QFileInfo &entry) const
    {
        const QString id = entry.fileName().right(36);
        if (QUuid(id).isNull() || !entry.fileName().endsWith(QLatin1Char('-') + id)
            || entry.isSymLink() || entry.isJunction() || !runIsActive(entry.absoluteFilePath())) return 0;
        const QFileInfo info(QDir(entry.absoluteFilePath()).filePath(QStringLiteral(".reservation")));
        if (info.isSymLink() || info.isJunction() || !info.isFile() || info.size() > 32) return 0;
        QFile file(info.filePath());
        if (!file.open(QIODevice::ReadOnly)) return 0;
        bool ok = false; const qint64 value = file.read(33).trimmed().toLongLong(&ok);
        return ok && value > 0 ? qMin(value, options.maxRetainedBytes) : 0;
    }
    qint64 addQuota(qint64 a, qint64 b) const
    {
        return a >= options.maxRetainedBytes || b >= options.maxRetainedBytes - a ? options.maxRetainedBytes : a + b;
    }
    bool tryAdmission()
    {
        const qint64 metadataBudget = qMin<qint64>(16LL * 1024 * 1024, qMax<qint64>(16 * 1024, options.maxRetainedBytes / 2));
        const qint64 budget = qMin(options.maxRetainedBytes,
            options.maxHistorySegmentBytes > (std::numeric_limits<qint64>::max() - metadataBudget) / 2
                ? options.maxRetainedBytes : 2 * options.maxHistorySegmentBytes + metadataBudget);
        QString failure;
        if (!safeDirectory(rootDirectory) || !safeDirectory(runDirectory)) failure = QStringLiteral("不安全的诊断目录");
        else if (budget <= metadataBudget + 1) failure = QStringLiteral("诊断日志容量不足，无法预留元数据和历史空间");
        const auto lock = failure.isEmpty() && safeLockFile(rootDirectory, QStringLiteral(".retention.lock"))
            ? diagnostic_storage::lease(rootDirectory) : nullptr;
        if (failure.isEmpty() && !lock) failure = QStringLiteral("诊断日志保留锁繁忙，等待容量准入");
        if (failure.isEmpty()) {
            // Count only safe, owned run directories. Active reservations cover future growth,
            // while inactive runs are charged their current named file sizes.
            qint64 otherReservations = 0;
            QDir root(rootDirectory);
            const auto entries = root.entryInfoList({QStringLiteral("run-*")}, QDir::Dirs | QDir::NoDotAndDotDot);
            for (const auto &entry : entries) {
                if (entry.absoluteFilePath() == runDirectory || entry.isSymLink() || entry.isJunction()) continue;
                const QString path = entry.absoluteFilePath();
                const qint64 reserved = activeReservation(entry);
                if (reserved) otherReservations = addQuota(otherReservations, qMax<qint64>(0, reserved - directoryBytes(path)));
            }
            pruneRuns(rootDirectory, options, addQuota(budget, otherReservations), true);
            qint64 used = 0;
            for (const auto &entry : root.entryInfoList({QStringLiteral("run-*")}, QDir::Dirs | QDir::NoDotAndDotDot)) {
                if (entry.absoluteFilePath() == runDirectory || entry.isSymLink() || entry.isJunction()) continue;
                const qint64 bytes = directoryBytes(entry.absoluteFilePath());
                const qint64 reservation = activeReservation(entry);
                if (reservation) { used = addQuota(used, qMax(bytes, reservation)); continue; }
                QByteArray manifest;
                if (!readRetentionManifest(entry.absoluteFilePath(), &manifest)) continue;
                const auto obj = parseObject(manifest);
                if (obj.value(QStringLiteral("formatVersion")).toInt() != 1 || obj.value(QStringLiteral("runId")).toString().isEmpty()
                    || !entry.fileName().endsWith(QLatin1Char('-') + obj.value(QStringLiteral("runId")).toString())) continue;
                used = addQuota(used, bytes);
            }
            if (used > options.maxRetainedBytes - budget) failure = QStringLiteral("诊断日志保留容量不足，等待历史记录释放");
            else if (!writeAtomicBytes(QDir(runDirectory).filePath(QStringLiteral(".reservation")), QByteArray::number(budget), &failure)) {}
            else {
                metadataLimit = metadataBudget / 2;
                segmentLimit = (budget - metadataBudget) / 2;
                ensureFile(runtimeFile.fileName(), QStringLiteral("runtime.log")); ensureFile(eventsFile.fileName(), QStringLiteral("events.jsonl"));
                ensureFile(networkHistoryFile.fileName(), QStringLiteral("network_history.jsonl")); ensureFile(settingsHistoryFile.fileName(), QStringLiteral("settings_history.jsonl")); ensureFile(cardHistoryFile.fileName(), QStringLiteral("card_history.jsonl"));
                writeInitialArtifacts();
                { std::lock_guard<std::mutex> guard(mutex); admitted = true; quotaPending = false; quotaError.clear(); }
                progress.notify_all();
                return true;
            }
        }
        { std::lock_guard<std::mutex> guard(mutex); quotaPending = true; quotaError = failure; }
        progress.notify_all();
        return false;
    }

    void stopRun()
    {
        { std::lock_guard<std::mutex> guard(mutex); if (!accepting && !worker.joinable()) return; accepting = false; stopping = true; flushRequested = true; flushTarget = nextSequence ? nextSequence - 1 : 0; }
        condition.notify_all(); if (worker.joinable()) worker.join();
        quint64 boundary = 0; { std::lock_guard<std::mutex> guard(mutex); active = false; endIsoTime = nowIso(); boundary = nextSequence ? nextSequence - 1 : 0; }
        if (admitted) writeRunArtifacts(boundary, false, QString());
        if (admitted) QFile::remove(QDir(runDirectory).filePath(QStringLiteral(".reservation")));
        if (!activeMarkerPath.isEmpty()) QFile::remove(activeMarkerPath);
        if (activeLock) activeLock->unlock();
        if (!admitted) QDir(rootDirectory).rmdir(QFileInfo(runDirectory).fileName());
        pruneRuns(rootDirectory, options);
    }

    qint64 monotonicMs() const { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - monoStart).count(); }
    void completeLocked(quint64 sequence)
    {
        if (!sequence || sequence <= processedSequence) return;
        auto next = completed.upperBound(sequence);
        quint64 first = sequence, last = sequence;
        if (next != completed.begin()) {
            auto previous = next; --previous;
            if (previous.value() >= sequence - 1) { first = previous.key(); last = qMax(last, previous.value()); completed.erase(previous); }
        }
        next = completed.lowerBound(first);
        if (next != completed.end() && next.key() <= last + 1) { last = qMax(last, next.value()); completed.erase(next); }
        completed.insert(first, last);
        auto head = completed.begin();
        if (head != completed.end() && head.key() <= processedSequence + 1) { processedSequence = head.value(); completed.erase(head); }
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
    struct HistoryWrite {
        QFile *file = nullptr;
        QByteArray bytes;
    };
    std::array<QFile *, 5> historyFiles()
    {
        return {&runtimeFile, &eventsFile, &networkHistoryFile, &settingsHistoryFile, &cardHistoryFile};
    }
    std::array<HistoryWrite, 2> serializeItem(const QueueItem &item)
    {
        switch (item.kind) {
        case QueueItem::Kind::Event:
            return {{{&eventsFile, eventJsonBytes(item.event)}, {&runtimeFile, runtimeLineBytes(item.event)}}};
        case QueueItem::Kind::NetworkSnapshot:
            return {{{&networkHistoryFile, jsonBytes(snapshotJson(item.snapshot)) + '\n'}, {}}};
        case QueueItem::Kind::SettingsSnapshot:
            return {{{&settingsHistoryFile, jsonBytes(snapshotJson(item.snapshot)) + '\n'}, {}}};
        case QueueItem::Kind::CardSnapshot:
            return {{{&cardHistoryFile, jsonBytes(cardSnapshotJson(item.card)) + '\n'}, {}}};
        }
        return {};
    }
    void reconcileSegmentBytes()
    {
        segmentBytes = 0;
        for (const QFile *file : historyFiles()) {
            const qint64 size = file->isOpen() ? file->size() : QFileInfo(file->fileName()).size();
            if (size > 0) segmentBytes += size;
        }
    }
    qint64 segmentBytes = 0;
    QString previousSegmentEnd;
    bool historyLockTimedOut = false;
    std::vector<std::weak_ptr<PendingHistoryCut>> pendingHistory;
    std::shared_ptr<PendingHistoryCut> rotationCut;
    bool canPinHistory = false;

    std::shared_ptr<PendingHistoryCut> capturePendingHistory()
    {
        auto cut = std::make_shared<PendingHistoryCut>();
        QString error;
        auto pinned = pinHistory(runDirectory, &error, true);
        cut->ready.set_value({std::move(pinned), std::move(error)});
        return cut;
    }

    void pinPendingHistory()
    {
        std::vector<std::weak_ptr<PendingHistoryCut>> pending;
        {
            std::lock_guard<std::mutex> lock(mutex);
            pending.swap(pendingHistory);
        }
        for (const auto &weak : pending) {
            if (auto cut = weak.lock()) {
                QString error;
                auto pinned = pinHistory(runDirectory, &error, true);
                cut->ready.set_value({std::move(pinned), std::move(error)});
            }
        }
    }

    bool rotateHistory(QString *error, bool *deferred)
    {
        const auto guard = diagnostic_storage::historyLease(runDirectory, historyLockTimedOut ? 0 : 2500);
        if (!guard) {
            if (error) *error = QStringLiteral("获取诊断历史轮转锁超时或失败：%1").arg(runDirectory);
            if (!historyLockTimedOut)
                std::fprintf(stderr, "Diagnostic history rotation lock unavailable: %s\n", qPrintable(runDirectory));
            historyLockTimedOut = true;
            if (deferred) *deferred = true;
            return false;
        }
        historyLockTimedOut = false;
        if (!flushFiles(error)) return false;
        const std::array<QFile *, 5> files{&runtimeFile,&eventsFile,&networkHistoryFile,&settingsHistoryFile,&cardHistoryFile};
        // Validate every path before changing any segment. In particular,
        // exists() alone does not detect a dangling symbolic link.
        for (const auto *file : files) {
            const QFileInfo current(file->fileName());
            const QFileInfo previous(file->fileName() + QStringLiteral(".previous"));
            if (current.isSymLink() || current.isJunction() || !current.isFile()
                || previous.isSymLink() || previous.isJunction()
                || (previous.exists() && !previous.isFile())) {
                if (error) *error = QStringLiteral("诊断分段不是普通文件或包含链接：%1").arg(file->fileName());
                return false;
            }
        }
        // Publish a cut before changing either pathname. Clicks during the
        // filesystem transaction share this pre-rotation view; requests
        // registered earlier must be fulfilled before any file is renamed.
        auto cut = capturePendingHistory();
        std::vector<std::weak_ptr<PendingHistoryCut>> pending;
        {
            std::lock_guard<std::mutex> lock(mutex);
            rotationCut = cut;
            pending.swap(pendingHistory);
        }
        for (const auto &weak : pending) {
            if (auto registered = weak.lock()) {
                QString pinError;
                auto pinned = pinHistory(runDirectory, &pinError, true);
                registered->ready.set_value({std::move(pinned), std::move(pinError)});
            }
        }
        struct EndRotation {
            State &state;
            ~EndRotation() {
                // Destruction may close file handles; do not do it under the
                // state mutex used by recorders and the export click.
                std::shared_ptr<PendingHistoryCut> finished;
                { std::lock_guard<std::mutex> lock(state.mutex); finished.swap(state.rotationCut); }
            }
        } endRotation{*this};
        bool evicted = false;
        for (auto *file : files) {
            file->close();
            const auto previous=file->fileName()+QStringLiteral(".previous");
            if (QFileInfo::exists(previous)) {
                evicted = true;
                #ifdef Q_OS_WIN
                // A shared reader can keep the old inode alive after deletion.
                // Rename first so its delete-pending name cannot block reuse.
                const QString retired = previous + QStringLiteral(".retired-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
                if (!MoveFileExW(reinterpret_cast<LPCWSTR>(previous.utf16()), reinterpret_cast<LPCWSTR>(retired.utf16()), 0)) {
                    if(error)*error=QStringLiteral("旧诊断分段无法移除"); return false;
                }
                if (!DeleteFileW(reinterpret_cast<LPCWSTR>(retired.utf16()))) {
                    MoveFileExW(reinterpret_cast<LPCWSTR>(retired.utf16()), reinterpret_cast<LPCWSTR>(previous.utf16()), 0);
                    if(error)*error=QStringLiteral("旧诊断分段无法移除"); return false;
                }
#else
                if (!QFile::remove(previous)) { if(error)*error=QStringLiteral("旧诊断分段无法移除"); return false; }
#endif
            }
            if (!QFile::rename(file->fileName(),previous) || !file->open(QIODevice::ReadWrite|QIODevice::Append|QIODevice::Unbuffered)) {
                if(error)*error=QStringLiteral("诊断分段轮转失败：%1").arg(file->fileName());
                return false;
            }
        }
        if (evicted && !writeAtomicBytes(QDir(runDirectory).filePath(QStringLiteral("history-retention.json")),
                jsonBytes(QJsonObject{{"evictedAt",previousSegmentEnd}}),error)) return false;
        previousSegmentEnd=nowIso();
        segmentBytes=0;
        return true;
    }
    bool writeItem(const QueueItem &item, QString *error, bool *deferred)
    {
        std::lock_guard<std::mutex> historyGuard(historyWriteMutex);
        { std::lock_guard<std::mutex> guard(mutex); if (writeError) { if (error) *error = writeErrorText; return false; } }
        const auto payloads = serializeItem(item);
        qint64 bytes = 0;
        for (const auto &payload : payloads) bytes += payload.bytes.size();
        if (bytes > segmentLimit) {
            if (error) *error = QStringLiteral("单条诊断记录超出分段容量");
            return false;
        }
        if (segmentBytes > segmentLimit - bytes && !rotateHistory(error, deferred)) {
            if (!*deferred) reconcileSegmentBytes();
            return false;
        }
        for (const auto &payload : payloads) {
            if (!payload.file) continue;
            if (!payload.file->isOpen() || payload.file->write(payload.bytes) != payload.bytes.size()) {
                if (error) *error = QStringLiteral("写入 %1 失败：%2")
                    .arg(QFileInfo(payload.file->fileName()).fileName(), payload.file->errorString());
                reconcileSegmentBytes();
                return false;
            }
            segmentBytes += payload.bytes.size();
        }
        return true;
    }
    bool flushFiles(QString *error)
    {
        for (QFile *file : {&runtimeFile, &eventsFile, &networkHistoryFile, &settingsHistoryFile, &cardHistoryFile}) if (file->isOpen() && !file->flush()) { if (error) *error = QStringLiteral("刷新诊断日志失败：%1").arg(file->errorString()); return false; } return true;
    }
    void openAppend(QFile &file, const QString &label) { if (!file.open(QIODevice::ReadWrite | QIODevice::Append | QIODevice::Unbuffered)) setWriteError(QStringLiteral("打开 %1 失败：%2").arg(label, file.errorString())); }
    void workerLoop()
    {
        bool opened = false;
        Clock::time_point lastFlush = Clock::now();
        for (;;) {
            pinPendingHistory();
            if (!admitted) {
                { std::unique_lock<std::mutex> lock(mutex);
                  if (stopping) { for (const auto &item : queue) if (item.kind == QueueItem::Kind::Event) appendFallbackLocked(item.event); queue.clear(); break; }
                  condition.wait_for(lock, std::chrono::milliseconds(250), [this] { return stopping || !pendingHistory.empty(); });
                  if (stopping) continue; }
                if (!tryAdmission()) continue;
            }
            if (!opened) {
                openAppend(runtimeFile, QStringLiteral("runtime.log")); openAppend(eventsFile, QStringLiteral("events.jsonl")); openAppend(networkHistoryFile, QStringLiteral("network_history.jsonl")); openAppend(settingsHistoryFile, QStringLiteral("settings_history.jsonl")); openAppend(cardHistoryFile, QStringLiteral("card_history.jsonl")); opened = true;
            }
            QueueItem item; bool haveItem = false, flushNow = false, finish = false;
            { std::unique_lock<std::mutex> lock(mutex); if (queue.empty() && !stopping && !flushRequested && pendingHistory.empty()) condition.wait_for(lock, std::chrono::milliseconds(1000)); if (!pendingHistory.empty()) continue; if (!queue.empty()) { item = queue.front(); queue.pop_front(); inFlight = item; hasInFlight = true; haveItem = true; } else { const bool targetReached = flushRequested && processedSequence >= flushTarget; const bool timedFlush = Clock::now() - lastFlush >= std::chrono::seconds(1); if (stopping || targetReached || timedFlush) { flushNow = true; finish = stopping; if (targetReached || stopping) flushRequested = false; } } }
            if (haveItem) { QString error; bool deferred = false; const bool ok = writeItem(item, &error, &deferred); { std::lock_guard<std::mutex> guard(mutex); if (!ok) { if(deferred) { ++drops.queueDropped; if(item.critical)++drops.criticalQueueDropped; } else setWriteErrorLocked(error.isEmpty() ? QStringLiteral("诊断日志写入失败") : error); if (item.kind == QueueItem::Kind::Event) appendFallbackLocked(item.event); } else writtenSequence = qMax(writtenSequence, item.sequence()); completeLocked(item.sequence()); hasInFlight = false; if (flushRequested && processedSequence >= flushTarget) { flushNow = true; flushRequested = false; } if (stopping && queue.empty()) { flushNow = true; finish = true; flushRequested = false; } } progress.notify_all(); if (Clock::now() - lastFlush >= std::chrono::seconds(1)) flushNow = true; }
            if (flushNow) { QString error; const bool ok = flushFiles(&error); { std::lock_guard<std::mutex> guard(mutex); if (!ok) setWriteErrorLocked(error); else flushedSequence = processedSequence; } lastFlush = Clock::now(); progress.notify_all(); if (finish) break; }
        }
        { std::lock_guard<std::mutex> lock(mutex); canPinHistory = false; }
        pinPendingHistory();
        runtimeFile.close(); eventsFile.close(); networkHistoryFile.close(); settingsHistoryFile.close(); cardHistoryFile.close(); progress.notify_all();
    }
    bool flushTo(quint64 target, int timeoutMs)
    {
        { std::lock_guard<std::mutex> guard(mutex); if (!worker.joinable()) return flushedSequence >= target && !writeError; if (quotaPending) return false; flushRequested = true; flushTarget = qMax(flushTarget, target); } condition.notify_all(); std::unique_lock<std::mutex> lock(mutex); const auto ready = [this, target] { return flushedSequence >= target || writeError || !worker.joinable(); }; bool reached = timeoutMs < 0 ? (progress.wait(lock, ready), true) : progress.wait_for(lock, std::chrono::milliseconds(timeoutMs), ready); return reached && flushedSequence >= target && !writeError;
    }
    void requestFlush() { { std::lock_guard<std::mutex> guard(mutex); if (!worker.joinable()) return; flushRequested = true; flushTarget = qMax(flushTarget, nextSequence ? nextSequence - 1 : 0); } condition.notify_all(); }
    void writeInitialArtifacts() { writeRunArtifacts(0, true, QString()); }
    void loadLiveSnapshotDocuments(quint64 boundary, QJsonObject *network, QJsonObject *settings, QStringList *truncationReasons) const
    {
        ExportRequest request; { std::lock_guard<std::mutex> guard(mutex); request.runId = runId; request.sourceDirectory = runDirectory; request.boundarySequence = boundary; request.networkSnapshots = networkSnapshots; request.settingsSnapshots = settingsSnapshots; request.cardSnapshots = cardSnapshots; }
        const SnapshotDocuments documents = loadSnapshotDocuments(runDirectory, runId, boundary, request, truncationReasons); if (network) *network = documents.network; if (settings) *settings = documents.settings;
    }
    bool writeMetadata(const QString &name, const QByteArray &bytes, QString *error)
    {
        qint64 total = bytes.size();
        for (const QString &other : {QStringLiteral("network.json"), QStringLiteral("settings.json"), QStringLiteral("summary.txt"), QStringLiteral("manifest.json"), QStringLiteral("history-retention.json")})
            if (other != name) total += QFileInfo(QDir(runDirectory).filePath(other)).size();
        // Leave room for the mandatory final manifest even when optional documents are large.
        const qint64 limit = name == QStringLiteral("manifest.json") ? metadataLimit : qMax<qint64>(0, metadataLimit - 4096);
        if (total > limit) { if (error) *error = QStringLiteral("诊断元数据超过预留容量"); return false; }
        return writeAtomicBytes(QDir(runDirectory).filePath(name), bytes, error);
    }
    void writeRunArtifacts(quint64 boundary, bool activeState, const QString &note)
    {
        QStringList missing, truncationReasons; QJsonObject network, settings; loadLiveSnapshotDocuments(boundary, &network, &settings, &truncationReasons); QString error;
        if (!writeMetadata(QStringLiteral("network.json"), jsonBytes(network) + '\n', &error)) { missing.append(QStringLiteral("network.json")); setWriteError(error); }
        if (!writeMetadata(QStringLiteral("settings.json"), jsonBytes(settings) + '\n', &error)) { missing.append(QStringLiteral("settings.json")); setWriteError(error); }
        QVector<Event> fallback; DropCounters dropCopy; bool writeErrorCopy = false; QString writeErrorTextCopy, startCopy, endCopy;
        { std::lock_guard<std::mutex> guard(mutex); fallback = fallbackEvents; dropCopy = drops; writeErrorCopy = writeError; writeErrorTextCopy = writeErrorText; startCopy = startIsoTime; endCopy = endIsoTime; }
        appendRetentionReason(runDirectory,&truncationReasons);
        appendDropReasons(dropCopy, &truncationReasons); const FilteredEvents events = readFilteredEvents(runDirectory, boundary, fallback, &missing, &truncationReasons); const bool complete = missing.isEmpty() && truncationReasons.isEmpty() && !writeErrorCopy;
        if (!writeMetadata(QStringLiteral("summary.txt"), summaryBytes(runId, startCopy, endCopy, boundary, activeState, complete, writeErrorCopy, writeErrorTextCopy, dropCopy, network, settings, events.count, note, truncationReasons), &error)) { missing.append(QStringLiteral("summary.txt")); setWriteError(error); }
        { std::lock_guard<std::mutex> guard(mutex); writeErrorCopy = writeError; writeErrorTextCopy = writeErrorText; dropCopy = drops; }
        const QJsonObject manifest = manifestForFiles(runId, startCopy, endCopy, boundary, activeState, writeErrorCopy, writeErrorTextCopy, dropCopy, missing, truncationReasons, note, runDirectory);
        if (!writeMetadata(QStringLiteral("manifest.json"), jsonBytes(manifest) + '\n', &error)) setWriteError(error);
    }
    void appendRequestStateLocked(ExportRequest *request) const
    {
        if (!request) return;
        auto addEvent = [&](const Event &event) { if (event.sequence <= request->boundarySequence && std::none_of(request->fallbackEvents.cbegin(), request->fallbackEvents.cend(), [&](const Event &old) { return old.sequence == event.sequence; })) request->fallbackEvents.append(event); };
        for (const Event &event : fallbackEvents) addEvent(event);
        auto addItem = [&](const QueueItem &item) { if (!item.sequence() || item.sequence() > request->boundarySequence) return; switch (item.kind) { case QueueItem::Kind::Event: addEvent(item.event); break; case QueueItem::Kind::NetworkSnapshot: request->networkSnapshots.append(item.snapshot); break; case QueueItem::Kind::SettingsSnapshot: request->settingsSnapshots.append(item.snapshot); break; case QueueItem::Kind::CardSnapshot: request->cardSnapshots.append(item.card); break; } };
        for (const QueueItem &item : queue) addItem(item); if (hasInFlight) addItem(inFlight);
        request->drops = drops;
        request->writeError = writeError || quotaPending;
        request->writeErrorText = writeError ? writeErrorText : quotaError;
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
    void registerTimeWindowCut(TimeWindowRequest &request, qint64 start, qint64 end)
    {
        ExportRequest source;
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto runStart = QDateTime::fromString(startIsoTime, Qt::ISODateWithMs);
            const auto runEnd = QDateTime::fromString(active ? request.exportCapturedAt : endIsoTime, Qt::ISODateWithMs);
            if ((runStart.isValid() && runStart.toMSecsSinceEpoch() > end)
                || (runEnd.isValid() && runEnd.toMSecsSinceEpoch() < start)) return;
            source.runId = runId;
            source.sourceDirectory = runDirectory;
            source.startIsoTime = startIsoTime;
            source.endIsoTime = endIsoTime;
            source.sourceWasActive = active;
            source.boundarySequence = nextSequence - 1;
            source.capturedLive = true;
            source.pendingAtCapture = processedSequence < source.boundarySequence;
            source.writeError = writeError || quotaPending;
            source.writeErrorText = writeError ? writeErrorText : quotaError;
            source.drops = drops;
            if (rotationCut) request.pendingHistory = rotationCut;
            else if (canPinHistory) {
                pendingHistory.erase(std::remove_if(pendingHistory.begin(), pendingHistory.end(),
                    [](const auto &weak) { return weak.expired(); }), pendingHistory.end());
                if (pendingHistory.size() >= 32) {
                    source.captureError = QStringLiteral("诊断日志快照请求过多，请稍后重试");
                } else {
                    request.pendingHistory = std::make_shared<PendingHistoryCut>();
                    pendingHistory.emplace_back(request.pendingHistory);
                }
            } else if (active && writeError) {
                source.historySnapshot = std::make_shared<HistorySnapshot>();
            } else {
                source.captureError = QStringLiteral("诊断日志记录器正在关闭，请稍后重试");
            }
        }
        request.sources.append(std::move(source));
        condition.notify_one();
    }
    Status statusCopy() const { std::lock_guard<std::mutex> guard(mutex); Status result; result.active = active; result.writeError = writeError || quotaPending; result.writeErrorText = writeError ? writeErrorText : quotaError; result.nextSequence = nextSequence; result.writtenSequence = writtenSequence; result.drops = drops; result.queuedEntries = static_cast<int>(queue.size()); result.fallbackEntries = fallbackEvents.size(); return result; }

    Options options; QString rootDirectory, runId, runDirectory, activeMarkerPath, startIsoTime, endIsoTime; Clock::time_point monoStart; std::unique_ptr<QLockFile> activeLock;
    mutable std::mutex historyWriteMutex;
    mutable std::mutex mutex; std::condition_variable condition, progress; std::deque<QueueItem> queue; QVector<Event> fallbackEvents; QVector<Snapshot> networkSnapshots, settingsSnapshots; QVector<CardSnapshot> cardSnapshots; QHash<QString, NoiseBucket> noiseBuckets; QMap<quint64, quint64> completed; DropCounters drops; QueueItem inFlight; bool hasInFlight = false;
    QFile runtimeFile, eventsFile, networkHistoryFile, settingsHistoryFile, cardHistoryFile; std::thread worker; bool active = false, accepting = false, stopping = false, flushRequested = false, writeError = false, admitted = false, quotaPending = false; qint64 segmentLimit = 0, metadataLimit = 0; QString writeErrorText, quotaError; quint64 nextSequence = 1, writtenSequence = 0, processedSequence = 0, flushedSequence = 0, flushTarget = 0;
};

namespace {

qint64 isoMillisLocal(const QString &text)
{
    if (text.isEmpty()) return std::numeric_limits<qint64>::min();
    const QDateTime value = QDateTime::fromString(text, Qt::ISODateWithMs);
    return value.isValid() ? value.toMSecsSinceEpoch() : std::numeric_limits<qint64>::min();
}

QString isoFromMillis(qint64 value)
{
    return value == std::numeric_limits<qint64>::min()
        ? QString() : QDateTime::fromMSecsSinceEpoch(value).toString(Qt::ISODateWithMs);
}

struct JsonWindowScan {
    QByteArray lines;
    QJsonArray objects;
    int formalCount = 0;
    int contextCount = 0;
    qint64 earliest = std::numeric_limits<qint64>::max();
    qint64 latest = std::numeric_limits<qint64>::min();
    qint64 includedEarliest = std::numeric_limits<qint64>::max();
    qint64 includedLatest = std::numeric_limits<qint64>::min();
    bool exists = false;
    QStringList warnings;
};

QString scanWarnings(const JsonWindowScan &scan)
{
    return scan.warnings.join(QStringLiteral("；"));
}

QJsonObject tagWindowObject(const QJsonObject &input,
                            const QString &role,
                            const QString &runId,
                            const QString &directory)
{
    QJsonObject value = input;
    value.insert(QStringLiteral("windowRole"), role);
    value.insert(QStringLiteral("sourceRunId"), runId);
    value.insert(QStringLiteral("sourceRunDirectory"), directory);
    return value;
}

JsonWindowScan scanJsonHistoryFile(const QString &path,
                                   const QString &runId,
                                   const QString &directory,
                                   qint64 startMs,
                                   qint64 endMs,
                                   quint64 boundary,
                                   const QString &kind, const DiagnosticRecorder::HistorySnapshot *snapshot = nullptr)
{
    Q_UNUSED(kind);
    JsonWindowScan result;
    HistoryReader file(path, snapshot);
    if (!file.exists()) return result;
    result.exists = true;
    if (!file.open(QIODevice::ReadOnly)) {
        result.warnings.append(QStringLiteral("无法读取 %1：%2").arg(path, file.errorString()));
        return result;
    }

    QJsonObject before;
    QJsonObject after;
    qint64 beforeMs = std::numeric_limits<qint64>::min();
    qint64 afterMs = std::numeric_limits<qint64>::max();
    quint64 beforeSequence = 0;
    quint64 afterSequence = 0;
    while (!file.atEnd()) {
        const QByteArray line = file.readLine();
        if (line.trimmed().isEmpty()) continue;
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            result.warnings.append(QStringLiteral("%1 存在无法解析的记录").arg(path));
            continue;
        }
        const QJsonObject object = document.object();
        const quint64 sequence = static_cast<quint64>(object.value(QStringLiteral("sequence")).toDouble(0));
        if (boundary != 0 && sequence != 0 && sequence > boundary) continue;
        const qint64 timestamp = isoMillisLocal(object.value(QStringLiteral("timestamp")).toString());
        if (timestamp == std::numeric_limits<qint64>::min()) {
            result.warnings.append(QStringLiteral("%1 存在无效 timestamp").arg(path));
            continue;
        }
        result.earliest = qMin(result.earliest, timestamp);
        result.latest = qMax(result.latest, timestamp);
        if (timestamp >= startMs && timestamp <= endMs) {
            const QJsonObject tagged = tagWindowObject(object, QStringLiteral("formal"), runId, directory);
            result.lines += QJsonDocument(tagged).toJson(QJsonDocument::Compact) + '\n';
            result.objects.append(tagged);
            ++result.formalCount;
            result.includedEarliest = qMin(result.includedEarliest, timestamp);
            result.includedLatest = qMax(result.includedLatest, timestamp);
        }
        if (timestamp <= startMs && timestamp >= beforeMs) {
            before = object;
            beforeMs = timestamp;
            beforeSequence = sequence;
        }
        if (timestamp >= endMs && timestamp <= afterMs) {
            after = object;
            afterMs = timestamp;
            afterSequence = sequence;
        }
    }

    const auto appendContext = [&](const QJsonObject &object,
                                   quint64 sequence,
                                   const QString &role) {
        if (object.isEmpty()) return;
        const qint64 timestamp = isoMillisLocal(object.value(QStringLiteral("timestamp")).toString());
        if (timestamp >= startMs && timestamp <= endMs) return;
        const QJsonObject tagged = tagWindowObject(object, role, runId, directory);
        result.lines += QJsonDocument(tagged).toJson(QJsonDocument::Compact) + '\n';
        result.objects.append(tagged);
        result.includedEarliest = qMin(result.includedEarliest, timestamp);
        result.includedLatest = qMax(result.includedLatest, timestamp);
        Q_UNUSED(sequence);
        ++result.contextCount;
    };
    appendContext(before, beforeSequence, QStringLiteral("boundary-before"));
    if (afterSequence != beforeSequence)
        appendContext(after, afterSequence, QStringLiteral("boundary-after"));
    return result;
}

JsonWindowScan scanEventHistoryFile(const QString &path,
                                    const QString &runId,
                                    const QString &directory,
                                    qint64 startMs,
                                    qint64 endMs,
                                    quint64 boundary, const DiagnosticRecorder::HistorySnapshot *snapshot = nullptr)
{
    JsonWindowScan result;
    HistoryReader file(path, snapshot);
    if (!file.exists()) return result;
    result.exists = true;
    if (!file.open(QIODevice::ReadOnly)) {
        result.warnings.append(QStringLiteral("无法读取 %1：%2").arg(path, file.errorString()));
        return result;
    }
    while (!file.atEnd()) {
        const QByteArray line = file.readLine();
        if (line.trimmed().isEmpty()) continue;
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            result.warnings.append(QStringLiteral("%1 存在无法解析的记录").arg(path));
            continue;
        }
        const QJsonObject object = document.object();
        const quint64 sequence = static_cast<quint64>(object.value(QStringLiteral("sequence")).toDouble(0));
        if (boundary != 0 && sequence != 0 && sequence > boundary) continue;
        const qint64 timestamp = isoMillisLocal(object.value(QStringLiteral("timestamp")).toString());
        if (timestamp == std::numeric_limits<qint64>::min()) {
            result.warnings.append(QStringLiteral("%1 存在无效 timestamp").arg(path));
            continue;
        }
        result.earliest = qMin(result.earliest, timestamp);
        result.latest = qMax(result.latest, timestamp);
        if (timestamp < startMs || timestamp > endMs) continue;
        const QJsonObject tagged = tagWindowObject(object, QStringLiteral("formal"), runId, directory);
        result.lines += QJsonDocument(tagged).toJson(QJsonDocument::Compact) + '\n';
        result.objects.append(tagged);
        ++result.formalCount;
        result.includedEarliest = qMin(result.includedEarliest, timestamp);
        result.includedLatest = qMax(result.includedLatest, timestamp);
    }
    return result;
}

JsonWindowScan scanRuntimeFile(const QString &path,
                               qint64 startMs,
                               qint64 endMs,
                               quint64 boundary, const DiagnosticRecorder::HistorySnapshot *snapshot = nullptr)
{
    JsonWindowScan result;
    HistoryReader file(path, snapshot);
    if (!file.exists()) return result;
    result.exists = true;
    if (!file.open(QIODevice::ReadOnly)) {
        result.warnings.append(QStringLiteral("无法读取 %1：%2").arg(path, file.errorString()));
        return result;
    }
    const QRegularExpression expression(QStringLiteral("^\\[seq=(\\d+)\\] \\[(.*?)\\]"));
    while (!file.atEnd()) {
        const QByteArray line = file.readLine();
        const QRegularExpressionMatch match = expression.match(QString::fromUtf8(line));
        if (!match.hasMatch()) {
            result.warnings.append(QStringLiteral("%1 存在无序号记录").arg(path));
            continue;
        }
        bool ok = false;
        const quint64 sequence = match.captured(1).toULongLong(&ok);
        const qint64 timestamp = isoMillisLocal(match.captured(2));
        if (!ok || timestamp == std::numeric_limits<qint64>::min()) {
            result.warnings.append(QStringLiteral("%1 存在无效 timestamp").arg(path));
            continue;
        }
        if (boundary != 0 && sequence != 0 && sequence > boundary) continue;
        result.earliest = qMin(result.earliest, timestamp);
        result.latest = qMax(result.latest, timestamp);
        if (timestamp >= startMs && timestamp <= endMs) {
            result.lines += line;
            ++result.formalCount;
            result.includedEarliest = qMin(result.includedEarliest, timestamp);
            result.includedLatest = qMax(result.includedLatest, timestamp);
        }
    }
    return result;
}

} // namespace

DiagnosticRecorder *DiagnosticRecorder::instance() { std::lock_guard<std::mutex> guard(g_instanceMutex); return g_instance; }
DiagnosticRecorder::HistorySnapshot::~HistorySnapshot()
{
    std::lock_guard<std::mutex> lock(g_snapshotBudgetMutex);
    g_snapshotBytes -= reserved;
}

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
#ifdef PAIMAGE_DIAGNOSTIC_TEST_SEAM
void DiagnosticRecorder::setSnapshotReadHookForTest(std::function<void()> hook) { std::lock_guard<std::mutex> lock(g_snapshotHookMutex); g_snapshotReadHook = std::move(hook); }
#endif
void DiagnosticRecorder::setBundleCapture(BundleCapture capture){std::lock_guard<std::mutex> lock(g_bundleMutex);g_bundleCapture=std::move(capture);}
bool DiagnosticRecorder::flush(int timeoutMs) { if (!m_state) return false; const quint64 boundary = m_state->captureRequest(0, QString()).boundarySequence; return m_state->flushTo(boundary, timeoutMs); }
quint64 DiagnosticRecorder::captureBoundary() const { if (!m_state) return 0; std::lock_guard<std::mutex> lock(m_state->mutex); return m_state->nextSequence - 1; }
DiagnosticRecorder::ExportRequest DiagnosticRecorder::captureExportRequest(quint64 boundarySequence, const QString &note) const
{
    if (!m_state) return ExportRequest();
    ExportRequest request;
    {
        std::lock_guard<std::mutex> lock(m_state->historyWriteMutex);
        request = m_state->captureRequest(boundarySequence, note);
        { std::lock_guard<std::mutex> stateLock(m_state->mutex); request.pendingAtCapture = m_state->processedSequence < request.boundarySequence; }
        request.historySnapshot = captureHistory(request.sourceDirectory, &request.captureError);
    }
    request.capturedLive = true;
    request.bundleExtension=captureBundle(std::numeric_limits<qint64>::min(),QDateTime::currentMSecsSinceEpoch());
    request.flushBeforeExport = [state = m_state, boundary = request.boundarySequence] {
        return state->flushTo(boundary, 5000);
    };
    return request;
}

namespace {
void materializeTimeWindow(DiagnosticRecorder::TimeWindowRequest &result)
{
    if (!result.discoverSources || !result.validationError.isEmpty()) return;
    result.discoverSources = false;
    const qint64 startMs = isoMillisLocal(result.requestedStartTime);
    const qint64 endMs = isoMillisLocal(result.requestedEndTime);
    const qint64 capturedMs = isoMillisLocal(result.exportCapturedAt);
    const QFileInfo rootInfo(result.rootDirectory);
    if (rootInfo.isSymLink() || rootInfo.isJunction()) {
        result.validationError=QStringLiteral("不安全的日志目录：%1").arg(result.rootDirectory); return;
    }
    if(QDir(result.rootDirectory).exists()) {
        if (!safeLockFile(result.rootDirectory, QStringLiteral(".retention.lock"))) {
            result.validationError=QStringLiteral("不安全的日志锁文件"); return;
        }
        result.retentionPin=std::make_shared<DiagnosticRecorder::RetentionPin>();
        result.retentionPin->lease=diagnostic_storage::awaitExportLease(result.rootDirectory);
        if(!result.retentionPin->lease){result.validationError=QStringLiteral("日志目录正在清理，请重试导出");return;}
        // This copy can drop its intent once its filesystem lease protects the reads.
        result.exportIntent.reset();
    }
    if (result.pendingHistory) {
        result.pendingHistory->materialize(result.sources.first());
        result.pendingHistory.reset();
    }
    // Enumerate metadata only: scanning an active file before its capture races rotation.
    QVector<DiagnosticRecorder::RunInfo> runs;
    const QFileInfoList entries = QDir(result.rootDirectory).entryInfoList(
        {QStringLiteral("run-*")}, QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time);
    for (const QFileInfo &info : entries) {
        if (!safeDirectory(info.absoluteFilePath()) || !safeLockFile(info.absoluteFilePath(), QStringLiteral(".active.lock"))) { result.validationError=QStringLiteral("不安全的运行目录：%1").arg(info.filePath()); return; }
        DiagnosticRecorder::RunInfo run; run.directory=info.absoluteFilePath(); run.active=runIsActive(run.directory);
        QByteArray bytes;
        if (readAllBytes(QDir(run.directory).filePath(QStringLiteral("manifest.json")), &bytes)) {
            const QJsonObject manifest=parseObject(bytes);
            run.runId=manifest.value(QStringLiteral("runId")).toString();
            run.startIsoTime=manifest.value(QStringLiteral("startTime")).toString();
            run.endIsoTime=manifest.value(QStringLiteral("endTime")).toString();
            run.lastSequence=static_cast<quint64>(manifest.value(QStringLiteral("boundarySequence")).toDouble(0));
        }
        if (run.runId.isEmpty()) run.runId=info.fileName();
        runs.append(run);
    }
    const QString liveDirectory = result.sources.isEmpty() ? QString() : result.sources.first().sourceDirectory;
    for (const auto &run : runs) {
        const qint64 earliest = isoMillisLocal(run.earliestIsoTime.isEmpty()
                                                   ? run.startIsoTime : run.earliestIsoTime);
        qint64 latest = isoMillisLocal(run.latestIsoTime.isEmpty()
                                           ? run.endIsoTime : run.latestIsoTime);
        if (run.active || latest == std::numeric_limits<qint64>::min())
            latest = qMax(latest, capturedMs);
        const bool overlap = earliest == std::numeric_limits<qint64>::min()
            || latest == std::numeric_limits<qint64>::min()
            || (earliest <= endMs && latest >= startMs);
        if (!overlap || (earliest != std::numeric_limits<qint64>::min() && earliest > capturedMs)) continue;
        if (!liveDirectory.isEmpty()
            && QFileInfo(run.directory).absoluteFilePath()
                == QFileInfo(liveDirectory).absoluteFilePath()) {
            continue;
        }
        DiagnosticRecorder::ExportRequest source;
        source.runId = run.runId;
        source.sourceDirectory = run.directory;
        source.startIsoTime = run.startIsoTime;
        source.endIsoTime = run.endIsoTime;
        source.boundarySequence = run.lastSequence;
        source.sourceWasActive = run.active;
        source.note = result.note;
        if (run.active) {
            // A foreign writer can append while the rotation lease is held,
            // so there is no coherent cross-file sequence fence to copy.
            source.captureError=QStringLiteral("其他进程的活动运行暂不支持一致性快照：%1").arg(run.directory);
        } else {
            // Ended runs can have a stale manifest after an abnormal exit.
            source.boundarySequence=0;
        }
        result.sources.append(source);
    }
    bool hasHistorical = false;
    for (const auto &source : result.sources)
        if (!source.sourceWasActive && !source.capturedLive && source.captureError.isEmpty()) hasHistorical = true;
    if (!hasHistorical) result.retentionPin.reset();
}
} // namespace

DiagnosticRecorder::TimeWindowRequest DiagnosticRecorder::registerTimeWindowRequest(
    const QDateTime &startLocal,
    const QDateTime &endLocal,
    const QString &note) const
{
    TimeWindowRequest result;
    result.requestedStartTime = startLocal.toString(Qt::ISODateWithMs);
    result.requestedEndTime = endLocal.toString(Qt::ISODateWithMs);
    result.note = note;
    result.rootDirectory = m_state ? m_state->rootDirectory : QString();

    if (!startLocal.isValid() || !endLocal.isValid()) {
        result.validationError = QStringLiteral("时间窗无效");
        return result;
    }
    if (startLocal > endLocal) {
        result.validationError = QStringLiteral("开始时间必须早于或等于结束时间");
        return result;
    }

    if (!result.rootDirectory.isEmpty()) {
        result.exportIntent = diagnostic_storage::registerExportIntent(result.rootDirectory);
        if (!result.exportIntent) {
            result.validationError = QStringLiteral("无法保护待导出的历史日志，目录可能正在清理，请重试导出");
            return result;
        }
    }
    // Capture the extension first so both roots are protected before the recorder's timestamp.
    result.bundleExtension = captureBundle(startLocal.toMSecsSinceEpoch(), endLocal.toMSecsSinceEpoch());
    result.exportCapturedAt = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    result.discoverSources = true;
    if (m_state) m_state->registerTimeWindowCut(result, startLocal.toMSecsSinceEpoch(), endLocal.toMSecsSinceEpoch());
    return result;
}

DiagnosticRecorder::TimeWindowRequest DiagnosticRecorder::captureTimeWindowRequest(
    const QDateTime &startLocal, const QDateTime &endLocal, const QString &note) const
{
    auto request = registerTimeWindowRequest(startLocal, endLocal, note);
    materializeTimeWindow(request);
    return request;
}
DiagnosticRecorder::ExportResult DiagnosticRecorder::exportRun(const QString &targetZipPath, quint64 boundarySequence, const QString &note) { return exportRequest(captureExportRequest(boundarySequence, note), targetZipPath); }
std::future<DiagnosticRecorder::ExportResult> DiagnosticRecorder::exportRunAsync(const QString &targetZipPath, quint64 boundarySequence, const QString &note) { const ExportRequest request = captureExportRequest(boundarySequence, note); return std::async(std::launch::async, [request, targetZipPath] { return DiagnosticRecorder::exportRequest(request, targetZipPath); }); }

DiagnosticRecorder::ExportResult DiagnosticRecorder::exportRequest(const ExportRequest &request, const QString &targetZipPath)
{
    if (!request.captureError.isEmpty()) { ExportResult failed; failed.targetPath=targetZipPath; failed.error=request.captureError; return failed; }
    const auto root=QFileInfo(request.sourceDirectory).absolutePath();
    if (!request.historySnapshot && !request.sourceDirectory.isEmpty()
        && (!safeDirectory(root) || !safeDirectory(request.sourceDirectory)
            || !safeLockFile(root, QStringLiteral(".retention.lock"))
            || !safeLockFile(request.sourceDirectory, QStringLiteral(".active.lock")))) {
        ExportResult failed; failed.targetPath=targetZipPath; failed.error=QStringLiteral("不安全的日志目录或锁文件"); return failed;
    }
    const bool hasDiskSource=!request.historySnapshot && !request.sourceDirectory.isEmpty()&&QDir(root).exists();
    auto sourceLease=request.storageLease ? request.storageLease :
        (hasDiskSource ? diagnostic_storage::exportLease(root) : nullptr);
    if(hasDiskSource&&!sourceLease){ ExportResult failed;failed.targetPath=targetZipPath;
        failed.error=QStringLiteral("日志目录正在清理，请重试导出");return failed; }
    ExportRequest effective = request;
    QStringList sourceWarnings;
    appendRetentionReason(effective.sourceDirectory,&sourceWarnings,std::numeric_limits<qint64>::min(),effective.historySnapshot.get());
    if (effective.flushBeforeExport && !effective.flushBeforeExport())
        sourceWarnings.append(QStringLiteral("导出前刷新未完成或发生写入错误，已合并点击时的内存记录"));
    // Historical callers normally provide only RunInfo. Merge the persisted
    // manifest so drops, write failures, and the original run identity are
    // not lost when exporting after the recorder object has gone away.
    QByteArray manifestBytes;
    if (!request.capturedLive && !request.sourceDirectory.isEmpty()
        && capturedBytes(QDir(request.sourceDirectory).filePath(QStringLiteral("manifest.json")), request.historySnapshot.get(), &manifestBytes)) {
        if (!effective.historySnapshot) effective.sourceWasActive = runIsActive(effective.sourceDirectory);
        mergeRunManifest(effective, parseObject(manifestBytes), true, sourceWarnings);
    }
    if (!effective.capturedLive && !effective.historySnapshot && runIsActive(effective.sourceDirectory)) {
        ExportResult failed; failed.targetPath=targetZipPath; failed.error=QStringLiteral("活动运行需要先捕获快照，请重试导出"); return failed;
    }
    if (!effective.capturedLive && effective.boundarySequence == 0) effective.boundarySequence = maxSequenceOnDisk(effective.sourceDirectory);
    ExportResult result; result.targetPath = targetZipPath; result.boundarySequence = effective.boundarySequence;
    if (targetZipPath.isEmpty()) { result.error = QStringLiteral("target ZIP path is empty"); return result; }
    if (effective.sourceDirectory.isEmpty()) { result.error = QStringLiteral("source diagnostic directory is empty"); return result; }
    if (QFileInfo::exists(targetZipPath)) { result.error = QStringLiteral("目标文件已存在，请选择新的文件名"); return result; }
    QStringList missing, truncationReasons = sourceWarnings; if (effective.snapshotsTruncated) truncationReasons.append(QStringLiteral("内存快照已达到上限；若磁盘历史缺失，早期快照可能无法恢复")); appendDropReasons(effective.drops, &truncationReasons);
    const FilteredEvents events = readFilteredEvents(effective.sourceDirectory, effective.boundarySequence, effective.fallbackEvents, &missing, &truncationReasons, effective.historySnapshot.get());
    const SnapshotDocuments snapshots = loadSnapshotDocuments(effective.sourceDirectory, effective.runId, effective.boundarySequence, effective, &truncationReasons);
    const bool complete = missing.isEmpty() && truncationReasons.isEmpty() && !effective.writeError;
    const QByteArray summary = summaryBytes(effective.runId, effective.startIsoTime, effective.endIsoTime, effective.boundarySequence, effective.sourceWasActive, complete, effective.writeError, effective.writeErrorText, effective.drops, snapshots.network, snapshots.settings, events.count, effective.note, truncationReasons);
    const QStringList names = {QStringLiteral("runtime.log"), QStringLiteral("events.jsonl"), QStringLiteral("network.json"), QStringLiteral("settings.json"), QStringLiteral("summary.txt"), QStringLiteral("manifest.json")};
    QHash<QString, QByteArray> contents; contents.insert(QStringLiteral("runtime.log"), events.runtime); contents.insert(QStringLiteral("events.jsonl"), events.events); contents.insert(QStringLiteral("network.json"), jsonBytes(snapshots.network) + '\n'); contents.insert(QStringLiteral("settings.json"), jsonBytes(snapshots.settings) + '\n'); contents.insert(QStringLiteral("summary.txt"), summary);
    const QJsonObject manifest = manifestObject(effective.runId, effective.startIsoTime, effective.endIsoTime, effective.boundarySequence, effective.sourceWasActive, effective.writeError, effective.writeErrorText, effective.drops, missing, truncationReasons, effective.note, names, contents); contents.insert(QStringLiteral("manifest.json"), jsonBytes(manifest) + '\n');
    sourceLease.reset();
    const QFileInfo targetInfo(targetZipPath); if (!QDir().mkpath(targetInfo.absolutePath())) { result.error = QStringLiteral("create target directory failed: %1").arg(targetInfo.absolutePath()); result.missingFiles = missing; result.truncationReasons = truncationReasons; return result; }
    const QString temporaryPath = targetZipPath + QStringLiteral(".part-") + QUuid::createUuid().toString(QUuid::WithoutBraces); QString error; ZipWriter writer(temporaryPath); bool ok = writer.open(&error); if (ok) for (const QString &name : names) if (!writer.add(name, contents.value(name), &error)) { ok = false; break; } if(ok&&effective.bundleExtension)ok=effective.bundleExtension([&](const QString& name,const QByteArray& bytes){return writer.add(name,bytes,&error);},&error); if (ok) ok = writer.close(&error);
    if (!ok) { writer.cancel(); QFile::remove(temporaryPath); result.error = error; result.missingFiles = missing; result.truncationReasons = truncationReasons; return result; }
    if (!QFile::rename(temporaryPath, targetZipPath)) { QFile::remove(temporaryPath); result.error = QStringLiteral("publish target ZIP failed"); return result; }
    result.success = true; result.missingFiles = missing; result.truncationReasons = truncationReasons; return result;
}

DiagnosticRecorder::ExportResult DiagnosticRecorder::exportTimeWindow(
    const TimeWindowRequest &registered,
    const QString &targetZipPath)
{
    auto request = registered;
    materializeTimeWindow(request);
    ExportResult result;
    result.targetPath = targetZipPath;
    if (!request.validationError.isEmpty()) {
        result.error = request.validationError;
        return result;
    }
    if (targetZipPath.isEmpty()) {
        result.error = QStringLiteral("target ZIP path is empty");
        return result;
    }
    const QFileInfo requestRoot(request.rootDirectory);
    if (!request.rootDirectory.isEmpty() && (requestRoot.isSymLink() || requestRoot.isJunction()
        || (requestRoot.isDir() && !safeLockFile(request.rootDirectory, QStringLiteral(".retention.lock"))))) {
        result.error=QStringLiteral("不安全的日志目录或锁文件"); return result;
    }
    const bool hasRoot = !request.rootDirectory.isEmpty() && QDir(request.rootDirectory).exists();
    auto sourceLease = request.retentionPin ? request.retentionPin->take() : request.storageLease;
    bool needsHistory = false;
    for (const ExportRequest &source : request.sources)
        if (!source.historySnapshot && source.captureError.isEmpty()) needsHistory = true;
    if (!sourceLease && hasRoot && needsHistory) sourceLease=diagnostic_storage::exportLease(request.rootDirectory);
    if (hasRoot && needsHistory && !sourceLease) {
        result.error=QStringLiteral("日志目录正在清理，请重试导出"); return result;
    }
    if (QFileInfo::exists(targetZipPath)) {
        result.error = QStringLiteral("目标文件已存在，请选择新的文件名");
        return result;
    }

    const qint64 startMs = isoMillisLocal(request.requestedStartTime);
    const qint64 endMs = isoMillisLocal(request.requestedEndTime);
    if (startMs == std::numeric_limits<qint64>::min()
        || endMs == std::numeric_limits<qint64>::min() || startMs > endMs) {
        result.error = QStringLiteral("时间窗无效");
        return result;
    }

    QByteArray runtime;
    QByteArray events;
    QByteArray networkHistory;
    QByteArray settingsHistory;
    QByteArray cardHistory;
    QJsonArray networkObjects;
    QJsonArray settingsObjects;
    QJsonArray cardObjects;
    QStringList missing;
    QStringList truncationReasons;
    QSet<QString> sourceIds;
    QJsonArray activeSources;
    QJsonArray incompleteSources;
    QJsonArray sourceRunIntegrity;
    DiagnosticRecorder::DropCounters drops;
    bool anyWriteError = false;
    QStringList writeErrors;
    int formalEvents = 0;
    int formalRuntime = 0;
    int formalNetwork = 0;
    int formalSettings = 0;
    int formalCards = 0;
    int contextNetwork = 0;
    int contextSettings = 0;
    int contextCards = 0;
    qint64 includedEarliest = std::numeric_limits<qint64>::max();
    qint64 includedLatest = std::numeric_limits<qint64>::min();

    const auto updateIncludedBounds = [&](const JsonWindowScan &scan) {
        if (scan.includedEarliest != std::numeric_limits<qint64>::max())
            includedEarliest = qMin(includedEarliest, scan.includedEarliest);
        if (scan.includedLatest != std::numeric_limits<qint64>::min())
            includedLatest = qMax(includedLatest, scan.includedLatest);
    };
    const auto appendWarnings = [&](const JsonWindowScan &scan) {
        truncationReasons.append(scanWarnings(scan));
    };
    const auto mergeDrops = [&](const DiagnosticRecorder::DropCounters &value) {
        drops.queueDropped += value.queueDropped;
        drops.criticalQueueDropped += value.criticalQueueDropped;
        drops.noiseDropped += value.noiseDropped;
        drops.writeFallbackDropped += value.writeFallbackDropped;
    };

    for (const ExportRequest &originalSource : request.sources) {
        ExportRequest source = originalSource;
        if (!source.captureError.isEmpty()) {
            if (source.capturedLive) { result.error=source.captureError; return result; }
            if (sourceIds.contains(source.runId)) continue;
            sourceIds.insert(source.runId);
            incompleteSources.append(source.runId);
            if (source.sourceWasActive) activeSources.append(source.runId);
            truncationReasons.append(source.captureError);
            continue;
        }
        if (source.runId.isEmpty() || source.sourceDirectory.isEmpty() || sourceIds.contains(source.runId)) continue;
        if (!source.historySnapshot && (!safeDirectory(QFileInfo(source.sourceDirectory).absolutePath())
            || !safeDirectory(source.sourceDirectory)
            || !safeLockFile(source.sourceDirectory, QStringLiteral(".active.lock"))
            || !safeLockFile(source.sourceDirectory, QStringLiteral(".history.lock")))) {
            result.error=QStringLiteral("不安全的日志目录或锁文件：%1").arg(source.sourceDirectory); return result;
        }
        const bool mutableSource = !source.historySnapshot
            && (source.sourceWasActive || runIsActive(source.sourceDirectory));
        if (mutableSource) {
            if (!source.boundarySequence || !source.flushBeforeExport) {
                result.error=QStringLiteral("活动运行缺少明确的序号边界或刷新回调，无法安全导出"); return result;
            }
            // Flush before taking the per-run rotation lease; the callback can
            // wait for a writer that itself needs this lock.
            if (!source.flushBeforeExport())
                truncationReasons.append(QStringLiteral("运行 %1 导出前刷新未完成或发生写入错误").arg(source.runId));
            source.flushBeforeExport = {};
            {
                auto lease=diagnostic_storage::historyLease(source.sourceDirectory, 0);
                if (!lease) { result.error=QStringLiteral("活动运行正在轮转，请重试导出：%1").arg(source.sourceDirectory); return result; }
                source.historySnapshot=captureHistory(source.sourceDirectory, &source.captureError, std::move(lease));
            }
            if (!source.historySnapshot) { result.error=source.captureError; return result; }
            truncationReasons.append(QStringLiteral("运行 %1 使用调用方提供的序号边界及活动文件固定前缀；跨进程文件间一致性无法验证").arg(source.runId));
        }
        sourceIds.insert(source.runId);
        QStringList runReasons;
        bool persistedIncomplete = false;
        if (!source.capturedLive && !mutableSource && !source.sourceWasActive) {
            QByteArray bytes;
            if (capturedBytes(QDir(source.sourceDirectory).filePath(QStringLiteral("manifest.json")),
                              source.historySnapshot.get(), &bytes)) {
                const auto manifest = parseObject(bytes);
                mergeRunManifest(source, manifest, false, runReasons);
                persistedIncomplete = manifest.value(QStringLiteral("complete")).isBool() &&
                                      !manifest.value(QStringLiteral("complete")).toBool();
            }
        }
        appendDropReasons(source.drops, &runReasons);
        if (source.writeError)
            runReasons.append(QStringLiteral("该运行曾发生日志写入错误：%1").arg(source.writeErrorText));
        runReasons.removeDuplicates();
        for (const auto &reason : runReasons)
            truncationReasons.append(QStringLiteral("运行 %1（整次运行，窗口内精确损失数量未知）：%2").arg(source.runId, reason));
        sourceRunIntegrity.append(QJsonObject{
            {QStringLiteral("runId"), source.runId},
            {QStringLiteral("dropCounters"), dropCountersObject(source.drops)},
            {QStringLiteral("writeError"), source.writeError},
            {QStringLiteral("writeErrorText"), source.writeErrorText},
            {QStringLiteral("truncationReasons"), QJsonArray::fromStringList(runReasons)}});
        appendRetentionReason(source.sourceDirectory,&truncationReasons,startMs,source.historySnapshot.get());
        if (source.sourceWasActive)
            activeSources.append(source.runId);
        if ((!source.sourceWasActive && source.endIsoTime.isEmpty()) || persistedIncomplete || !runReasons.isEmpty())
            incompleteSources.append(source.runId);

        if (source.pendingAtCapture)
            truncationReasons.append(QStringLiteral("运行 %1 点击时仍有待处理记录，正式历史仅包含捕获时已写入的前缀").arg(source.runId));
        if (source.flushBeforeExport && !source.flushBeforeExport())
            truncationReasons.append(QStringLiteral("运行 %1 导出前刷新未完成或发生写入错误").arg(source.runId));
        mergeDrops(source.drops);
        anyWriteError = anyWriteError || source.writeError;
        if (!source.writeErrorText.isEmpty())
            writeErrors.append(QStringLiteral("%1: %2").arg(source.runId, source.writeErrorText));

        if (source.capturedLive && source.boundarySequence == 0) continue;

        const JsonWindowScan eventScan = scanEventHistoryFile(
            QDir(source.sourceDirectory).filePath(QStringLiteral("events.jsonl")),
            source.runId, source.sourceDirectory, startMs, endMs, source.boundarySequence, source.historySnapshot.get());
        const JsonWindowScan runtimeScan = scanRuntimeFile(
            QDir(source.sourceDirectory).filePath(QStringLiteral("runtime.log")),
            startMs, endMs, source.boundarySequence, source.historySnapshot.get());
        const JsonWindowScan networkScan = scanJsonHistoryFile(
            QDir(source.sourceDirectory).filePath(QStringLiteral("network_history.jsonl")),
            source.runId, source.sourceDirectory, startMs, endMs, source.boundarySequence,
            QStringLiteral("network"), source.historySnapshot.get());
        const JsonWindowScan settingsScan = scanJsonHistoryFile(
            QDir(source.sourceDirectory).filePath(QStringLiteral("settings_history.jsonl")),
            source.runId, source.sourceDirectory, startMs, endMs, source.boundarySequence,
            QStringLiteral("settings"), source.historySnapshot.get());
        const JsonWindowScan cardScan = scanJsonHistoryFile(
            QDir(source.sourceDirectory).filePath(QStringLiteral("card_history.jsonl")),
            source.runId, source.sourceDirectory, startMs, endMs, source.boundarySequence,
            QStringLiteral("card"), source.historySnapshot.get());

        const auto requireFile = [&](const JsonWindowScan &scan, const QString &name) {
            if (!scan.exists)
                missing.append(QStringLiteral("%1/%2").arg(source.runId, name));
        };
        requireFile(eventScan, QStringLiteral("events.jsonl"));
        requireFile(runtimeScan, QStringLiteral("runtime.log"));
        requireFile(networkScan, QStringLiteral("network_history.jsonl"));
        requireFile(settingsScan, QStringLiteral("settings_history.jsonl"));
        requireFile(cardScan, QStringLiteral("card_history.jsonl"));
        appendWarnings(eventScan);
        appendWarnings(runtimeScan);
        appendWarnings(networkScan);
        appendWarnings(settingsScan);
        appendWarnings(cardScan);

        events += eventScan.lines;
        runtime += runtimeScan.lines;
        networkHistory += networkScan.lines;
        settingsHistory += settingsScan.lines;
        cardHistory += cardScan.lines;
        for (const QJsonValue &value : networkScan.objects) networkObjects.append(value);
        for (const QJsonValue &value : settingsScan.objects) settingsObjects.append(value);
        for (const QJsonValue &value : cardScan.objects) cardObjects.append(value);
        formalEvents += eventScan.formalCount;
        formalRuntime += runtimeScan.formalCount;
        formalNetwork += networkScan.formalCount;
        formalSettings += settingsScan.formalCount;
        formalCards += cardScan.formalCount;
        contextNetwork += networkScan.contextCount;
        contextSettings += settingsScan.contextCount;
        contextCards += cardScan.contextCount;
        updateIncludedBounds(eventScan);
        updateIncludedBounds(runtimeScan);
        updateIncludedBounds(networkScan);
        updateIncludedBounds(settingsScan);
        updateIncludedBounds(cardScan);
    }

    sourceLease.reset();
    result.sourceRunCount = sourceIds.size();
    result.noRecords = formalEvents == 0 && formalRuntime == 0
        && formalNetwork == 0 && formalSettings == 0 && formalCards == 0;
    result.formalWindowRecordCounts = QJsonObject{
        {QStringLiteral("events"), formalEvents},
        {QStringLiteral("runtime"), formalRuntime},
        {QStringLiteral("network"), formalNetwork},
        {QStringLiteral("settings"), formalSettings},
        {QStringLiteral("card"), formalCards}
    };
    result.boundaryContextCounts = QJsonObject{
        {QStringLiteral("network"), contextNetwork},
        {QStringLiteral("settings"), contextSettings},
        {QStringLiteral("card"), contextCards}
    };

    QJsonObject networkDocument = makeSnapshotDocument(
        QStringLiteral("time-window"), 0, networkObjects, cardObjects);
    networkDocument.insert(QStringLiteral("requestedStartTime"), request.requestedStartTime);
    networkDocument.insert(QStringLiteral("requestedEndTime"), request.requestedEndTime);
    QJsonObject settingsDocument = makeSnapshotDocument(
        QStringLiteral("time-window"), 0, settingsObjects);
    settingsDocument.insert(QStringLiteral("requestedStartTime"), request.requestedStartTime);
    settingsDocument.insert(QStringLiteral("requestedEndTime"), request.requestedEndTime);

    QStringList summaryLines;
    summaryLines << QStringLiteral("MC410T 时间窗诊断导出")
                 << QStringLiteral("正式时间窗：[ %1, %2 ]").arg(request.requestedStartTime, request.requestedEndTime)
                 << QStringLiteral("导出捕获时间：%1").arg(request.exportCapturedAt)
                 << QStringLiteral("源 run 数：%1").arg(result.sourceRunCount)
                 << QStringLiteral("正式记录：events=%1, runtime=%2, network=%3, settings=%4, card=%5")
                       .arg(formalEvents).arg(formalRuntime).arg(formalNetwork)
                       .arg(formalSettings).arg(formalCards)
                 << QStringLiteral("窗口外边界上下文：network=%1, settings=%2, card=%3")
                       .arg(contextNetwork).arg(contextSettings).arg(contextCards);
    if (result.noRecords)
        summaryLines << QStringLiteral("该时间段没有日志（没有正式时间窗记录）。");
    if (!request.note.isEmpty()) summaryLines << QStringLiteral("现场说明：%1").arg(request.note);
    if (!missing.isEmpty()) summaryLines << QStringLiteral("缺失文件：%1").arg(missing.join(QStringLiteral("；")));
    if (!truncationReasons.isEmpty())
        summaryLines << QStringLiteral("完整性说明：%1").arg(truncationReasons.join(QStringLiteral("；")));

    QJsonObject manifest;
    manifest.insert(QStringLiteral("formatVersion"), 2);
    manifest.insert(QStringLiteral("exportType"), QStringLiteral("time-window"));
    manifest.insert(QStringLiteral("requestedStartTime"), request.requestedStartTime);
    manifest.insert(QStringLiteral("requestedEndTime"), request.requestedEndTime);
    manifest.insert(QStringLiteral("exportCapturedAt"), request.exportCapturedAt);
    QJsonArray sourceRunIds;
    for (const QString &id : sourceIds) sourceRunIds.append(id);
    manifest.insert(QStringLiteral("sourceRunIds"), sourceRunIds);
    manifest.insert(QStringLiteral("sourceRunCount"), result.sourceRunCount);
    manifest.insert(QStringLiteral("formalWindowRecordCounts"), result.formalWindowRecordCounts);
    manifest.insert(QStringLiteral("boundaryContextCounts"), result.boundaryContextCounts);
    manifest.insert(QStringLiteral("actualEarliestIncludedTime"),
                    isoFromMillis(includedEarliest));
    manifest.insert(QStringLiteral("actualLatestIncludedTime"),
                    isoFromMillis(includedLatest));
    manifest.insert(QStringLiteral("activeSourceRunIds"), activeSources);
    manifest.insert(QStringLiteral("incompleteSourceRunIds"), incompleteSources);
    manifest.insert(QStringLiteral("sourceRunIntegrity"), sourceRunIntegrity);
    manifest.insert(QStringLiteral("recorderDropCountersScope"), QStringLiteral("source-runs; exact window loss unknown"));
    manifest.insert(QStringLiteral("missingFiles"), QJsonArray::fromStringList(missing));
    manifest.insert(QStringLiteral("truncationReasons"), QJsonArray::fromStringList(truncationReasons));
    manifest.insert(QStringLiteral("recorderWriteError"), anyWriteError);
    if (!writeErrors.isEmpty()) manifest.insert(QStringLiteral("recorderWriteErrorText"), writeErrors.join(QStringLiteral("; ")));
    manifest.insert(QStringLiteral("recorderDropCounters"), dropCountersObject(drops));
    manifest.insert(QStringLiteral("note"), request.note);

    QHash<QString, QByteArray> contents;
    contents.insert(QStringLiteral("runtime.log"), runtime);
    contents.insert(QStringLiteral("events.jsonl"), events);
    contents.insert(QStringLiteral("network_history.jsonl"), networkHistory);
    contents.insert(QStringLiteral("settings_history.jsonl"), settingsHistory);
    contents.insert(QStringLiteral("card_history.jsonl"), cardHistory);
    contents.insert(QStringLiteral("network.json"), jsonBytes(networkDocument) + '\n');
    contents.insert(QStringLiteral("settings.json"), jsonBytes(settingsDocument) + '\n');
    contents.insert(QStringLiteral("summary.txt"), (summaryLines.join(QStringLiteral("\n")) + '\n').toUtf8());
    contents.insert(QStringLiteral("manifest.json"), jsonBytes(manifest) + '\n');

    const QFileInfo targetInfo(targetZipPath);
    if (!QDir().mkpath(targetInfo.absolutePath())) {
        result.error = QStringLiteral("create target directory failed: %1").arg(targetInfo.absolutePath());
        result.missingFiles = missing;
        result.truncationReasons = truncationReasons;
        return result;
    }
    const QString temporaryPath = targetZipPath + QStringLiteral(".part-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QString error;
    ZipWriter writer(temporaryPath);
    bool ok = writer.open(&error);
    for (auto it = contents.constBegin(); ok && it != contents.constEnd(); ++it)
        ok = writer.add(it.key(), it.value(), &error);
    if(ok&&request.bundleExtension)ok=request.bundleExtension([&](const QString& name,const QByteArray& bytes){return writer.add(name,bytes,&error);},&error);
    if (ok) ok = writer.close(&error);
    if (!ok) {
        writer.cancel();
        QFile::remove(temporaryPath);
        result.error = error;
        result.missingFiles = missing;
        result.truncationReasons = truncationReasons;
        return result;
    }
    if (!QFile::rename(temporaryPath, targetZipPath)) {
        QFile::remove(temporaryPath);
        result.error = QStringLiteral("publish target ZIP failed");
        return result;
    }
    result.success = true;
    result.missingFiles = missing;
    result.truncationReasons = truncationReasons;
    return result;
}

std::future<DiagnosticRecorder::ExportResult> DiagnosticRecorder::exportTimeWindowAsync(
    const TimeWindowRequest &request,
    const QString &targetZipPath) const
{
    return std::async(std::launch::async, [request, targetZipPath] {
        return DiagnosticRecorder::exportTimeWindow(request, targetZipPath);
    });
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
        const JsonWindowScan eventBounds = scanEventHistoryFile(
            QDir(run.directory).filePath(QStringLiteral("events.jsonl")),
            run.runId, run.directory,
            std::numeric_limits<qint64>::min(),
            std::numeric_limits<qint64>::max(), 0);
        run.earliestIsoTime = isoFromMillis(eventBounds.includedEarliest);
        run.latestIsoTime = isoFromMillis(eventBounds.includedLatest);
        if (run.earliestIsoTime.isEmpty()) run.earliestIsoTime = run.startIsoTime;
        if (run.latestIsoTime.isEmpty()) run.latestIsoTime = run.endIsoTime;
        if (run.runId.isEmpty()) run.runId = info.fileName(); result.append(run);
    }
    return result;
}

QString DiagnosticRecorder::defaultRootDirectory() { return defaultRootDirectoryLocal(); }
QString DiagnosticRecorder::severityName(Severity severity) { return severityNameLocal(severity); }
