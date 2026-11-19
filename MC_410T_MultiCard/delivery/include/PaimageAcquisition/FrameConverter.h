#pragma once
#include "SourceCore.h"
#include "DataTypes.h"
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <optional>
#include <functional>
namespace paimage {
// Host-only adapter. Saving and sync workers share one integer->float conversion
// for a source object, without putting conversion back on the receive thread.
class FrameConverter {
public:
    FrameConverter(int bits,std::uint64_t wallMs,Time monotonic,
                   std::function<void()> afterSnapshot = {})
        :bits_(bits),wallMs_(wallMs),monotonic_(monotonic),afterSnapshot_(std::move(afterSnapshot)){}
    TriggerGroupPtr convert(Frame);
    TriggerGroupPtr convertForSave(Frame);
    void tagSaveOverride(Frame,std::uint64_t session,std::uint64_t round,std::uint64_t generation,bool late);
    void tagSaveSession(Frame, std::uint64_t, const std::shared_ptr<const std::string>& captureId = {});
    void tagNormalization(Frame,const PhysicalRoundClassification&);
    std::uint64_t conversions()const{return conversions_.load();}
private:
    struct Entry {
        std::weak_ptr<const CardFrame> source;
        std::once_flag once;
        TriggerGroupPtr group;
        std::uint64_t saveSession=0;
        std::shared_ptr<const std::string> captureId;
        std::optional<PhysicalRoundClassification> normalization;
        struct SaveOverride {std::uint64_t session,round,generation;bool late;};
        std::optional<SaveOverride> saveOverride;
        // Written by call_once, read only after call_once has returned.
        std::optional<SaveOverride> frozenSaveOverride;
    };
    TriggerGroupPtr convert(Frame,std::optional<Entry::SaveOverride>*);
    int bits_;std::uint64_t wallMs_;Time monotonic_;
    // Optional synchronization seam for conversion contract tests; invoked
    // without mutex_ and before decoding.
    std::function<void()> afterSnapshot_;
    std::mutex mutex_;std::unordered_map<const CardFrame*,std::shared_ptr<Entry>> entries_;
    unsigned inserted_=0;std::atomic<std::uint64_t> conversions_{0};
};
}
