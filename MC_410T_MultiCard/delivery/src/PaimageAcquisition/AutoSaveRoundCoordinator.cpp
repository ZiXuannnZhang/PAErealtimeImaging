#include "PaimageAcquisition/AutoSaveRoundCoordinator.h"

#include <QDir>

#include <limits>
#include <utility>

namespace paimage {

const char* autoSaveBoundaryKindName(AutoSaveBoundaryKind kind)
{
    switch (kind) {
    case AutoSaveBoundaryKind::SessionStart: return "session_start";
    case AutoSaveBoundaryKind::Count: return "count";
    case AutoSaveBoundaryKind::Timeout: return "timeout";
    }
    return "unknown";
}

AutoSaveRoundCoordinator::AutoSaveRoundCoordinator(DirectoryPreparer preparer)
    : directoryPreparer_(std::move(preparer))
{
    if (!directoryPreparer_)
        directoryPreparer_ = [](const QString& path) { return QDir().mkpath(path); };
}

void AutoSaveRoundCoordinator::configure(const QString& baseDirectory,
                                         std::uint64_t lastDirectoryNumber)
{
    std::lock_guard<std::mutex> lock(mutex_);
    baseDirectory_ = baseDirectory.trimmed().isEmpty()
        ? QString()
        : QDir(baseDirectory).absolutePath();
    nextDirectoryNumber_ = lastDirectoryNumber;
    directories_.clear();
    boundaryBindings_.clear();
    roundBindings_.clear();
    lookupFailureKeys_.clear();
    configured_ = !baseDirectory_.isEmpty();
    hasLastBoundary_ = false;
    lastMeasurementSession_ = 0;
    lastRoundGeneration_ = 0;
    lastBoundaryKind_ = AutoSaveBoundaryKind::SessionStart;
    currentGeneration_.store(0, std::memory_order_release);
    enabled_.store(configured_, std::memory_order_release);
    faulted_.store(false, std::memory_order_release);
}

void AutoSaveRoundCoordinator::disable()
{
    std::lock_guard<std::mutex> lock(mutex_);
    configured_ = false;
    enabled_.store(false, std::memory_order_release);
    // Keep a failed coordinator fail-closed until the next configure().
    // Clearing the generation here could let an in-flight saver route a
    // group back to its previous directory during asynchronous stop.
    if (faulted_.load(std::memory_order_acquire)) {
        currentGeneration_.store(kFailedGeneration, std::memory_order_release);
    } else {
        currentGeneration_.store(0, std::memory_order_release);
    }
}

void AutoSaveRoundCoordinator::setDirectoryPreparer(DirectoryPreparer preparer)
{
    std::lock_guard<std::mutex> lock(mutex_);
    directoryPreparer_ = std::move(preparer);
    if (!directoryPreparer_)
        directoryPreparer_ = [](const QString& path) { return QDir().mkpath(path); };
}

void AutoSaveRoundCoordinator::setEventSink(EventSink sink)
{
    std::lock_guard<std::mutex> lock(mutex_);
    eventSink_ = std::move(sink);
}

QString AutoSaveRoundCoordinator::makeBoundaryKey(
    std::uint64_t measurementSession,
    std::uint64_t roundGeneration,
    AutoSaveBoundaryKind boundaryKind)
{
    return QStringLiteral("%1/%2/%3")
        .arg(QString::number(measurementSession),
             QString::number(roundGeneration),
             QString::number(static_cast<unsigned>(boundaryKind)));
}

QString AutoSaveRoundCoordinator::makeRoundKey(
    std::uint64_t measurementSession,
    std::uint64_t roundGeneration)
{
    return QStringLiteral("%1/%2")
        .arg(QString::number(measurementSession),
             QString::number(roundGeneration));
}

AutoSaveRoundCoordinator::Event AutoSaveRoundCoordinator::eventFromResult(
    Event::Kind kind, const CommitResult& result)
{
    Event event;
    event.kind = kind;
    event.measurementSession = result.measurementSession;
    event.roundGeneration = result.roundGeneration;
    event.boundaryKind = result.boundaryKind;
    event.oldSessionGen = result.oldSessionGen;
    event.newSessionGen = result.newSessionGen;
    event.publishedSessionGen = result.publishedSessionGen;
    event.boundaryKey = result.boundaryKey;
    event.oldDirectory = result.oldDirectory;
    event.directory = result.directory;
    event.phase = result.phase;
    event.error = result.error;
    return event;
}

void AutoSaveRoundCoordinator::publishEvents(const Event& reserved,
                                             bool hasReserved,
                                             const Event& finalEvent) const
{
    EventSink sink;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sink = eventSink_;
    }
    if (!sink) return;
    if (hasReserved) sink(reserved);
    sink(finalEvent);
}

