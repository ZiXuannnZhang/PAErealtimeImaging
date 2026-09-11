#include "ImagingBypass.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace {
using Clock = std::chrono::steady_clock;
std::uint64_t elapsedNs(Clock::time_point begin)
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count());
}
}

const char *imagingSubmitResultName(ImagingSubmitResult result) noexcept
{
    switch (result) {
    case ImagingSubmitResult::Accepted: return "Accepted";
    case ImagingSubmitResult::Disabled: return "Disabled";
    case ImagingSubmitResult::QueueFull: return "QueueFull";
    case ImagingSubmitResult::QueueBusy: return "QueueBusy";
    case ImagingSubmitResult::BusySlots: return "BusySlots";
    case ImagingSubmitResult::Stopping: return "Stopping";
    case ImagingSubmitResult::InvalidFrame: return "InvalidFrame";
    case ImagingSubmitResult::InvalidPayload: return "InvalidPayload";
    case ImagingSubmitResult::VersionMismatch: return "VersionMismatch";
    case ImagingSubmitResult::StaleGeneration: return "StaleGeneration";
    case ImagingSubmitResult::StaleSession: return "StaleSession";
    case ImagingSubmitResult::ServiceNotReady: return "ServiceNotReady";
    case ImagingSubmitResult::CallbackFailed: return "CallbackFailed";
    }
    return "InvalidFrame";
}

ImagingBypass::ImagingBypass(std::size_t capacity)
    : capacity_(std::max<std::size_t>(1, capacity))
{
}

ImagingBypass::~ImagingBypass()
{
    stop();
}

void ImagingBypass::setConsumer(Consumer consumer)
{
    std::lock_guard<std::mutex> lock(mutex_);
    consumer_ = std::move(consumer);
}

void ImagingBypass::setEnabledChannels(const std::array<bool, 8> &channels)
{
    std::lock_guard<std::mutex> lock(mutex_);
    enabledChannels_ = channels;
    std::uint32_t mask = 0;
    for (std::size_t i = 0; i < channels.size(); ++i)
        if (channels[i]) mask |= (1u << i);
    enabledMask_.store(mask, std::memory_order_release);
}

void ImagingBypass::setServiceReady(bool ready)
{
    serviceReady_.store(ready, std::memory_order_release);
    if (!ready) clear(ImagingSubmitResult::ServiceNotReady);
}

void ImagingBypass::beginSession(std::uint64_t session)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const std::uint64_t discarded = queue_.size();
    queue_.clear();
    depth_.store(0, std::memory_order_relaxed);
    activeSession_.store(session, std::memory_order_release);
    if (discarded) {
        droppedOnClear_.fetch_add(discarded, std::memory_order_relaxed);
        droppedStaleSession_.fetch_add(discarded, std::memory_order_relaxed);
    }
}

void ImagingBypass::start()
{
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;
    stopping_.store(false, std::memory_order_release);
    try {
        worker_ = std::thread(&ImagingBypass::workerLoop, this);
    } catch (...) {
        stopping_.store(true, std::memory_order_release);
        running_.store(false, std::memory_order_release);
        throw;
    }
}

void ImagingBypass::stop()
{
    if (!running_.exchange(false)) return;
    stopping_.store(true, std::memory_order_release);
    clear(ImagingSubmitResult::Stopping);
    ready_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void ImagingBypass::clear(ImagingSubmitResult reason)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const std::uint64_t discarded = queue_.size();
    queue_.clear();
    depth_.store(0, std::memory_order_relaxed);
    if (discarded) {
        droppedOnClear_.fetch_add(discarded, std::memory_order_relaxed);
        countDrop(reason, discarded);
    }
}

ImagingSubmitResult ImagingBypass::tryPush(const TriggerGroupConstPtr &frame) noexcept
{
    const auto begin = Clock::now();
    attempts_.fetch_add(1, std::memory_order_relaxed);
    auto finish = [this, begin](ImagingSubmitResult result) {
        updateMax(maxSubmitNs_, elapsedNs(begin));
        if (result != ImagingSubmitResult::Accepted) countDrop(result);
        return result;
    };
    if (!frame || !frame->isComplete || frame->cardId < 0
        || frame->sampleCount <= 0 || frame->freqA.empty() || frame->freqB.empty())
        return finish(ImagingSubmitResult::InvalidFrame);
    if (!frame->quality.inputUsable())
        return finish(ImagingSubmitResult::InvalidPayload);
    if (stopping_.load(std::memory_order_acquire))
        return finish(ImagingSubmitResult::Stopping);
    if (!enabled_.load(std::memory_order_acquire))
        return finish(ImagingSubmitResult::Disabled);
    if (frame->cardId >= 4) return finish(ImagingSubmitResult::Disabled);
    const int channelA = frame->cardId * 2;
    const int channelB = channelA + 1;
    const auto channelMask = enabledMask_.load(std::memory_order_acquire);
    if ((channelMask & ((1u << channelA) | (1u << channelB))) == 0)
        return finish(ImagingSubmitResult::Disabled);
    if (!serviceReady_.load(std::memory_order_acquire))
        return finish(ImagingSubmitResult::ServiceNotReady);
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return finish(ImagingSubmitResult::QueueBusy);
    std::uint64_t active = activeSession_.load(std::memory_order_acquire);
    const std::uint64_t incoming = frame->measurementSession;
    if (incoming != 0 && active != 0 && incoming < active)
        return finish(ImagingSubmitResult::StaleSession);
    if (incoming != 0 && incoming > active) {
        const std::uint64_t discarded = queue_.size();
        queue_.clear();
        depth_.store(0, std::memory_order_relaxed);
        activeSession_.store(incoming, std::memory_order_release);
        if (discarded) {
            droppedOnClear_.fetch_add(discarded, std::memory_order_relaxed);
            droppedStaleSession_.fetch_add(discarded, std::memory_order_relaxed);
        }
    }
    if (queue_.size() >= capacity_) return finish(ImagingSubmitResult::QueueFull);

    queue_.push_back(Item{frame, incoming});
    const auto depth = depth_.fetch_add(1, std::memory_order_relaxed) + 1;
    updateMax(peakDepth_, depth);
    accepted_.fetch_add(1, std::memory_order_relaxed);
    lock.unlock();
    ready_.notify_one();
    updateMax(maxSubmitNs_, elapsedNs(begin));
    return ImagingSubmitResult::Accepted;
}

