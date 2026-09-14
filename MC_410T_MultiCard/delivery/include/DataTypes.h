#pragma once
#include <cstdint>
#include <vector>
#include <memory>
#include <atomic>
#include <string>
#include "Constants.h"
#include "PaimageAcquisition/PhysicalRoundNormalizer.h"

// ============================================================
// DataPacket  Layer 2  Layer 3 的数据包（接收线程  处理线程）
// ============================================================
struct DataPacket {
    uint16_t packetSeq  = 0;                       // 包序号（0-based，单次触发内）
    uint16_t triggerSeq = 0;                       // 触发序号（uint16 回绕，200Hz 时 327s 回绕）
    int      cardIndex  = -1;                      // 卡号（0-based）
    uint16_t dataSize   = UDP_PAYLOAD_BYTES;       // 有效载荷字节数（最后一包可能 < 1440）
    uint32_t sourceIPv4 = 0;                       // 源 IP（uint32 网络字节序），FileSaver 用于判断是否切换文件
    uint8_t  data[UDP_PAYLOAD_BYTES] = {};         // 1440 字节原始载荷（B/A 交织 int16）
    // 由 DataProcessor::enqueuePacket 在进程侧打标。0 表示 gate 关闭期间的旧包。
    uint64_t measurementSessionToken = 0;
};

// ============================================================
// TriggerGroup  一次触发的完整数据（Layer 3 内部）
// ============================================================
struct TriggerGroup {
    int      cardId       = -1;       // 卡号（0-based）
    uint16_t triggerSeq   = 0;        // 触发序号
    int      sampleCount  = 0;        // 本次触发实际接收的采样点数
    uint64_t timestamp_ms = 0;        // 第一包到达的时间戳（ms since epoch）
    bool     isComplete   = true;     // 是否收到全部包（无丢包）
    uint32_t sourceIPv4   = 0;        // 数据源 IP（uint32 大端序，比较用）
    uint64_t sessionGen   = 0;        // 自动保存会话代（0=手动/无会话代；DataProcessor 入队前打标）
    uint64_t measurementSession = 0;  // 采集会话令牌；成像旁路用来拒绝旧会话帧

    // PAimage physical-round normalization metadata.  The source frame is
    // immutable, so FrameConverter copies this decision onto the host group
    // before either the save or sync worker can consume it.
    bool normalizationApplied = false;
    paimage::PhysicalTriggerDecision physicalDecision =
        paimage::PhysicalTriggerDecision::LogicalScan;
    uint64_t roundGeneration = 0;
    int64_t logicalTriggerIndex = -1;
    bool roundComplete = false;
    bool sourceTimedOut = false;

    //  完整采样数据（float32，sampleCount 个点）
    std::vector<float> freqA;         // A 通道瞬时频率（kHz）
    std::vector<float> freqB;         // B 通道瞬时频率（kHz）

    //  显示用降采样数据（float32，displayPoints 个点）
    std::vector<float> phaseA_display;
    std::vector<float> phaseB_display;
    std::vector<float> freqA_display;
    std::vector<float> freqB_display;

    void allocate(int n, int displayPts) {
        freqA.resize(n);       freqB.resize(n);
        phaseA_display.resize(displayPts); phaseB_display.resize(displayPts);
        freqA_display.resize(displayPts);  freqB_display.resize(displayPts);
    }

    void reset() {
        cardId = -1; triggerSeq = 0; sampleCount = 0;
        timestamp_ms = 0; isComplete = true; sourceIPv4 = 0;
        sessionGen = 0;
        measurementSession = 0;
        normalizationApplied = false;
        physicalDecision = paimage::PhysicalTriggerDecision::LogicalScan;
        roundGeneration = 0;
        logicalTriggerIndex = -1;
        roundComplete = false;
        sourceTimedOut = false;
        freqA.clear(); freqB.clear();
        phaseA_display.clear(); phaseB_display.clear();
        freqA_display.clear();  freqB_display.clear();
    }
};

using TriggerGroupPtr = std::shared_ptr<TriggerGroup>;
using TriggerGroupConstPtr = std::shared_ptr<const TriggerGroup>;

// ============================================================
// SyncFrame  多卡同步帧（FramePublisher  ZeroMQ 发布）
// ============================================================
struct SyncFrame {
    uint16_t triggerSeq    = 0;
    int      nCards        = 0;
    int      samplesPerCard = 0;
    uint64_t timestamp_ms  = 0;
    // 布局：[Card0_A, Card0_B, Card1_A, Card1_B, ...]
    // 每通道 samplesPerCard 个 float32
    std::vector<float> data;
};

// ============================================================
// CardStats  单卡运行统计
// ============================================================
struct CardStats {
    int cardId = -1;