AutoSaveRoundCoordinator::CommitResult AutoSaveRoundCoordinator::beginSession(
    std::uint64_t measurementSession, const QString& phase)
{
    return commitBoundary(measurementSession, 0,
                          AutoSaveBoundaryKind::SessionStart, phase);
}

AutoSaveRoundCoordinator::CommitResult AutoSaveRoundCoordinator::bindMeasurementSession(
    std::uint64_t measurementSession, const QString& phase)
{
    return bindMeasurementRound(measurementSession, 0, phase);
}

AutoSaveRoundCoordinator::CommitResult AutoSaveRoundCoordinator::bindMeasurementRound(
    std::uint64_t measurementSession,
    std::uint64_t roundGeneration,
    const QString& phase)
{
    CommitResult result;
    result.measurementSession = measurementSession;
    result.roundGeneration = roundGeneration;
    result.boundaryKind = AutoSaveBoundaryKind::SessionStart;
    result.phase = phase;
    result.boundaryKey = makeBoundaryKey(
        measurementSession, roundGeneration, AutoSaveBoundaryKind::SessionStart);

    Event finalEvent;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        result.enabled = enabled_.load(std::memory_order_relaxed);
        result.oldSessionGen = currentGeneration_.load(std::memory_order_relaxed);
        result.oldDirectory = directories_.value(result.oldSessionGen);

        auto fail = [&](const QString& error) {
            result.failed = true;
            result.enabled = false;
            result.error = error;
            result.newSessionGen = 0;
            currentGeneration_.store(kFailedGeneration, std::memory_order_release);
            enabled_.store(false, std::memory_order_release);
            faulted_.store(true, std::memory_order_release);
            finalEvent = eventFromResult(Event::Kind::Failed, result);
        };

        if (faulted_.load(std::memory_order_relaxed)) {
            fail(QStringLiteral("自动保存协调器已故障，等待重新配置"));
        } else if (!configured_ || !result.enabled) {
            // HostOutput keeps this binder installed for the whole listener
            // lifetime.  A manual-save session must therefore be a harmless
            // no-op, not a coordinator fault; its round resolver returns the
            // existing generation=0 manual semantics.
            return result;
        } else if (result.oldSessionGen == 0 ||
                   result.oldSessionGen == kFailedGeneration ||
                   result.oldDirectory.isEmpty()) {
            fail(QStringLiteral("测量会话开始时没有已准备的自动保存 generation"));
        } else {
            // A measurement start is a lifecycle boundary.  The numeric
            // source token may be reused after a source restart, so never
            // retain an old session's round bindings or boundary ordering.
            roundBindings_.clear();
            boundaryBindings_.clear();
            lookupFailureKeys_.clear();
            hasLastBoundary_ = false;

            const QString roundKey = makeRoundKey(measurementSession, roundGeneration);
            roundBindings_.insert(roundKey, result.oldSessionGen);
            boundaryBindings_.insert(result.boundaryKey, result.oldSessionGen);
            result.newSessionGen = result.oldSessionGen;
            result.publishedSessionGen = result.oldSessionGen;
            result.directory = result.oldDirectory;
            result.committed = true;
            finalEvent = eventFromResult(Event::Kind::Committed, result);
        }
    }

    publishEvents(Event{}, false, finalEvent);
    return result;
}

