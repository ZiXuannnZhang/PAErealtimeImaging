#pragma once

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QRegularExpression>
#include <QUuid>
#include <algorithm>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

namespace diagnostic_storage {

// A captured export keeps this lease until its file reads finish. Cleanup
// never waits for an exporter or follows links outside the managed root.
inline std::shared_ptr<QLockFile> lease(const QString &root)
{
    auto lock = std::make_shared<QLockFile>(QDir(root).filePath(QStringLiteral(".retention.lock")));
    lock->setStaleLockTime(0);
    return lock->tryLock(0) ? lock : nullptr;
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
        "(?:trace|timing|looplog|looplog-window)-[0-9]+\\.bin|\\.active\\.lock)$"));
    for (const auto &dir : root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (dir.isSymLink() || dir.isJunction() || QUuid(dir.fileName()).isNull()) continue;
        const QString runPath = dir.absoluteFilePath();
        const auto children = QDir(runPath).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
        qint64 bytes = 0;
        auto updated = dir.lastModified();
        bool owned = true;
        for (const auto &child : children) {
            if (!child.isFile() || child.isSymLink() || child.isJunction() || !known.match(child.fileName()).hasMatch()) {
                owned = false; break;
            }
            bytes += child.size();
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
        if (removed) for (const auto &entry : entries) {
            if (entry.fileName() != QStringLiteral(".active.lock") &&
                entry.fileName() != QStringLiteral("run-config.json") &&
                !QFile::remove(entry.absoluteFilePath())) { removed = false; break; }
        }
        if (removed) removed = dir.remove(QStringLiteral("run-config.json"));
        active.unlock();
        if (removed && root.rmdir(dir.dirName())) { total -= run.bytes; --remaining; }
    }
    return total;
}
}
