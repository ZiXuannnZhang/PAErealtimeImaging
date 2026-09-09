#pragma once
#include "SourceCore.h"
#include "DataTypes.h"
#include <atomic>
#include <mutex>
#include <unordered_map>
namespace paimage {
// Host-only adapter. Saving and sync workers share one integer->float conversion
// for a source object, without putting conversion back on the receive thread.
class FrameConverter {
public:
    FrameConverter(int bits,std::uint64_t wallMs,Time monotonic):bits_(bits),wallMs_(wallMs),monotonic_(monotonic){}
    TriggerGroupPtr convert(Frame);
    void tagSaveSession(Frame,std::uint64_t);
    std::uint64_t conversions()const{return conversions_.load();}
private:
    struct Entry {std::weak_ptr<const CardFrame> source;std::once_flag once;TriggerGroupPtr group;std::uint64_t saveSession=0;};
    int bits_;std::uint64_t wallMs_;Time monotonic_;
    std::mutex mutex_;std::unordered_map<const CardFrame*,std::shared_ptr<Entry>> entries_;
    unsigned inserted_=0;std::atomic<std::uint64_t> conversions_{0};
};
}
