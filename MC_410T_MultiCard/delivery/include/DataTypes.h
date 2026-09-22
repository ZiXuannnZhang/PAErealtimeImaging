#pragma once
#include <cstdint>
#include <vector>
#include <memory>
#include <atomic>
#include <string>
#include "Constants.h"
#include "RoundIdentity.h"
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
    // One-shot boundary pulse from the first classification of the final
    // logical trigger. Kept for control/event compatibility paths.
    bool roundComplete = false;
    // Stable per-trigger terminal property (true on first and cached
    // classifications alike). The Ring data plane must source its final
    // marker from this field, never from the one-shot pulse.
    bool isFinalLogicalTrigger = false;
    bool sourceTimedOut = false;

    // The normalized Ring path consumes this pair as one immutable identity.
    // Keeping the legacy scalar fields preserves the existing save contract
    // and makes the shared identity explicit at the Ring boundary.
    paimage::RoundIdentity physicalRoundIdentity() const noexcept {
        return {measurementSession, roundGeneration};
    }

    bool hasPhysicalRoundIdentity() const noexcept {
        return normalizationApplied &&
               physicalDecision == paimage::PhysicalTriggerDecision::LogicalScan &&
               physicalRoundIdentity().valid();
    }

    // ── 载荷语义核实记录（2026-09-23，本轮滤波前置核实）────────────────
    // 结论：freqA/freqB 的实际内容是【零均值双向振荡的时域波形幅度】，
    // 不是 Δφ 相位量，也不是瞬时频率（kHz）。保存、显示、实时成像三条下游
    // 消费的是同一份数据，语义一致。
    //
    // 证据 1 —— 落盘样本实测（testdata/01/Card*_Ch*_test_*.dat，FileSaver
    //   float16 无头格式，各抽 20 万点）：
    //   * 100% 为整数值、最小非零幅度 = 1 → decodeRaw 的 float(int16) 透传，
    //     未施加任何 Δφ→kHz 缩放。若走 ogprog/MC_410T 的 computeFrequency
    //     （×1.214288），落盘应为 1.214288 的非整数倍，实测不符。
    //   * mean/std ≈ -0.002、负值占比 49%~51%、每 5000 点分段均值在 ±3 内
    //     抖动而 std ≈ 500、过零率 0.19~0.49 → 典型双向振荡波形。
    //   * 反证：若为实信号的 Δφ（= 2πf/fs > 0），样本应几乎全为正号且均值
    //     显著非零、几乎不过零，与上列实测直接矛盾。
    // 证据 2 —— MATLAB 参照 实时重建脚本/preprocessBlock.m 对
    //   bscan[SampDepth × nBlock] 做 butter + filtfilt 时域带通后再送 DAS
    //   波束形成；DAS 依赖传播延迟，输入必须是时域波形量，频率类曲线无法成像。
    // 证据 3 —— DataProcessor::computeFrequency 现为 identity 透传，与上述一致。
    //
    // 与之冲突但已过时的命名（本次按要求【不改既有注释】，仅补充记录）：
    //   下方 "瞬时频率（kHz）"、Constants.h 的 FREQ_SCALE_KHZ 契约、
    //   phase*_display 的 "差分相位" 叫法、UI 的 "相位(rad)/瞬时频率" 标签，
    //   均来自 ogprog/MC_410T 时代「FPGA 输出 Δφ Q0.15（满量程 π rad）」的
    //   旧契约。新代码请按"时域幅度"理解，勿用 FREQ_SCALE_KHZ 做换算。
    //
    // 已知连带问题（本轮搁置，另行处理）：FrontendPreprocessor::
    // prepareDisplayData 仍用旧的 "kHz→rad" 积分系数累加 freqA 得 phase*_display，
    // 因 computeFrequency 已去掉配对的 scale，该曲线既非差分相位也非累积相位。
    //
    // 对滤波的影响：高/低通零相位滤波按"时域幅度的 A-line"设计，逐 A-line
    // 独立作用，唯一插入点 FrontendPreprocessor::processFrontendSignal()。
    // ────────────────────────────────────────────────────────────────

    //  完整采样数据（float32，sampleCount 个点）
    std::vector<float> freqA;         // A 通道瞬时频率（kHz）
    std::vector<float> freqB;         // B 通道瞬时频率（kHz）

    //  显示用全分辨率数据（float32，sampleCount 个点；不做显示抽点）
    std::vector<float> phaseA_display;
    std::vector<float> phaseB_display;
    std::vector<float> freqA_display;
    std::vector<float> freqB_display;

    void allocate(int n) {
        freqA.resize(n);       freqB.resize(n);
        phaseA_display.resize(n); phaseB_display.resize(n);
        freqA_display.resize(n);  freqB_display.resize(n);
    }
    // 兼容旧测试/辅助代码的两参数调用；第二参数不再控制显示抽点。
    void allocate(int n, int /*legacyDisplayPts*/) { allocate(n); }

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
        isFinalLogicalTrigger = false;
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
    // 跳号数：由 forward triggerSeq gap 推断出的、完全 0 包到达的 missing
    // trigger 累计数量（per-card observability；T100->T104 记 +3）。
    // 部分到达的 trigger 只计入 triggersPartial/包级丢包，不在此计数。
    std::atomic<uint64_t> missingTriggerCount{0};
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
        uint64_t missingTriggerCount = 0;   // 跳号数：完全 0 包到达的 missing trigger 累计
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
        s.missingTriggerCount = missingTriggerCount.load(std::memory_order_relaxed);
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
