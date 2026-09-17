#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <list>
#include <vector>
#include "PaimageAcquisition/SocketTimestamp.h"

// PAimage-derived: recovered names, not original symbols. See the behavior map.
// Single receiver-thread ownership. No socket, host session fence, or old assembler.
namespace paimage {
using Time = std::int64_t; // steady-clock nanoseconds
enum class Decision : std::uint16_t { Accepted, Short, Disabled, RecentTrigger,
    Duplicate, OffsetOutside, Complete, TriggerSwitch, Timeout, StartupIdleClear,
    StartupOverflow, StartupConfirmed, SyncExpired, StopTruncated, StartupBuffered,
    CardOutput, SyncOutput, InvalidCard, StartupOverflowDiscard, StopBufferedDiscard,
    ListenerActiveDiscard, ListenerBufferedDiscard, ListenerPendingSyncDiscard,
    StartPendingSyncDiscard, StartActiveDiscard, StartupActiveDiscard, StartupPendingSyncDiscard,
    CompleteStartPendingSyncDiscard, CompleteStartActiveDiscard,
    StartFencePreStartDiscard, StartFenceHeld, StartFenceReleased,
    StartFenceFailedDiscard, StartFenceOverflow, StartFenceResetDiscard,
    StartFenceStopDiscard, StartFenceShutdownDiscard, TriggerGap };
struct Observation {
    Decision decision{}; int card=-1; std::uint16_t trigger=0, packet=0;
    std::uint32_t count=0; Time time=0; std::uint64_t firstIngressId=0;
};
struct CardFrame {
    int card=0; std::uint16_t trigger=0, basePacket=0;
    std::uint32_t actual=0, unique=0, expected=0;
    Time first=0,last=0,closed=0; Decision reason{}; bool complete=false;
    std::uint64_t measurementSession=0,firstIngressId=0; // observation, never admission
    std::uint32_t sourceIPv4=0;
    std::vector<std::uint16_t> lengths;
    std::vector<std::uint32_t> seen;
    std::vector<std::uint8_t> bytes; // zero-filled, fixed byte slots
};
using Frame=std::shared_ptr<const CardFrame>;
// VA 0138bb defaults to 0; UI modes 0..4 set 1000 at 03ad80.
// The caller must explicitly select the recovered UI-mode setting.
struct Config {
    int cards=4, samples=5000, bits=32, startupIdleMs=0;
    SocketTimestampMode socketTimestampMode=SocketTimestampMode::Off;
};
struct Counters {
    std::uint64_t startupFilteredCards=0,startupFilteredSync=0;
    std::uint64_t startupIncomplete=0,runtimeIncomplete=0,completeCards=0;
    std::uint64_t stopActiveCards=0,stopBufferedCards=0,stopBufferedSync=0;
    std::uint64_t startFenceHeld=0,startFenceReleased=0;
    std::uint64_t startFencePreStartDiscard=0,startFenceFailedDiscard=0;
    std::uint64_t startFenceOverflow=0,startFenceResetDiscard=0;
    std::uint64_t startFenceStopDiscard=0,startFenceShutdownDiscard=0;
};
class SourceCore {
public:
    using Observer=std::function<void(const Observation&)>;
    using CardSink=std::function<void(Frame)>;
    using SyncSink=std::function<void(std::uint16_t,const std::vector<Frame>&,bool startupRelease)>;
    SourceCore(Config,CardSink,SyncSink,Observer={});
    void prepareStart(std::uint64_t diagnosticSession=0,Time now=0);
    void completeStart(bool success, Time now);
    void prepareStop();
    void completeStop(bool success,Time now);
    Decision ingest(int card,const std::uint8_t*,std::size_t,Time,std::uint64_t ingressId=0,std::uint32_t sourceIPv4=0);
    // Record an admission decision made before ingest without manufacturing a
    // second raw-ingress record or applying assembly logic.
    void observeAdmission(Decision,int,std::uint16_t,std::uint16_t,std::uint32_t,Time,
                          std::uint64_t firstIngressId=0);
    void poll(Time);
    void observeShutdown(Time); // observation only, no state transition
    bool enabled() const { return enabled_; }
    bool confirmed() const { return confirmed_; }
    const Counters& counters() const { return counts_; }
    std::size_t startupCardCount() const { return startupCards_.size(); }
private:
    struct Assembly {
        bool active=false; std::uint16_t trigger=0,base=0;
        std::uint32_t actual=0,unique=0; Time first=0,last=0;
        std::uint64_t firstIngressId=0;
        std::uint32_t sourceIPv4=0;
        std::vector<std::uint32_t> seen; std::vector<std::uint16_t> lengths;
        std::vector<std::uint8_t> bytes; std::deque<std::uint16_t> recent;
        // Full-trigger-gap anchor: last forward-accepted (or reset-recovered)
        // trigger on this card. Shares the recent-window lifetime; session
        // resets clear both.
        std::uint16_t gapAnchor=0; bool gapAnchorValid=false;
    };
    // Same-session triggerSeq reset-recovery threshold for the gap tracker:
    // a large direct back-jump (>= this value) re-establishes the gap anchor
    // without counting a gap for the reset transition itself. Same value and
    // frozen semantics as DataProcessor::kTriggerResetBackJumpThreshold.
    static constexpr std::int32_t kTriggerResetBackJumpThreshold=256;
    struct Pending { std::uint16_t trigger=0; Time first=0; std::vector<Frame> cards; };
    std::list<Pending>::iterator findOrInsertPending(std::uint16_t,Time);
    void clearAssembly(Assembly&,bool recent=false);
    void clearStartup(Time,bool count,Decision reason=Decision::StartupIdleClear);
    void close(int,Decision,Time);
    void deliverCard(Frame,Time);
    void deliverSync(std::uint16_t,std::vector<Frame>,Time);
    void event(Decision,int,std::uint16_t,std::uint16_t,std::uint32_t,Time,std::uint64_t firstIngressId=0);
    Config config_; int expected_=0; std::size_t bytes_=0;
    std::uint64_t diagnosticSession_=0;
    bool enabled_=false,confirmed_=true; Time lastStartup_=0,firstSync_=0;
    std::size_t startupBytes_=0; Counters counts_;
    std::vector<Assembly> cards_; std::list<Pending> pending_;
    std::size_t bucketCount_=8; // source MSVC hash order; clear does not shrink buckets
    std::vector<Frame> startupCards_;
    std::deque<std::pair<std::uint16_t,std::vector<Frame>>> startupSync_;
    CardSink cardSink_; SyncSink syncSink_; Observer observer_;
};
using Command=std::array<std::uint8_t,58>;
Command configCommand(std::int32_t durationNs,std::int32_t delayA,std::int32_t delayB);
Command startCommand();
Command stopCommand();
int feedbackType(const std::uint8_t*,std::size_t);
// Host numerical contract: one integer->float conversion, no normalization.
void decodeRaw(const CardFrame&,int bits,std::vector<float>& a,std::vector<float>& b);
}
