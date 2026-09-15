#pragma once

#include <QString>
#include <QHash>

#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>

namespace paimage {

// The physical-round source is the only authority that can advance an
// automatic-save session.  The UI consumes the result after the data-plane
// commit; it never derives the next generation itself.
enum class AutoSaveBoundaryKind : std::uint8_t {
    SessionStart = 0,
    Count = 1,
    Timeout = 2
};

const char* autoSaveBoundaryKindName(AutoSaveBoundaryKind kind);

class AutoSaveRoundCoordinator final {
public:
    // A failed directory registration is represented by a non-zero sentinel
    // so a producer cannot silently fall back to the previous auto-save
    // directory.  FileSaver resolves it to an empty path and drops the group.
    static constexpr std::uint64_t kFailedGeneration =
        (std::numeric_limits<std::uint64_t>::max)();

    struct CommitResult {
        bool enabled = false;
        bool committed = false;
        bool alreadyApplied = false;
        bool failed = false;
        std::uint64_t measurementSession = 0;
        std::uint64_t roundGeneration = 0;
        AutoSaveBoundaryKind boundaryKind = AutoSaveBoundaryKind::Timeout;
        std::uint64_t oldSessionGen = 0;
        std::uint64_t newSessionGen = 0;
        std::uint64_t publishedSessionGen = 0;
        QString boundaryKey;
        QString directory;
        QString phase;
        QString error;
    };

    struct Event {
        enum class Kind : std::uint8_t {
            Reserved,
            Committed,
            AlreadyApplied,
            Failed
        };

        Kind kind = Kind::Failed;
        std::uint64_t measurementSession = 0;
        std::uint64_t roundGeneration = 0;
        AutoSaveBoundaryKind boundaryKind = AutoSaveBoundaryKind::Timeout;
        std::uint64_t oldSessionGen = 0;
        std::uint64_t newSessionGen = 0;
        std::uint64_t publishedSessionGen = 0;
        QString boundaryKey;
        QString directory;
        QString phase;
        QString error;
    };

    using DirectoryPreparer = std::function<bool(const QString&)>;
    using EventSink = std::function<void(const Event&)>;

    explicit AutoSaveRoundCoordinator(DirectoryPreparer preparer = {});

    // Configuration is low frequency and resets the boundary epoch.  The
    // caller supplies the largest existing three-digit folder number so a
    // new controller never reuses an on-disk session directory.
    void configure(const QString& baseDirectory,
                   std::uint64_t lastDirectoryNumber);
    void disable();

    void setDirectoryPreparer(DirectoryPreparer preparer);
    void setEventSink(EventSink sink);

    CommitResult beginSession(std::uint64_t measurementSession,
                              const QString& phase = QStringLiteral("ui_session_start"));
    CommitResult commitBoundary(std::uint64_t measurementSession,
                                std::uint64_t roundGeneration,
                                AutoSaveBoundaryKind boundaryKind,
                                const QString& phase);

    // This is the hot-path read used while stamping a LogicalScan.  It is an
    // acquire load of the generation published only after its directory has
    // been prepared and registered.
    std::uint64_t currentGeneration() const noexcept;
    bool enabled() const noexcept;
    bool faulted() const noexcept;

    QString directoryFor(std::uint64_t generation) const;
    QString baseDirectory() const;

private:
    static QString makeBoundaryKey(std::uint64_t measurementSession,
                                   std::uint64_t roundGeneration,
                                   AutoSaveBoundaryKind boundaryKind);
    static Event eventFromResult(Event::Kind kind, const CommitResult& result);
    void publishEvents(const Event& reserved,
                       bool hasReserved,
                       const Event& finalEvent) const;

    mutable std::mutex mutex_;
    QString baseDirectory_;
    std::uint64_t nextDirectoryNumber_ = 0;
    QHash<std::uint64_t, QString> directories_;
    QHash<QString, std::uint64_t> boundaryBindings_;
    bool configured_ = false;
    bool hasLastBoundary_ = false;
    std::uint64_t lastMeasurementSession_ = 0;
    std::uint64_t lastRoundGeneration_ = 0;
    AutoSaveBoundaryKind lastBoundaryKind_ = AutoSaveBoundaryKind::Timeout;
    DirectoryPreparer directoryPreparer_;
    EventSink eventSink_;

    std::atomic<std::uint64_t> currentGeneration_{0};
    std::atomic<bool> enabled_{false};
    std::atomic<bool> faulted_{false};
};

} // namespace paimage
