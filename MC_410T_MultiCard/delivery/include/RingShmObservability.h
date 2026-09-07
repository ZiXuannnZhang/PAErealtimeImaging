#pragma once

#include <QJsonObject>

#include <chrono>
#include <cstdint>
#include <mutex>

// Pure bookkeeping for the Ring SHM single-slot observability path.
// This type never reads/writes SHM and never changes producer/consumer flow.
namespace ring_shm_obs {

struct ReadyMetadata {
    uint32_t seq = 0;
    uint64_t submitIndex = 0;
    uint64_t submitWallUs = 0;
    bool hasSeq = false;
};

// The optional fields are deliberately ignored when absent so the original
// {"cmd":"ring_block_ready","seq":N} message remains valid.
inline ReadyMetadata parseReadyMessage(const QJsonObject &msg)
{
    ReadyMetadata result;
    const QJsonValue seq = msg.value(QStringLiteral("seq"));
    if (!seq.isUndefined() && !seq.isNull()) {
        result.seq = static_cast<uint32_t>(seq.toVariant().toULongLong());
        result.hasSeq = true;
    }
    const QJsonValue submitIndex = msg.value(QStringLiteral("submit_index"));
    if (!submitIndex.isUndefined() && !submitIndex.isNull())
        result.submitIndex = submitIndex.toVariant().toULongLong();
    const QJsonValue submitWallUs = msg.value(QStringLiteral("submit_wall_us"));
    if (!submitWallUs.isUndefined() && !submitWallUs.isNull())
        result.submitWallUs = submitWallUs.toVariant().toULongLong();
    return result;
}

inline uint64_t wallNowUs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

inline uint64_t steadyNowUs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct Snapshot {
    uint64_t session = 0;
    uint64_t epoch = 0;

    uint64_t submitted = 0;
    uint64_t slotBusyBeforeSubmit = 0;
    uint64_t notifications = 0;
    uint64_t consumed = 0;
    uint64_t notifyShmMismatch = 0;
    uint64_t readyZeroBeforeCopy = 0;
    uint64_t duplicateShmSeq = 0;
    uint64_t shmSeqGap = 0;

    uint64_t lastSubmitIndex = 0;
    uint64_t lastSubmitIntervalUs = 0;
    uint32_t lastSubmittedSeq = 0;
    uint32_t lastNotifySeq = 0;
    uint32_t lastShmSeq = 0;
    bool hasLastSubmittedSeq = false;
    bool hasLastNotifySeq = false;
    bool hasLastShmSeq = false;

    int64_t lastQueueDelayUs = -1;
    uint64_t lastCopyLockUs = 0;
    uint64_t lastProcessUs = 0;
    uint64_t totalQueueDelayUs = 0;
    uint64_t totalCopyLockUs = 0;
    uint64_t totalProcessUs = 0;
    uint64_t processSamples = 0;
};

struct ProducerEvent {
    bool slotBusy = false;
    uint32_t previousSeq = 0;
    uint32_t newSeq = 0;
    uint64_t submitIndex = 0;
    uint64_t submitWallUs = 0;
    uint64_t submitIntervalUs = 0;
    uint64_t submittedCount = 0;
    uint64_t slotBusyCount = 0;
    bool periodicDue = false;
};

struct ConsumerEvent {
    bool notifyShmMismatch = false;
    bool readyZeroBeforeCopy = false;
    bool duplicateShmSeq = false;
    bool shmSeqGap = false;
    uint32_t notifySeq = 0;
    uint32_t shmSeq = 0;
    uint8_t readyBeforeCopy = 0;
    int64_t queueDelayUs = -1;
    uint64_t copyLockUs = 0;
    uint64_t consumedCount = 0;
    bool periodicDue = false;

    bool hasAnomaly() const
    {
        return notifyShmMismatch || readyZeroBeforeCopy
            || duplicateShmSeq || shmSeqGap;
    }
};

class Tracker {
public:
    void beginSession()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const uint64_t nextSession = m_state.session + 1;
        m_state = Snapshot{};
        m_state.session = nextSession;
        m_haveLastSubmit = false;
        m_lastSubmitWallUs = 0;
        m_haveLastConsumed = false;
        m_lastConsumedSeq = 0;
    }

