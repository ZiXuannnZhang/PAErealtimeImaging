#pragma once
#include <QThread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include "DataTypes.h"
#include "AcqConfig.h"
#include "DisplayBuffer.h"
#include "PacketAssemblyBuffer.h"
#include "third_party/concurrentqueue.h"

// 前向声明
class FileSaver;
class FramePublisher;

// ============================================================
// DataProcessor  单卡数据处理线程
//
// 职责：
//   1. 消费 MultiPortReceiver 入队的 DataPacket
//   2. 用 PacketAssemblyBuffer 重组完整触发
//   3. computeFrequency：int16 Q0.15 差分相位  float32 kHz
//   4. downsample：sampleCount  displayPoints
//   5. 三路分发：FileSaver / FramePublisher / DisplayBuffer
// ============================================================
class DataProcessor : public QThread {
    Q_OBJECT

public:
    explicit DataProcessor(
        int cardId,
        moodycamel::ConcurrentQueue<TriggerGroupPtr>* saveQueue,  // FileSaver 队列指针（可为 nullptr）
        DisplayBuffer* displayBuf,
        FramePublisher* publisher,        // 可为 nullptr
        const AcqConfig& config,
        QObject* parent = nullptr);

    ~DataProcessor() override;

    //  热路径接口（接收线程调用，无锁入队 + 唤醒）
    void enqueuePacket(const DataPacket& pkt);

    //  停止：发出中断请求并立即唤醒等待中的条件变量
    void requestStop() {
        requestInterruption();
        m_wakeCv.notify_all();
    }

    //  采集控制 
    void setSaveEnabled(bool enable) { m_saveEnabled = enable; }
    bool isSaveEnabled() const       { return m_saveEnabled; }
    // 更新采集参数（停止测量后、重新开始前调用，线程安全）
    void updateConfig(const AcqConfig& config) {
        m_config = config;
        m_expectedPackets = config.packetsPerTrig();
        m_displayPoints.store(config.displayPoints, std::memory_order_relaxed);
    }
    // 实时更新显示点数（主线程安全，atomic写入）
    void setDisplayPoints(int points) {
        if (points > 0) m_displayPoints.store(points, std::memory_order_relaxed);
    }

    //  统计查询（主线程调用，线程安全）
    CardStats::Snapshot statsSnapshot() const { return m_stats.snapshot(); }
    CardStats&          stats()               { return m_stats; }
    int   inputQueueDepth() const;
    int   cardId()          const { return m_cardId; }

protected:
    void run() override;

private:
    // int16 Q0.15 差分相位序列  float32 kHz（差分法，O(N)，禁止用 FFT）
    // 同时计算相位（累积积分，rad 单位，填入 group 的 phaseA/B_display 前）
    void computeFrequency(TriggerGroup& group);

    // sampleCount  displayPoints 均匀降采样（填写 *_display 字段）
    void downsample(TriggerGroup& group, int displayPoints);

    // 将 assemblyBuf 当前内容 export → compute → 分发（供正常完成和强制 flush 共用）
    void flushAssemblyBuf(PacketAssemblyBuffer& assemblyBuf);

    //  成员 
    int              m_cardId;
    AcqConfig        m_config;
    bool             m_saveEnabled = false;
    // 原始：使用静态 MAX_SAVE_QUEUE 常量在 cpp 中控制
    std::atomic<int> m_displayPoints{1000};  // 显示降采样点数（主线程可实时修改）

    moodycamel::ConcurrentQueue<DataPacket>       m_inputQueue;
    moodycamel::ConcurrentQueue<TriggerGroupPtr>* m_saveQueue     = nullptr;
    DisplayBuffer*                                m_displayBuffer  = nullptr;
    FramePublisher*                               m_framePublisher = nullptr;

    // 条件变量唤醒（接收线程写数据后通知处理线程）
    std::mutex              m_wakeMtx;
    std::condition_variable m_wakeCv;
    std::atomic<bool>       m_hasData{false};

    CardStats  m_stats;
    // 丢包统计锚点：用触发序号取代包序号跟踪，对乱序完全不敏感
    uint16_t   m_lastFlushedTriggerSeq = 0;     // 最后完成 flush 的触发序号
    bool       m_hasFlushedOnce        = false;  // 首次 flush 前不做回溯统计（避免启动误判）
    int        m_expectedPackets       = 0;      // 一次触发期望包数

    // 连续丢弃恢复：当 FPGA 重置触发序号时，m_lastFlushedTriggerSeq 持有旧值
    // 导致所有新包均被 step1 丢弃（带宽正常但触发数停止）。
    // 超过阈值后强制重置锚点，快速恢复。
    int        m_consecutiveDiscards   = 0;
};
