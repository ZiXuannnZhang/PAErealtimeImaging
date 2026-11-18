#pragma once

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QRegularExpression>
#include <QThread>
#include <QUuid>
#include <algorithm>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <vector>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace diagnostic_storage {

// Root locks serialize background reads and quota accounting. This separate
// gate only orders click-time intent against the destructive part of pruning.
// A click never waits or touches the filesystem. On Windows, kernel handles
// protect the interval across application processes and are reclaimed on exit.
#ifdef Q_OS_WIN
namespace detail {
inline QString retentionGateName(const QString &root)
{
    const auto path = QDir::cleanPath(QDir(root).absolutePath()).toCaseFolded();
    return QStringLiteral("Global\\PAimageDiagnosticRetention-")
        + QString::fromLatin1(QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha256).toHex());
}

class NativeHandle {
public:
    explicit NativeHandle(HANDLE handle) : m_handle(handle) {}
    ~NativeHandle() { if (m_handle) CloseHandle(m_handle); }
    NativeHandle(const NativeHandle &) = delete;
    NativeHandle &operator=(const NativeHandle &) = delete;
    HANDLE get() const { return m_handle; }
private:
    HANDLE m_handle;
};
} // namespace detail

class ExportIntent : public detail::NativeHandle {
public:
    using NativeHandle::NativeHandle;
};

class PrunePermit : public detail::NativeHandle {
public:
    explicit PrunePermit(const QString &name)
        : NativeHandle(CreateMutexW(nullptr, FALSE, reinterpret_cast<LPCWSTR>(name.utf16())))
    {
        if (!get()) return;
        const auto result = WaitForSingleObject(get(), 0);
        m_locked = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
    }
    ~PrunePermit() { if (m_locked) ReleaseMutex(get()); }
    bool locked() const { return m_locked; }
private:
    bool m_locked = false;
};

inline std::shared_ptr<ExportIntent> registerExportIntent(const QString &root)
{
    if (root.isEmpty()) return {};
    const auto name = detail::retentionGateName(root);
    PrunePermit gate(name);
    if (!gate.locked()) return {};
    const auto intentName = name + QStringLiteral("-intent");
    auto intent = std::make_shared<ExportIntent>(CreateEventW(nullptr, TRUE, FALSE,
        reinterpret_cast<LPCWSTR>(intentName.utf16())));
    return intent->get() ? intent : nullptr;
}

// Acquire and destroy on the cleanup thread. The caller already holds the
// root file lease; the click path never acquires that lease under this gate.
inline std::shared_ptr<PrunePermit> tryPrune(const QString &root)
{
    const auto name = detail::retentionGateName(root);
    auto gate = std::make_shared<PrunePermit>(name);
    if (!gate->locked()) return {};
    const auto intentName = name + QStringLiteral("-intent");
    detail::NativeHandle intent(OpenEventW(SYNCHRONIZE, FALSE,
        reinterpret_cast<LPCWSTR>(intentName.utf16())));
    return !intent.get() && GetLastError() == ERROR_FILE_NOT_FOUND ? gate : nullptr;
}
#else
// Non-Windows tools use the same ordering in-process. The receiver itself
// ships on Windows; cross-process cleanup on other platforms is unsupported.
namespace detail {
struct RetentionGate {
    std::mutex mutex;
    std::weak_ptr<int> intent;
};
inline std::shared_ptr<RetentionGate> retentionGate(const QString &root)
{
    static std::mutex mutex;
    static std::map<QString, std::weak_ptr<RetentionGate>> gates;
    std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);
    if (!lock.owns_lock()) return {};
    for (auto it = gates.begin(); it != gates.end();)
        if (it->second.expired()) it = gates.erase(it); else ++it;
    auto &entry = gates[QDir::cleanPath(QDir(root).absolutePath())];
    auto gate = entry.lock();
    if (!gate) entry = gate = std::make_shared<RetentionGate>();
    return gate;
}
} // namespace detail
struct ExportIntent {
    std::shared_ptr<detail::RetentionGate> gate;
    std::shared_ptr<int> marker;
};
struct PrunePermit {
    explicit PrunePermit(std::shared_ptr<detail::RetentionGate> state)
        : gate(std::move(state)), lock(gate->mutex, std::try_to_lock) {}
    std::shared_ptr<detail::RetentionGate> gate;
    std::unique_lock<std::mutex> lock;
};
inline std::shared_ptr<ExportIntent> registerExportIntent(const QString &root)
{
    const auto gate = detail::retentionGate(root);
    if (!gate) return {};
    std::unique_lock<std::mutex> lock(gate->mutex, std::try_to_lock);
    if (!lock.owns_lock()) return {};
    auto marker = gate->intent.lock();
    if (!marker) gate->intent = marker = std::make_shared<int>();
    return std::make_shared<ExportIntent>(ExportIntent{gate, marker});
}
inline std::shared_ptr<PrunePermit> tryPrune(const QString &root)
{
    const auto gate = detail::retentionGate(root);
    if (!gate) return {};
    auto permit = std::make_shared<PrunePermit>(gate);
    return permit->lock.owns_lock() && gate->intent.expired() ? permit : nullptr;
}
#endif

// A captured export keeps this lease until its file reads finish. Cleanup
// never waits for an exporter or follows links outside the managed root.
inline std::shared_ptr<QLockFile> lease(const QString &root)
{
    auto lock = std::make_shared<QLockFile>(QDir(root).filePath(QStringLiteral(".retention.lock")));
    lock->setStaleLockTime(0);
    return lock->tryLock(0) ? lock : nullptr;
}

