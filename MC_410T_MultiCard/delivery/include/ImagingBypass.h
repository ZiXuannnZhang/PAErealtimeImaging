#pragma once

#include "DataTypes.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

enum class ImagingSubmitResult : std::uint8_t {
    Accepted = 0,
    Disabled,
    QueueFull,
    QueueBusy,
    Stopping,
    InvalidFrame,
    StaleSession,
    ServiceNotReady,
    CallbackFailed
};

const char *imagingSubmitResultName(ImagingSubmitResult result) noexcept;

// Bounded FIFO. The mutex protects only queue/metadata operations; producers
// never wait for queue space or the imaging service.
class ImagingBypass final
{
public:
    struct Snapshot {
        std::uint64_t attempts = 0;
        std::uint64_t accepted = 0;
        std::uint64_t dequeued = 0;
        std::uint64_t processed = 0;
        std::uint64_t processFailed = 0;
        std::uint64_t blocksFormed = 0;
        std::uint64_t blocksSubmitted = 0;
        std::uint64_t blocksSkipped = 0;
        std::uint64_t blockExceptions = 0;
        std::uint64_t droppedDisabled = 0;
        std::uint64_t droppedQueueFull = 0;
        std::uint64_t droppedQueueBusy = 0;
        std::uint64_t droppedStopping = 0;
        std::uint64_t droppedInvalidFrame = 0;
        std::uint64_t droppedStaleSession = 0;
        std::uint64_t droppedServiceNotReady = 0;
        std::uint64_t droppedCallbackFailed = 0;
        std::uint64_t droppedOnClear = 0;
        std::uint64_t currentDepth = 0;
        std::uint64_t peakDepth = 0;
        std::uint64_t maxSubmitNs = 0;
        std::uint64_t maxWorkerNs = 0;
        std::uint64_t activeSession = 0;
    };

    using Consumer = std::function<bool(const TriggerGroupConstPtr &,
                                        const std::array<bool, 8> &)>;

    explicit ImagingBypass(std::size_t capacity = 256);
    ~ImagingBypass();

    ImagingBypass(const ImagingBypass &) = delete;
    ImagingBypass &operator=(const ImagingBypass &) = delete;

    void setConsumer(Consumer consumer);
    void setEnabledChannels(const std::array<bool, 8> &channels);
    void setEnabled(bool enabled) noexcept { enabled_.store(enabled, std::memory_order_release); }
    void setServiceReady(bool ready);
    void beginSession(std::uint64_t session);
    void start();
    void stop();
    void clear(ImagingSubmitResult reason);

    ImagingSubmitResult tryPush(const TriggerGroupConstPtr &frame) noexcept;
    void observeBlockResult(bool submitted) noexcept;
    void observeBlockException() noexcept;
    Snapshot snapshot() const noexcept;

private:
    struct Item { TriggerGroupConstPtr frame; std::uint64_t session = 0; };

    void workerLoop() noexcept;
    void clearLocked(ImagingSubmitResult reason);
    void countDrop(ImagingSubmitResult result, std::uint64_t count = 1) noexcept;
    static void updateMax(std::atomic<std::uint64_t> &target, std::uint64_t value) noexcept;

    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::vector<Item> queue_;
    std::size_t head_ = 0, size_ = 0;
    std::shared_ptr<const Consumer> consumer_;
    std::array<bool, 8> enabledChannels_{};
    std::atomic<std::uint32_t> enabledMask_{0};
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{true};
    std::atomic<bool> enabled_{false};
    std::atomic<bool> serviceReady_{false};
    std::atomic<std::uint64_t> activeSession_{0};

    std::atomic<std::uint64_t> attempts_{0}, accepted_{0}, dequeued_{0};
    std::atomic<std::uint64_t> processed_{0}, processFailed_{0};
    std::atomic<std::uint64_t> blocksFormed_{0}, blocksSubmitted_{0};
    std::atomic<std::uint64_t> blocksSkipped_{0}, blockExceptions_{0};
    std::atomic<std::uint64_t> droppedDisabled_{0}, droppedQueueFull_{0};
    std::atomic<std::uint64_t> droppedQueueBusy_{0};
    std::atomic<std::uint64_t> droppedStopping_{0}, droppedInvalidFrame_{0};
    std::atomic<std::uint64_t> droppedStaleSession_{0}, droppedServiceNotReady_{0};
    std::atomic<std::uint64_t> droppedCallbackFailed_{0};
    std::atomic<std::uint64_t> droppedOnClear_{0}, depth_{0}, peakDepth_{0};
    std::atomic<std::uint64_t> maxSubmitNs_{0}, maxWorkerNs_{0};
};