    //  热路径字段（由 DataProcessor 处理线程更新，relaxed atomic）
    std::atomic<uint64_t> packetsReceived{0};
    std::atomic<uint64_t> packetsDropped{0};
    std::atomic<uint64_t> triggersComplete{0};
    std::atomic<uint64_t> triggersPartial{0};
    std::atomic<uint64_t> triggersDiscarded{0};
    std::atomic<uint64_t> saveQueueDiscards{0};  // 存储队列满导致的丢弃（triggersDiscarded 子集）
    // 分层采集计数：socket 成功接收、processor 成功出队，以及
    // batch quota 边界哨兵。最后一个计数在修复后的正常路径应始终为 0。
    std::atomic<uint64_t> socketPacketsReceived{0};
    std::atomic<uint64_t> socketBytesReceived{0};
    std::atomic<uint64_t> processorPacketsDequeued{0};
    std::atomic<uint64_t> batchBoundaryDiscards{0};

    // 网络入口/组包观测字段。它们只记录既有控制流已经做出的判断，
    // 不参与接收、排队、组包或丢包决策。
    std::atomic<uint64_t> sameTriggerForwardGapEvents{0};
    std::atomic<uint64_t> sameTriggerForwardGapPackets{0};
    std::atomic<uint64_t> sameTriggerBackstepEvents{0};
    std::atomic<uint64_t> sameTriggerDuplicateSeqEvents{0};
    std::atomic<uint64_t> crossTriggerLateArrivalEvents{0};
    std::atomic<uint64_t> staleTriggerPacketsDiscarded{0};
    std::atomic<uint64_t> assemblyDuplicatePackets{0};
    std::atomic<uint64_t> assemblyOffsetOutOfRangePackets{0};
    // Valid datagrams observed by the receiver while the session admission
    // gate was closed.  This is cumulative observability, not a processor
    // loss counter and is never folded into packetsDropped.
    std::atomic<uint64_t> sessionBoundaryPacketsDiscarded{0};
    std::atomic<uint16_t> rawLastTriggerSeq{0};
    std::atomic<uint16_t> rawLastPacketSeq{0};
    std::atomic<bool> rawSequenceInitialized{false};