    // Keep cumulative counters but reset sequence continuity for a new epoch.
    void resetEpoch()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_state.epoch;
        m_haveLastConsumed = false;
        m_lastConsumedSeq = 0;
        m_state.hasLastNotifySeq = false;
        m_state.hasLastShmSeq = false;
        m_state.lastQueueDelayUs = -1;
        m_state.lastCopyLockUs = 0;
        m_state.lastProcessUs = 0;
    }

    ProducerEvent observeProducerSubmit(uint8_t previousReady,
                                         uint32_t previousSeq,
                                         uint32_t newSeq,
                                         uint64_t submitWallUs)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_state.submitted;
        const bool slotBusy = previousReady != 0;
        if (slotBusy) ++m_state.slotBusyBeforeSubmit;

        uint64_t intervalUs = 0;
        if (m_haveLastSubmit && submitWallUs >= m_lastSubmitWallUs)
            intervalUs = submitWallUs - m_lastSubmitWallUs;
        m_haveLastSubmit = true;
        m_lastSubmitWallUs = submitWallUs;
        m_state.lastSubmitIndex = m_state.submitted;
        m_state.lastSubmitIntervalUs = intervalUs;
        m_state.lastSubmittedSeq = newSeq;
        m_state.hasLastSubmittedSeq = true;

        ProducerEvent event;
        event.slotBusy = slotBusy;
        event.previousSeq = previousSeq;
        event.newSeq = newSeq;
        event.submitIndex = m_state.lastSubmitIndex;
        event.submitWallUs = submitWallUs;
        event.submitIntervalUs = intervalUs;
        event.submittedCount = m_state.submitted;
        event.slotBusyCount = m_state.slotBusyBeforeSubmit;
        event.periodicDue = (m_state.submitted % kPeriodicEvery) == 0;
        return event;
    }

    uint64_t observeNotification(uint32_t notifySeq, uint64_t submitIndex)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_state.notifications;
        m_state.lastNotifySeq = notifySeq;
        m_state.hasLastNotifySeq = true;
        m_state.lastSubmitIndex = submitIndex;
        return m_state.notifications;
    }

    ConsumerEvent observeConsumed(uint32_t notifySeq,
                                   bool notifySeqValid,
                                   uint32_t shmSeq,
                                   uint8_t readyBeforeCopy,
                                   uint64_t submitWallUs,
                                   uint64_t consumerWallUs,
                                   uint64_t copyLockUs)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_state.consumed;
        m_state.lastShmSeq = shmSeq;
        m_state.hasLastShmSeq = true;

        ConsumerEvent event;
        event.notifySeq = notifySeq;
        event.shmSeq = shmSeq;
        event.readyBeforeCopy = readyBeforeCopy;
        event.notifyShmMismatch = notifySeqValid && notifySeq != shmSeq;
        event.readyZeroBeforeCopy = readyBeforeCopy == 0;
        if (event.notifyShmMismatch) ++m_state.notifyShmMismatch;
        if (event.readyZeroBeforeCopy) ++m_state.readyZeroBeforeCopy;

        if (m_haveLastConsumed) {
            if (shmSeq == m_lastConsumedSeq) {
                event.duplicateShmSeq = true;
                ++m_state.duplicateShmSeq;
            } else if (shmSeq != static_cast<uint32_t>(m_lastConsumedSeq + 1u)) {
                event.shmSeqGap = true;
                ++m_state.shmSeqGap;
            }
        }
        m_haveLastConsumed = true;
        m_lastConsumedSeq = shmSeq;

        event.queueDelayUs = -1;
        if (submitWallUs != 0) {
            event.queueDelayUs = static_cast<int64_t>(consumerWallUs)
                               - static_cast<int64_t>(submitWallUs);
            if (event.queueDelayUs >= 0)
                m_state.totalQueueDelayUs += static_cast<uint64_t>(event.queueDelayUs);
        }
        m_state.lastQueueDelayUs = event.queueDelayUs;
        m_state.lastCopyLockUs = copyLockUs;
        m_state.totalCopyLockUs += copyLockUs;

        event.consumedCount = m_state.consumed;
        event.periodicDue = (m_state.consumed % kPeriodicEvery) == 0;
        return event;
    }

    void recordProcessDuration(uint64_t processUs)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_state.lastProcessUs = processUs;
        m_state.totalProcessUs += processUs;
        ++m_state.processSamples;
    }

    Snapshot snapshot() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_state;
    }

    static constexpr uint64_t kPeriodicEvery = 32;