void ImagingBypass::workerLoop() noexcept
{
    for (;;) {
        Item item;
        Consumer consumer;
        std::array<bool, 8> channels{};
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ready_.wait(lock, [this] { return stopping_.load() || !queue_.empty(); });
            if (stopping_.load() && queue_.empty()) break;
            item = std::move(queue_.front());
            queue_.pop_front();
            depth_.fetch_sub(1, std::memory_order_relaxed);
            consumer = consumer_;
            channels = enabledChannels_;
        }
        dequeued_.fetch_add(1, std::memory_order_relaxed);
        const auto begin = Clock::now();
        if (item.session != 0 && item.session != activeSession_.load(std::memory_order_acquire)) {
            countDrop(ImagingSubmitResult::StaleSession);
            continue;
        }
        if (!serviceReady_.load(std::memory_order_acquire)) {
            countDrop(ImagingSubmitResult::ServiceNotReady);
            continue;
        }
        try {
            if (consumer && consumer(item.frame, channels))
                processed_.fetch_add(1, std::memory_order_relaxed);
            else
                processFailed_.fetch_add(1, std::memory_order_relaxed);
        } catch (...) {
            processFailed_.fetch_add(1, std::memory_order_relaxed);
        }
        updateMax(maxWorkerNs_, elapsedNs(begin));
    }
}

void ImagingBypass::countDrop(ImagingSubmitResult result, std::uint64_t count) noexcept
{
    switch (result) {
    case ImagingSubmitResult::Disabled: droppedDisabled_.fetch_add(count); break;
    case ImagingSubmitResult::QueueFull: droppedQueueFull_.fetch_add(count); break;
    case ImagingSubmitResult::QueueBusy: droppedQueueBusy_.fetch_add(count); break;
    case ImagingSubmitResult::BusySlots: droppedBusySlots_.fetch_add(count); break;
    case ImagingSubmitResult::Stopping: droppedStopping_.fetch_add(count); break;
    case ImagingSubmitResult::InvalidFrame: droppedInvalidFrame_.fetch_add(count); break;
    case ImagingSubmitResult::InvalidPayload: droppedInvalidPayload_.fetch_add(count); break;
    case ImagingSubmitResult::VersionMismatch: droppedVersionMismatch_.fetch_add(count); break;
    case ImagingSubmitResult::StaleGeneration: droppedStaleGeneration_.fetch_add(count); break;
    case ImagingSubmitResult::StaleSession: droppedStaleSession_.fetch_add(count); break;
    case ImagingSubmitResult::ServiceNotReady: droppedServiceNotReady_.fetch_add(count); break;
    case ImagingSubmitResult::CallbackFailed: droppedCallbackFailed_.fetch_add(count); break;
    case ImagingSubmitResult::Accepted: break;
    }
}

void ImagingBypass::observeBlockResult(bool submitted) noexcept
{
    blocksFormed_.fetch_add(1, std::memory_order_relaxed);
    (submitted ? blocksSubmitted_ : blocksSkipped_).fetch_add(1, std::memory_order_relaxed);
}

void ImagingBypass::observeBlockException() noexcept
{
    blocksFormed_.fetch_add(1, std::memory_order_relaxed);
    blockExceptions_.fetch_add(1, std::memory_order_relaxed);
}

void ImagingBypass::updateMax(std::atomic<std::uint64_t> &target, std::uint64_t value) noexcept
{
    auto current = target.load(std::memory_order_relaxed);
    while (value > current && !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {}
}

ImagingBypass::Snapshot ImagingBypass::snapshot() const noexcept
{
    Snapshot s;
    s.attempts=attempts_.load();s.accepted=accepted_.load();s.dequeued=dequeued_.load();
    s.processed=processed_.load();s.processFailed=processFailed_.load();
    s.blocksFormed=blocksFormed_.load();s.blocksSubmitted=blocksSubmitted_.load();
    s.blocksSkipped=blocksSkipped_.load();s.blockExceptions=blockExceptions_.load();
    s.droppedDisabled=droppedDisabled_.load();s.droppedQueueFull=droppedQueueFull_.load();
    s.droppedQueueBusy=droppedQueueBusy_.load();
    s.droppedBusySlots=droppedBusySlots_.load();
    s.droppedStopping=droppedStopping_.load();s.droppedInvalidFrame=droppedInvalidFrame_.load();
    s.droppedInvalidPayload=droppedInvalidPayload_.load();
    s.droppedVersionMismatch=droppedVersionMismatch_.load();
    s.droppedStaleGeneration=droppedStaleGeneration_.load();
    s.droppedStaleSession=droppedStaleSession_.load();
    s.droppedServiceNotReady=droppedServiceNotReady_.load();s.droppedOnClear=droppedOnClear_.load();
    s.droppedCallbackFailed=droppedCallbackFailed_.load();
    s.currentDepth=depth_.load();s.peakDepth=peakDepth_.load();
    s.maxSubmitNs=maxSubmitNs_.load();s.maxWorkerNs=maxWorkerNs_.load();
    s.activeSession=activeSession_.load();
    return s;
}