AutoSaveRoundCoordinator::CommitResult AutoSaveRoundCoordinator::commitBoundary(
    std::uint64_t measurementSession,
    std::uint64_t roundGeneration,
    AutoSaveBoundaryKind boundaryKind,
    const QString& phase)
{
    CommitResult result;
    result.measurementSession = measurementSession;
    result.roundGeneration = roundGeneration;
    result.boundaryKind = boundaryKind;
    result.phase = phase;
    result.boundaryKey = makeBoundaryKey(measurementSession, roundGeneration, boundaryKind);

    Event reservedEvent;
    bool hasReservedEvent = false;
    Event finalEvent;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        result.enabled = enabled_.load(std::memory_order_relaxed);
        result.oldSessionGen = currentGeneration_.load(std::memory_order_relaxed);
        result.oldDirectory = directories_.value(result.oldSessionGen);

        auto fail = [&](const QString& error) {
            result.failed = true;
            result.enabled = false;
            result.error = error;
            result.newSessionGen = 0;
            currentGeneration_.store(kFailedGeneration, std::memory_order_release);
            enabled_.store(false, std::memory_order_release);
            faulted_.store(true, std::memory_order_release);
            finalEvent = eventFromResult(Event::Kind::Failed, result);
        };

        if (!configured_ || !result.enabled || faulted_.load(std::memory_order_relaxed)) {
            fail(faulted_.load(std::memory_order_relaxed)
                     ? QStringLiteral("自动保存协调器已故障，等待重新配置")
                     : QStringLiteral("自动保存协调器尚未配置有效基线目录"));
        } else {
            const auto existing = boundaryBindings_.constFind(result.boundaryKey);
            const auto roundKey = makeRoundKey(measurementSession, roundGeneration);
            const auto existingRound = roundBindings_.constFind(roundKey);
            const bool exactDuplicate = existing != boundaryBindings_.constEnd();
            const bool roundAlreadyBound = existingRound != roundBindings_.constEnd();
            const bool olderBoundary = hasLastBoundary_ &&
                (measurementSession < lastMeasurementSession_ ||
                 (measurementSession == lastMeasurementSession_ &&
                  roundGeneration <= lastRoundGeneration_));
            if (exactDuplicate || roundAlreadyBound || olderBoundary) {
                result.alreadyApplied = true;
                result.enabled = true;
                result.newSessionGen = exactDuplicate
                    ? existing.value()
                    : roundAlreadyBound
                        ? existingRound.value()
                    : currentGeneration_.load(std::memory_order_relaxed);
                result.publishedSessionGen = currentGeneration_.load(std::memory_order_relaxed);
                result.directory = directories_.value(result.newSessionGen);
                boundaryBindings_.insert(result.boundaryKey, result.newSessionGen);
                finalEvent = eventFromResult(Event::Kind::AlreadyApplied, result);
            } else if (nextDirectoryNumber_ >=
                       (std::numeric_limits<std::uint64_t>::max)() - 1u) {
                fail(QStringLiteral("自动保存目录编号耗尽"));
            } else {
                const std::uint64_t candidate = ++nextDirectoryNumber_;
                const QString folder = QStringLiteral("%1")
                    .arg(QString::number(candidate), 3, QChar('0'));
                const QString directory = QDir(baseDirectory_).filePath(folder);
                result.newSessionGen = candidate;
                result.directory = directory;
                reservedEvent = eventFromResult(Event::Kind::Reserved, result);
                hasReservedEvent = true;

                bool prepared = false;
                try {
                    prepared = directoryPreparer_ && directoryPreparer_(directory);
                } catch (...) {
                    prepared = false;
                }
                if (!prepared) {
                    // Do not insert a mapping or publish the candidate.  The
                    // fault sentinel makes subsequent groups fail closed.
                    --nextDirectoryNumber_;
                    fail(QStringLiteral("无法创建/注册自动保存目录：%1").arg(directory));
                } else {
                    directories_.insert(candidate, directory);
                    boundaryBindings_.insert(result.boundaryKey, candidate);
                    roundBindings_.insert(roundKey, candidate);
                    lookupFailureKeys_.remove(roundKey);
                    hasLastBoundary_ = true;
                    lastMeasurementSession_ = measurementSession;
                    lastRoundGeneration_ = roundGeneration;
                    lastBoundaryKind_ = boundaryKind;
                    result.committed = true;
                    result.enabled = true;
                    result.publishedSessionGen = candidate;
                    // The release store is the publication point.  The
                    // mapping is already visible under mutex before this
                    // store, so producers cannot observe an unregistered gen.
                    currentGeneration_.store(candidate, std::memory_order_release);
                    finalEvent = eventFromResult(Event::Kind::Committed, result);
                }
            }
        }
    }

    publishEvents(reservedEvent, hasReservedEvent, finalEvent);
    return result;
}