private:
    mutable std::mutex m_mutex;
    Snapshot m_state;
    bool m_haveLastSubmit = false;
    uint64_t m_lastSubmitWallUs = 0;
    bool m_haveLastConsumed = false;
    uint32_t m_lastConsumedSeq = 0;
};

inline QJsonObject snapshotToJson(const Snapshot &snapshot)
{
    QJsonObject json;
    json[QStringLiteral("session")] = static_cast<qint64>(snapshot.session);
    json[QStringLiteral("epoch")] = static_cast<qint64>(snapshot.epoch);
    json[QStringLiteral("submitted")] = static_cast<qint64>(snapshot.submitted);
    json[QStringLiteral("slot_busy_before_submit")] = static_cast<qint64>(snapshot.slotBusyBeforeSubmit);
    json[QStringLiteral("notifications")] = static_cast<qint64>(snapshot.notifications);
    json[QStringLiteral("consumed")] = static_cast<qint64>(snapshot.consumed);
    json[QStringLiteral("notify_shm_mismatch")] = static_cast<qint64>(snapshot.notifyShmMismatch);
    json[QStringLiteral("ready_zero_before_copy")] = static_cast<qint64>(snapshot.readyZeroBeforeCopy);
    json[QStringLiteral("duplicate_shm_seq")] = static_cast<qint64>(snapshot.duplicateShmSeq);
    json[QStringLiteral("shm_seq_gap")] = static_cast<qint64>(snapshot.shmSeqGap);
    json[QStringLiteral("last_submit_index")] = static_cast<qint64>(snapshot.lastSubmitIndex);
    json[QStringLiteral("last_submit_interval_us")] = static_cast<qint64>(snapshot.lastSubmitIntervalUs);
    json[QStringLiteral("last_submitted_seq")] = static_cast<qint64>(snapshot.lastSubmittedSeq);
    json[QStringLiteral("last_notify_seq")] = static_cast<qint64>(snapshot.lastNotifySeq);
    json[QStringLiteral("last_shm_seq")] = static_cast<qint64>(snapshot.lastShmSeq);
    json[QStringLiteral("has_last_submitted_seq")] = snapshot.hasLastSubmittedSeq;
    json[QStringLiteral("has_last_notify_seq")] = snapshot.hasLastNotifySeq;
    json[QStringLiteral("has_last_shm_seq")] = snapshot.hasLastShmSeq;
    json[QStringLiteral("last_queue_delay_us")] = static_cast<qint64>(snapshot.lastQueueDelayUs);
    json[QStringLiteral("last_copy_lock_us")] = static_cast<qint64>(snapshot.lastCopyLockUs);
    json[QStringLiteral("last_process_us")] = static_cast<qint64>(snapshot.lastProcessUs);
    json[QStringLiteral("avg_queue_delay_us")] = snapshot.consumed > 0
        ? static_cast<double>(snapshot.totalQueueDelayUs) / static_cast<double>(snapshot.consumed) : 0.0;
    json[QStringLiteral("avg_copy_lock_us")] = snapshot.consumed > 0
        ? static_cast<double>(snapshot.totalCopyLockUs) / static_cast<double>(snapshot.consumed) : 0.0;
    json[QStringLiteral("avg_process_us")] = snapshot.processSamples > 0
        ? static_cast<double>(snapshot.totalProcessUs) / static_cast<double>(snapshot.processSamples) : 0.0;
    return json;
}

} // namespace ring_shm_obs