    // 由 MultiPortReceiver 在解析完包头后调用；该函数只做整数比较和
    // relaxed atomic 累计，不调用系统 API、不格式化字符串、不写磁盘。
    void observeRawReceive(uint16_t triggerSeq, uint16_t packetSeq) {
        if (!rawSequenceInitialized.load(std::memory_order_relaxed)) {
            rawLastTriggerSeq.store(triggerSeq, std::memory_order_relaxed);
            rawLastPacketSeq.store(packetSeq, std::memory_order_relaxed);
            rawSequenceInitialized.store(true, std::memory_order_relaxed);
            return;
        }

        const uint16_t latestTrigger = rawLastTriggerSeq.load(std::memory_order_relaxed);
        const uint16_t latestPacket = rawLastPacketSeq.load(std::memory_order_relaxed);
        const int16_t triggerDelta = static_cast<int16_t>(triggerSeq - latestTrigger);
        if (triggerDelta < 0) {
            crossTriggerLateArrivalEvents.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        if (triggerDelta == 0) {
            const uint16_t packetDelta = static_cast<uint16_t>(packetSeq - latestPacket);
            if (packetDelta == 0) {
                sameTriggerDuplicateSeqEvents.fetch_add(1, std::memory_order_relaxed);
            } else if (packetDelta < 0x8000U) {
                if (packetDelta > 1U) {
                    sameTriggerForwardGapEvents.fetch_add(1, std::memory_order_relaxed);
                    sameTriggerForwardGapPackets.fetch_add(
                        static_cast<uint64_t>(packetDelta - 1U), std::memory_order_relaxed);
                }
            } else {
                sameTriggerBackstepEvents.fetch_add(1, std::memory_order_relaxed);
            }
        }

        // 只把当前最新 trigger 的最后一个包序号作为下一次同 trigger
        // 比较锚点；旧 trigger 迟到包不会污染后续判断。
        if (triggerDelta >= 0) {
            rawLastTriggerSeq.store(triggerSeq, std::memory_order_relaxed);
            rawLastPacketSeq.store(packetSeq, std::memory_order_relaxed);
        }
    }

    // Measurement session 边界只重置 raw-order anchor，不清除累计计数。
    // 该操作由 DataProcessor worker 在 prepare/disarm barrier 内调用。
    void resetRawSequenceAnchor() {
        rawLastTriggerSeq.store(0, std::memory_order_relaxed);
        rawLastPacketSeq.store(0, std::memory_order_relaxed);
        rawSequenceInitialized.store(false, std::memory_order_relaxed);
    }

    //  速率字段（由主线程 1Hz 采样更新，无需 atomic）
    double recvMbps        = 0.0;
    double triggerHz       = 0.0;
    double packetLossRate  = 0.0;
    uint64_t lastUpdateMs  = 0;

    //  队列深度（约 30fps 采样）
    int inputQueueDepth    = 0;
    int saveQueueDepth     = 0;

    //  快照结构（主线程安全读取）
    struct Snapshot {
        int      cardId          = -1;
        uint64_t packetsReceived = 0;
        uint64_t packetsDropped  = 0;
        uint64_t triggersComplete = 0;
        uint64_t triggersPartial  = 0;
        double   recvMbps         = 0.0;
        double   triggerHz        = 0.0;
        double   packetLossRate   = 0.0;
        int      inputQueueDepth  = 0;
        int      saveQueueDepth   = 0;
        uint64_t triggersDiscarded = 0;
        uint64_t saveQueueDiscards = 0;  // 存储队列满丢弃（可与 triggersDiscarded 对比诊断根因）
        uint64_t socketPacketsReceived = 0;
        uint64_t processorPacketsDequeued = 0;
        uint64_t batchBoundaryDiscards = 0;
        uint64_t sameTriggerForwardGapEvents = 0;
        uint64_t sameTriggerForwardGapPackets = 0;
        uint64_t sameTriggerBackstepEvents = 0;
        uint64_t sameTriggerDuplicateSeqEvents = 0;
        uint64_t crossTriggerLateArrivalEvents = 0;
        uint64_t staleTriggerPacketsDiscarded = 0;
        uint64_t assemblyDuplicatePackets = 0;
        uint64_t assemblyOffsetOutOfRangePackets = 0;
        uint64_t sessionBoundaryPacketsDiscarded = 0;
        uint16_t lastTriggerSeq = 0;
        uint16_t lastPacketSeq = 0;
        bool rawSequenceInitialized = false;
    };

    Snapshot snapshot() const {
        Snapshot s;
        s.cardId           = cardId;
        s.packetsReceived  = packetsReceived.load(std::memory_order_relaxed);
        s.packetsDropped   = packetsDropped.load(std::memory_order_relaxed);
        s.triggersComplete = triggersComplete.load(std::memory_order_relaxed);
        s.triggersPartial  = triggersPartial.load(std::memory_order_relaxed);
        s.recvMbps         = recvMbps;
        s.triggerHz        = triggerHz;
        s.packetLossRate   = packetLossRate;
        s.inputQueueDepth  = inputQueueDepth;
        s.saveQueueDepth   = saveQueueDepth;
        s.triggersDiscarded = triggersDiscarded.load(std::memory_order_relaxed);
        s.saveQueueDiscards = saveQueueDiscards.load(std::memory_order_relaxed);
        s.socketPacketsReceived = socketPacketsReceived.load(std::memory_order_relaxed);
        s.processorPacketsDequeued = processorPacketsDequeued.load(std::memory_order_relaxed);
        s.batchBoundaryDiscards = batchBoundaryDiscards.load(std::memory_order_relaxed);
        s.sameTriggerForwardGapEvents = sameTriggerForwardGapEvents.load(std::memory_order_relaxed);
        s.sameTriggerForwardGapPackets = sameTriggerForwardGapPackets.load(std::memory_order_relaxed);
        s.sameTriggerBackstepEvents = sameTriggerBackstepEvents.load(std::memory_order_relaxed);
        s.sameTriggerDuplicateSeqEvents = sameTriggerDuplicateSeqEvents.load(std::memory_order_relaxed);
        s.crossTriggerLateArrivalEvents = crossTriggerLateArrivalEvents.load(std::memory_order_relaxed);
        s.staleTriggerPacketsDiscarded = staleTriggerPacketsDiscarded.load(std::memory_order_relaxed);
        s.assemblyDuplicatePackets = assemblyDuplicatePackets.load(std::memory_order_relaxed);
        s.assemblyOffsetOutOfRangePackets = assemblyOffsetOutOfRangePackets.load(std::memory_order_relaxed);
        s.sessionBoundaryPacketsDiscarded = sessionBoundaryPacketsDiscarded.load(std::memory_order_relaxed);
        s.lastTriggerSeq = rawLastTriggerSeq.load(std::memory_order_relaxed);
        s.lastPacketSeq = rawLastPacketSeq.load(std::memory_order_relaxed);
        s.rawSequenceInitialized = rawSequenceInitialized.load(std::memory_order_relaxed);
        return s;
    }

    // 禁止拷贝（含 atomic 成员）
    CardStats() = default;
    CardStats(const CardStats&) = delete;
    CardStats& operator=(const CardStats&) = delete;
};
