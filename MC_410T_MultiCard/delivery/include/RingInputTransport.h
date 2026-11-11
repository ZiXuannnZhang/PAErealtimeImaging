#pragma once

#include "ImagingSharedMemory.h"
#include <climits>
#include <cstring>
#include <limits>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// Input transport only. Output frames use their independent named mutex.
// Every process must use the same mutex name for a given native mapping.
namespace ring_input {
constexpr wchar_t kDefaultMutexName[] = L"Local\\MC_410T_RingShm_Input_v3";
enum class Result { Ok, Busy, Timeout, Abandoned, Invalid, Mismatch, Unavailable, OsError };
struct Identity {
    uint64_t epoch, publicationId, measurementSession, roundGeneration, submitWallUs;
    uint32_t blockSeq;
    int32_t direction;
    bool sourceRoundComplete;
    uint32_t roundBlockIndex;
};

// Size is checked before deriving the metadata pointer. Logical size and version
// matching rejects v2 peers and incompatible configurations.
struct View {
    RingImagingShmHeader *header = nullptr;
    RingInputMetadata *meta = nullptr;
    unsigned char *data = nullptr;
};
inline bool layout(void *memory, size_t bytes, bool requireV3, View &v)
{
    v = {};
    if (!memory || bytes < sizeof(RingImagingShmHeader)) return false;
    auto *h = static_cast<RingImagingShmHeader *>(memory);
    if (h->magic != 0x52494E47u || (requireV3 && h->version != kRingImagingShmVersion3)
        || h->block_size > INT_MAX || h->frame_size > INT_MAX || h->alines > INT_MAX
        || h->display_frame_size > INT_MAX || !h->block_size || !h->alines
        || !h->display_frame_size) return false;
    // Calculate in uint64_t so an invalid mapping cannot wrap size_t arithmetic.
    const uint64_t channelEnd = uint64_t(h->block_size) * 4u + uint64_t(h->alines) * 4u
        + ((uint64_t(h->alines) + 3u) & ~uint64_t(3u));
    const uint64_t end = sizeof(*h) + channelEnd + uint64_t(h->display_frame_size) * 8u;
    const uint64_t offset = (end + 7u) & ~uint64_t(7u);
    if (offset > std::numeric_limits<size_t>::max() - sizeof(RingInputMetadata)
        || bytes < offset + sizeof(RingInputMetadata)) return false;
    v.header = h;
    v.data = reinterpret_cast<unsigned char *>(h + 1);
    v.meta = reinterpret_cast<RingInputMetadata *>(reinterpret_cast<unsigned char *>(memory) + size_t(offset));
    if (reinterpret_cast<uintptr_t>(v.meta) % alignof(RingInputMetadata)) return false;
    if (requireV3 && (v.meta->magic != kRingInputProtocolMagic
        || v.meta->version != kRingInputProtocolVersion || v.meta->size != sizeof(RingInputMetadata)
        || v.meta->logical_size != offset + sizeof(RingInputMetadata))) return false;
    return true;
}

class Mutex {
public:
    explicit Mutex(const wchar_t *name = kDefaultMutexName) {
#ifdef _WIN32
        handle_ = name ? CreateMutexW(nullptr, FALSE, name) : nullptr;
#else
        (void)name;
#endif
    }
    ~Mutex() {
#ifdef _WIN32
        if (handle_) CloseHandle(handle_);
#endif
    }
    Mutex(const Mutex &) = delete;
    Mutex &operator=(const Mutex &) = delete;
    Result lock(uint32_t timeoutMs) {
#ifdef _WIN32
        if (!handle_ || held_) return Result::OsError;
        if (timeoutMs == INFINITE) return Result::Invalid;
        const DWORD r = WaitForSingleObject(handle_, timeoutMs);
        if (r == WAIT_OBJECT_0) { held_ = true; return Result::Ok; }
        if (r == WAIT_ABANDONED) { held_ = true; return Result::Abandoned; }
        if (r == WAIT_TIMEOUT) return Result::Timeout;
        return Result::OsError;
#else
        (void)timeoutMs; return Result::Unavailable;
#endif
    }
    void unlock() {
#ifdef _WIN32
        if (held_) { ReleaseMutex(handle_); held_ = false; }
#endif
    }
private:
#ifdef _WIN32
    HANDLE handle_ = nullptr;
    bool held_ = false;
#endif
};
struct Unlock { Mutex &mutex; ~Unlock() { mutex.unlock(); } };

// On abandonment neither partially published data nor partially copied data is
// accepted. A subsequent explicit reset with a different epoch is required.
inline Result acquire(Mutex &mutex, uint32_t timeoutMs, void *memory, size_t bytes)
{
    Result r = mutex.lock(timeoutMs);
    if (r == Result::Abandoned) {
        View v;
        if (layout(memory, bytes, true, v)) {
            v.meta->state = uint32_t(RingInputSlotState::Poisoned);
            v.header->block_ready = 0;
        }
        mutex.unlock();
    }
    return r;
}

// Only the creator may call initialize, while other attachers are excluded.
// It first fills the existing header/config and creates an exact v3-size map.
inline Result initialize(Mutex &mutex, void *memory, size_t bytes, uint64_t epoch, uint32_t timeoutMs)
{
    if (!epoch) return Result::Invalid;
    Result r = acquire(mutex, timeoutMs, memory, bytes);
    if (r != Result::Ok) return r;
    Unlock unlock{mutex}; View v;
    if (!layout(memory, bytes, false, v)) return Result::Mismatch;
    std::memset(v.meta, 0, sizeof(*v.meta));
    v.meta->magic = kRingInputProtocolMagic; v.meta->version = kRingInputProtocolVersion;
    v.meta->size = sizeof(*v.meta);
    v.meta->logical_size = size_t(reinterpret_cast<unsigned char *>(v.meta) - reinterpret_cast<unsigned char *>(memory)) + sizeof(*v.meta);
    v.meta->epoch = epoch;
    v.meta->state = uint32_t(RingInputSlotState::Empty);
    v.header->block_ready = 0; v.header->version = kRingImagingShmVersion3;
    return Result::Ok;
}
inline Result reset(Mutex &mutex, void *memory, size_t bytes, uint64_t newEpoch, uint32_t timeoutMs)
{
    Result r = acquire(mutex, timeoutMs, memory, bytes);
    if (r != Result::Ok) return r;
    Unlock unlock{mutex}; View v;
    if (!layout(memory, bytes, true, v)) return Result::Mismatch;
    if (!newEpoch || newEpoch == v.meta->epoch) return Result::Invalid;
    v.meta->epoch = newEpoch; v.meta->publication_id = 0; v.meta->acknowledged_id = 0;
    v.meta->state = uint32_t(RingInputSlotState::Empty); v.header->block_ready = 0;
    return Result::Ok;
}
// Caller guarantees raw has block_size floats and angles/channels each have alines elements.
inline Result publish(Mutex &mutex, void *memory, size_t bytes, const Identity &id,
                      const float *raw, const float *angles, const uint8_t *channels, uint32_t timeoutMs)
{
    if (!raw || !angles || !channels || !id.epoch || !id.publicationId
        || id.direction < -1 || id.direction > 1) return Result::Invalid;
    Result r = acquire(mutex, timeoutMs, memory, bytes);
    if (r != Result::Ok) return r;
    Unlock unlock{mutex}; View v;
    if (!layout(memory, bytes, true, v)) return Result::Mismatch;
    auto &m = *v.meta;
    if (m.epoch != id.epoch) return Result::Mismatch;
    if (m.state == uint32_t(RingInputSlotState::Ready)) return Result::Busy;
    if (m.state != uint32_t(RingInputSlotState::Empty) || id.publicationId <= m.publication_id)
        return Result::Invalid;
    std::memcpy(v.data, raw, size_t(v.header->block_size) * sizeof(float));
    std::memcpy(v.data + ringAnglesOffset(int(v.header->block_size)), angles, size_t(v.header->alines) * sizeof(float));
    std::memcpy(v.data + ringChannelsOffset(int(v.header->block_size), int(v.header->alines)), channels, v.header->alines);
    m.publication_id = id.publicationId; m.measurement_session = id.measurementSession;
    m.round_generation = id.roundGeneration; m.submit_wall_us = id.submitWallUs;
    m.block_seq = id.blockSeq; m.direction = id.direction; m.source_round_complete = id.sourceRoundComplete ? 1 : 0;
    m.round_block_index = id.roundBlockIndex;
    v.header->block_seq = id.blockSeq; v.header->block_ready = 1;
    m.state = uint32_t(RingInputSlotState::Ready);
    return Result::Ok;
}
// Withdraw only an exact still-ready publication. Mismatch means it may already
// have been copied; callers must resolve that via acknowledged(), not resubmit
// it with a different identity. A withdrawn id cannot be reused in this epoch.
inline Result withdraw(Mutex &mutex, void *memory, size_t bytes, uint64_t epoch,
                       uint64_t publicationId, uint32_t timeoutMs)
{
    if (!epoch || !publicationId) return Result::Invalid;
    Result r = acquire(mutex, timeoutMs, memory, bytes);
    if (r != Result::Ok) return r;
    Unlock unlock{mutex}; View v;
    if (!layout(memory, bytes, true, v)) return Result::Mismatch;
    if (v.meta->state != uint32_t(RingInputSlotState::Ready) || v.meta->epoch != epoch
        || v.meta->publication_id != publicationId) return Result::Mismatch;
    v.header->block_ready = 0; v.meta->state = uint32_t(RingInputSlotState::Empty);
    return Result::Ok;
}
// Poll after a successful publication before publishing the next block;
// only the most recent copied id is retained. notifications are hints, not acknowledgments.
inline Result acknowledged(Mutex &mutex, void *memory, size_t bytes, uint64_t epoch,
                           uint64_t publicationId, bool &copied, uint32_t timeoutMs)
{
    copied = false;
    if (!epoch || !publicationId) return Result::Invalid;
    Result r = acquire(mutex, timeoutMs, memory, bytes);
    if (r != Result::Ok) return r;
    Unlock unlock{mutex}; View v;
    if (!layout(memory, bytes, true, v) || v.meta->epoch != epoch) return Result::Mismatch;
    if (v.meta->state == uint32_t(RingInputSlotState::Poisoned)) return Result::Invalid;
    copied = v.meta->acknowledged_id == publicationId;
    return Result::Ok;
}
// Caller supplies independent buffers: block_size floats and alines angles/channels. Expected epoch/id
// come from the notification; stale or duplicate notifications cannot consume.
inline Result copyAndAcknowledge(Mutex &mutex, void *memory, size_t bytes, uint64_t epoch, uint64_t publicationId,
                                 float *raw, float *angles, uint8_t *channels, Identity &out, uint32_t timeoutMs)
{
    if (!raw || !angles || !channels || !epoch || !publicationId) return Result::Invalid;
    Result r = acquire(mutex, timeoutMs, memory, bytes);
    if (r != Result::Ok) return r;
    Unlock unlock{mutex}; View v;
    if (!layout(memory, bytes, true, v)) return Result::Mismatch;
    auto &m = *v.meta;
    if (m.state != uint32_t(RingInputSlotState::Ready) || m.epoch != epoch || m.publication_id != publicationId)
        return Result::Mismatch;
    std::memcpy(raw, v.data, size_t(v.header->block_size) * sizeof(float));
    std::memcpy(angles, v.data + ringAnglesOffset(int(v.header->block_size)), size_t(v.header->alines) * sizeof(float));
    std::memcpy(channels, v.data + ringChannelsOffset(int(v.header->block_size), int(v.header->alines)), v.header->alines);
    out = {m.epoch, m.publication_id, m.measurement_session, m.round_generation, m.submit_wall_us,
           m.block_seq, m.direction, m.source_round_complete != 0, m.round_block_index};
    m.acknowledged_id = m.publication_id; v.header->block_ready = 0;
    m.state = uint32_t(RingInputSlotState::Empty);
    return Result::Ok;
}
} // namespace ring_input