AutoSaveRoundCoordinator::ResolveResult AutoSaveRoundCoordinator::resolveRound(
    std::uint64_t measurementSession,
    std::uint64_t roundGeneration,
    const QString& phase) const
{
    ResolveResult result;
    result.measurementSession = measurementSession;
    result.roundGeneration = roundGeneration;
    result.roundKey = makeRoundKey(measurementSession, roundGeneration);
    result.phase = phase;

    Event lookupEvent;
    EventSink sink;
    bool emitLookupFailure = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        result.enabled = enabled_.load(std::memory_order_relaxed);
        if (faulted_.load(std::memory_order_relaxed)) {
            result.failed = true;
            result.sessionGen = kFailedGeneration;
            result.error = QStringLiteral("自动保存协调器已故障，拒绝 round lookup");
            return result;
        }
        if (!result.enabled)
            return result;

        if (!configured_) {
            result.failed = true;
            result.sessionGen = kFailedGeneration;
            result.error = QStringLiteral("自动保存协调器不可用，拒绝未绑定 round");
            return result;
        }

        const auto it = roundBindings_.constFind(result.roundKey);
        if (it != roundBindings_.constEnd()) {
            result.resolved = true;
            result.sessionGen = it.value();
            result.directory = directories_.value(result.sessionGen);
            if (result.sessionGen == 0 || result.sessionGen == kFailedGeneration ||
                result.directory.isEmpty()) {
                result.resolved = false;
                result.failed = true;
                result.sessionGen = kFailedGeneration;
                result.error = QStringLiteral("round binding 的目录映射不可用");
            } else {
                return result;
            }
        } else {
            result.failed = true;
            result.sessionGen = kFailedGeneration;
            result.error = QStringLiteral("未找到自动保存 round binding：%1")
                               .arg(result.roundKey);
        }

        // A missing identity is a low-frequency structural diagnostic, not a
        // per-packet log.  A later successful binding removes this key.
        if (!lookupFailureKeys_.contains(result.roundKey)) {
            lookupFailureKeys_.insert(result.roundKey);
            lookupEvent.kind = Event::Kind::LookupFailed;
            lookupEvent.measurementSession = measurementSession;
            lookupEvent.roundGeneration = roundGeneration;
            lookupEvent.boundaryKind = AutoSaveBoundaryKind::SessionStart;
            lookupEvent.publishedSessionGen = currentGeneration_.load(std::memory_order_relaxed);
            lookupEvent.boundaryKey = result.roundKey;
            lookupEvent.directory = result.directory;
            lookupEvent.phase = phase;
            lookupEvent.error = result.error;
            sink = eventSink_;
            emitLookupFailure = true;
        }
    }
    if (emitLookupFailure && sink)
        sink(lookupEvent);
    return result;
}

std::uint64_t AutoSaveRoundCoordinator::currentGeneration() const noexcept
{
    return currentGeneration_.load(std::memory_order_acquire);
}

bool AutoSaveRoundCoordinator::enabled() const noexcept
{
    return enabled_.load(std::memory_order_acquire);
}

bool AutoSaveRoundCoordinator::faulted() const noexcept
{
    return faulted_.load(std::memory_order_acquire);
}

QString AutoSaveRoundCoordinator::directoryFor(std::uint64_t generation) const
{
    if (generation == 0 || generation == kFailedGeneration)
        return QString();
    std::lock_guard<std::mutex> lock(mutex_);
    return directories_.value(generation);
}

QString AutoSaveRoundCoordinator::baseDirectory() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return baseDirectory_;
}

AutoSavePresentationState::ApplyResult AutoSavePresentationState::apply(
    const AutoSaveRoundCoordinator::CommitResult& commit)
{
    if (commit.failed || (!commit.committed && !commit.alreadyApplied) ||
        commit.newSessionGen == 0 ||
        commit.newSessionGen == AutoSaveRoundCoordinator::kFailedGeneration ||
        commit.directory.isEmpty())
        return ApplyResult::Invalid;

    if (!target_.valid) {
        target_.valid = true;
        target_.measurementSession = commit.measurementSession;
        target_.roundGeneration = commit.roundGeneration;
        target_.sessionGen = commit.newSessionGen;
        target_.directory = commit.directory;
        return ApplyResult::Applied;
    }

    const bool olderIdentity =
        commit.measurementSession < target_.measurementSession ||
        (commit.measurementSession == target_.measurementSession &&
         (commit.roundGeneration < target_.roundGeneration ||
          (commit.roundGeneration == target_.roundGeneration &&
           commit.newSessionGen < target_.sessionGen)));
    if (olderIdentity)
        return ApplyResult::Stale;

    const bool sameIdentity =
        commit.measurementSession == target_.measurementSession &&
        commit.roundGeneration == target_.roundGeneration &&
        commit.newSessionGen == target_.sessionGen &&
        commit.directory == target_.directory;
    if (sameIdentity)
        return ApplyResult::AlreadyCurrent;

    target_.measurementSession = commit.measurementSession;
    target_.roundGeneration = commit.roundGeneration;
    target_.sessionGen = commit.newSessionGen;
    target_.directory = commit.directory;
    return ApplyResult::Applied;
}

} // namespace paimage