// Serialize a bounded snapshot of one run with rotation across processes.
// The lease must not be held while waiting for a writer or producing an archive.
inline std::shared_ptr<QLockFile> historyLease(const QString &runDirectory, int timeoutMs)
{
    const QFileInfo directory(runDirectory);
    const QFileInfo file(QDir(runDirectory).filePath(QStringLiteral(".history.lock")));
    if (!directory.isDir() || directory.isSymLink() || directory.isJunction()
        || file.isSymLink() || file.isJunction() || (file.exists() && !file.isFile())) return nullptr;
    auto lock = std::make_shared<QLockFile>(file.filePath());
    lock->setStaleLockTime(0);
    return lock->tryLock(timeoutMs) ? lock : nullptr;
}

inline std::shared_ptr<QLockFile> exportLease(const QString &root)
{
    static std::mutex mutex;
    static std::map<QString, std::weak_ptr<QLockFile>> leases;
    std::lock_guard<std::mutex> guard(mutex);
    for (auto it = leases.begin(); it != leases.end();) {
        if (it->second.expired()) it = leases.erase(it);
        else ++it;
    }
    const QString path = QDir(root).absolutePath();
    if (auto existing = leases[path].lock()) return existing;
    auto acquired = lease(path);
    leases[path] = acquired;
    return acquired;
}

// Called only after registration, off the click path. Do not hold the lease
// registry mutex across a wait, including when exports use different roots.
inline std::shared_ptr<QLockFile> awaitExportLease(const QString &root)
{
    QElapsedTimer elapsed;
    elapsed.start();
    do {
        if (auto acquired = exportLease(root)) return acquired;
        QThread::msleep(20);
    } while (elapsed.elapsed() < 2500);
    return {};
}

inline constexpr qint64 MiB = 1024 * 1024;
inline constexpr qint64 TraceQuota = 2048 * MiB;
inline constexpr qint64 DetailedReserve = 1536 * MiB;
inline constexpr int RetainDays = 7;

inline std::optional<qint64> pruneTraces(const QString &path, qint64 reserve = 0,
                                        qint64 quota = TraceQuota, int retainDays = RetainDays)
{
    QDir root(path);
    if (!root.exists() || QFileInfo(root.absolutePath()).isSymLink()
        || QFileInfo(root.absolutePath()).isJunction()) return std::nullopt;
    const auto guard = lease(root.absolutePath());
    if (!guard) return std::nullopt;
    struct Run { QString path; QDateTime updated; qint64 bytes; };
    std::vector<Run> runs;
    qint64 total = 0;
    static const QRegularExpression known(QStringLiteral(
        "^(run-config\\.json|(?:trace|timing|looplog)-summary\\.json|"
        "(?:trace|timing|looplog|looplog-window)-[0-9]+\\.bin|"
        "looplog-window-[0-9]+\\.bin\\.part|\\.active\\.lock)$"));
    for (const auto &dir : root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (dir.isSymLink() || dir.isJunction() || QUuid(dir.fileName()).isNull()) continue;
        const QString runPath = dir.absoluteFilePath();
        const auto children = QDir(runPath).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
        qint64 bytes = 0;
        QDateTime updated;
        bool owned = true;
        for (const auto &child : children) {
            if (!child.isFile() || child.isSymLink() || child.isJunction() || !known.match(child.fileName()).hasMatch()) {
                owned = false; break;
            }
            bytes += child.size();
            if (child.fileName() != QStringLiteral(".active.lock"))
                updated = qMax(updated, child.lastModified());
        }
        if (!owned) continue;
        QFile config(QDir(runPath).filePath(QStringLiteral("run-config.json")));
        if (!config.open(QIODevice::ReadOnly)) continue;
        const auto metadata = QJsonDocument::fromJson(config.read(64 * 1024)).object();
        if (metadata.value(QStringLiteral("runId")).toString() != dir.fileName()
            || !metadata.value(QStringLiteral("backendId")).toString().startsWith(QStringLiteral("paimage-"))) continue;
        total += bytes;
        runs.push_back({runPath, updated, bytes});
    }
    std::sort(runs.begin(), runs.end(), [](const Run &a, const Run &b) { return a.updated < b.updated; });
    const auto cutoff = QDateTime::currentDateTimeUtc().addDays(-retainDays);
    auto remaining = runs.size();
    for (const auto &run : runs) {
        if (run.updated >= cutoff && total <= qMax<qint64>(0, quota - reserve) && remaining <= 64) break;
        QDir dir(run.path);
        QLockFile active(dir.filePath(QStringLiteral(".active.lock")));
        active.setStaleLockTime(0);
        if (!active.tryLock(0)) continue;
        // Only owned files were enumerated; rmdir refuses new/unexpected files.
        bool removed = true;
        const auto entries = dir.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
        for (const auto &entry : entries) {
            if (entry.fileName() == QStringLiteral(".active.lock")) continue;
            if (!entry.isFile() || entry.isSymLink() || entry.isJunction() ||
                !known.match(entry.fileName()).hasMatch()) { removed = false; break; }
        }
        if (removed) {
            const auto permit = tryPrune(root.absolutePath());
            if (!permit) break;
            for (const auto &entry : entries) {
                if (entry.fileName() != QStringLiteral(".active.lock") &&
                    entry.fileName() != QStringLiteral("run-config.json") &&
                    !QFile::remove(entry.absoluteFilePath())) { removed = false; break; }
            }
            if (removed) removed = dir.remove(QStringLiteral("run-config.json"));
            active.unlock();
            if (removed && root.rmdir(dir.dirName())) { total -= run.bytes; --remaining; }
        }
    }
    return total;
}
}
