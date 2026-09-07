#pragma once
#include <cstdint>
#include <vector>
#include <memory>
#include <atomic>
#include <string>
#include "Constants.h"

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
        freqA.clear(); freqB.clear();
        phaseA_display.clear(); phaseB_display.clear();
        freqA_display.clear();  freqB_display.clear();
    }
};

using TriggerGroupPtr = std::shared_ptr<TriggerGroup>;

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
        return s;
    }

    // 禁止拷贝（含 atomic 成员）
    CardStats() = default;
    CardStats(const CardStats&) = delete;
    CardStats& operator=(const CardStats&) = delete;
};
