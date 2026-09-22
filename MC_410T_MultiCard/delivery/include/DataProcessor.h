#pragma once
#include <QThread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <deque>
#include <memory>
#include "DataTypes.h"
#include "AcqConfig.h"
#include "FrontendPreprocessor.h"
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
//   4. 三路分发：FileSaver（raw）/ FramePublisher（raw）/ FrontendPreprocessor
//
// 全分辨率显示准备、DisplayBuffer 与 Ring 的 frontend 分发全部属于
// FrontendPreprocessor（每卡一个异步 stage）。DataProcessor 不再直接持有或
// 调用 DisplayBuffer / RingFeedSink，也不再修改 shared raw group 的 *_display。
// ============================================================
class DataProcessor : public QThread {
    Q_OBJECT

public:
    // Frontend enqueue sink: HostOutput/DataProcessor 热路径只做 enqueue。
    // 返回值只描述 frontend queue 的准入结果，不代表异步 Ring 最终结果。
    using FrontendSubmitSink = std::function<FrontendSubmitResult(const TriggerGroupPtr&)>;

    explicit DataProcessor(
        int cardId,
        moodycamel::ConcurrentQueue<TriggerGroupPtr>* saveQueue,  // FileSaver 队列指针（可为 nullptr）
        FramePublisher* publisher,        // 可为 nullptr
        const AcqConfig& config,
        const FrontendSubmitSink& frontendSubmitSink = {},  // 可为空
        QObject* parent = nullptr);

    ~DataProcessor() override;

    //  热路径接口（接收线程调用，无锁入队 + 唤醒）
    void enqueuePacket(const DataPacket& pkt);
    // Receiver-owned admission path.  The receiver supplies the session
    // epoch explicitly; DataProcessor never infers a new epoch from a late
    // packet's enqueue time.
    void enqueuePacketForSession(const DataPacket& pkt, uint64_t sessionToken);

    // Already assembled PAimage output. This entry never touches packet queues,
    // PacketAssemblyBuffer, StartFence, or numerical conversion. The source
    // saving worker uses save=true; its sync worker uses frontend=true.
    //
    // save 与 frontend submit 严格隔离：raw save 使用原始 group，frontend 只做
    // enqueue（deep copy 在 FrontendPreprocessor worker 内完成）。任一侧失败都
    // 不改写另一侧的结果，也不会计入 UDP 丢包 / missingTriggerCount /
    // saveQueueDiscards。
    struct DeliveryResult {
        enum Save { NotRequested, Disabled, Queued, QueueFull, QueueFailure, Consumed, ConsumerFailure } save=NotRequested;
        bool saveAccepted=false;
        // Frontend queue admission only.  The asynchronous Ring outcome is
        // owned by ImagingBypass/Ring statistics and is never reported here.
        bool frontendAccepted=false;
        FrontendSubmitResult frontendSubmit=FrontendSubmitResult::Stopping;
        bool publisherAccepted=false, exception=false;
    };
    DeliveryResult deliverAssembled(const TriggerGroupPtr&,bool save,bool frontend);
    // Set before starting source workers. No second saving queue in this path.
    void setDirectSaveSink(std::function<bool(const TriggerGroupPtr&)> sink) { m_directSaveSink=std::move(sink); }
    std::uint64_t captureSaveSessionGen()const{return m_sessionGenReader?m_sessionGenReader():0;}

    //  停止：发出中断请求并立即唤醒等待中的条件变量
    void requestStop() {
        requestInterruption();
        m_wakeCv.notify_all();
    }

    //  采集控制 
    void setSaveEnabled(bool enable) { m_saveEnabled.store(enable); }
    bool isSaveEnabled() const       { return m_saveEnabled.load(); }
    // 自动保存会话代读取器（入队前调用，把当前会话代打在触发组上；
    // 未设置时触发组 sessionGen=0，即手动模式）
    void setSessionGenReader(std::function<uint64_t()> reader) { m_sessionGenReader = std::move(reader); }
    // 兼容旧测试/调用方的简单门控。生产 Start/Stop 必须使用下面的
    // prepareSession -> armSession -> disarmSession barrier。
    void setMeasureEnabled(bool enable) {
        if (enable) {
            m_ingressSessionToken.store(1, std::memory_order_release);
            m_activeSessionToken.store(1, std::memory_order_release);
        } else {
            m_ingressSessionToken.store(0, std::memory_order_release);
            m_activeSessionToken.store(0, std::memory_order_release);
        }
        m_measureEnabled.store(enable, std::memory_order_relaxed);
    }
    bool isMeasureEnabled() const {
        return m_measureEnabled.load(std::memory_order_relaxed);
    }

    // Measurement session lifecycle.  These calls synchronously wait for the
    // DataProcessor worker to own and mutate its PacketAssemblyBuffer/state.
    bool prepareSession(uint64_t sessionToken, int timeoutMs = 1500);
    bool armSession(uint64_t sessionToken, int timeoutMs = 1500);
    bool disarmSession(int timeoutMs = 1500);
    // 更新采集参数（停止测量后、重新开始前调用，线程安全）
    void updateConfig(const AcqConfig& config) {
        m_config = config;
        m_expectedPackets = config.packetsPerTrig();
    }

    //  统计查询（主线程调用，线程安全）
    CardStats::Snapshot statsSnapshot() const { return m_stats.snapshot(); }
    CardStats&          stats()               { return m_stats; }
    int   inputQueueDepth() const;
    int   cardId()          const { return m_cardId; }

#ifdef DATA_PROCESSOR_TEST_SEAM
    // 仅供确定性边界测试：执行一轮与生产线程完全相同的 queue drain，
    // 不启动 QThread，便于断言 513 个包时第 513 个仍留在队列中。
    int drainBatchForTest();

    struct SessionStateSnapshot {
        int assemblyReceived = 0;
        bool hasFlushedOnce = false;
        uint16_t lastFlushedTriggerSeq = 0;
        int consecutiveDiscards = 0;
        uint64_t activeSessionToken = 0;
        bool measureEnabled = false;
    };
    SessionStateSnapshot sessionStateForTest() const;
#endif

signals:
    // 诊断：某触发未收齐全部包即被切换（缺包数 = 期望包数 - 实收包数）
    void partialTrigger(int cardId, uint16_t triggerSeq, int missingPackets);

protected:
    void run() override;

private:
    // int16 Q0.15 差分相位序列  float32 kHz（差分法，O(N)，禁止用 FFT）
    // 显示相位积分与全分辨率 *_display 生成属于 FrontendPreprocessor 的
    // full-resolution display preparation，本类不再触碰 *_display 字段。
    void computeFrequency(TriggerGroup& group);

    // 将 assemblyBuf 当前内容 export → compute → 分发（供正常完成和强制 flush 共用）
    void flushAssemblyBuf(PacketAssemblyBuffer& assemblyBuf);

    // 执行一轮有上限的输入队列 drain。生产线程和测试 seam 共用此路径，
    // 保证 batch 边界回归测试不会只验证独立条件表达式。
    int processInputBatch(PacketAssemblyBuffer& assemblyBuf);

    enum class SessionCommandKind { Prepare, Arm, Disarm };
    struct SessionCommandWait {
        std::mutex mutex;
        std::condition_variable condition;
        bool done = false;
        bool success = false;
    };
    struct SessionCommand {
        SessionCommandKind kind = SessionCommandKind::Prepare;
        uint64_t sessionToken = 0;
        std::shared_ptr<SessionCommandWait> wait;
    };

    bool postSessionCommand(SessionCommandKind kind,
                            uint64_t sessionToken,
                            int timeoutMs);
    void processSessionCommands(PacketAssemblyBuffer& assemblyBuf);
    int drainInputQueue();
    void resetSessionState(PacketAssemblyBuffer& assemblyBuf);

    //  成员 
    int              m_cardId;
    AcqConfig        m_config;
    std::atomic<bool> m_saveEnabled{false};
    std::function<bool(const TriggerGroupPtr&)> m_directSaveSink;
    std::atomic<bool> m_measureEnabled{false};  // 开始测量门控
    std::atomic<uint64_t> m_ingressSessionToken{0};
    std::atomic<uint64_t> m_activeSessionToken{0};
    // 原始：使用静态 MAX_SAVE_QUEUE 常量在 cpp 中控制

    moodycamel::ConcurrentQueue<DataPacket>       m_inputQueue;
    moodycamel::ConcurrentQueue<TriggerGroupPtr>* m_saveQueue     = nullptr;
    FramePublisher*                               m_framePublisher = nullptr;
    // Frontend enqueue only.  DisplayBuffer and RingFeedSink are owned and
    // driven by FrontendPreprocessor; production wires exactly one owner.
    FrontendSubmitSink                            m_frontendSubmitSink;
    std::function<uint64_t()>                     m_sessionGenReader;   // 自动保存会话代读取器

    // 条件变量唤醒（接收线程写数据后通知处理线程）
    std::mutex              m_wakeMtx;
    std::condition_variable m_wakeCv;
    std::atomic<bool>       m_hasData{false};
    std::mutex               m_sessionCommandMtx;
    std::deque<SessionCommand> m_sessionCommands;
    std::atomic<bool>          m_hasSessionCommand{false};

    CardStats  m_stats;
    // 丢包统计锚点：用触发序号取代包序号跟踪，对乱序完全不敏感
    uint16_t   m_lastFlushedTriggerSeq = 0;     // 最后完成 flush 的触发序号
    bool       m_hasFlushedOnce        = false;  // 首次 flush 前不做回溯统计（避免启动误判）
    int        m_expectedPackets       = 0;      // 一次触发期望包数

    // 连续丢弃恢复：当 FPGA 重置触发序号时，m_lastFlushedTriggerSeq 持有旧值
    // 导致所有新包均被 step1 丢弃（带宽正常但触发数停止）。
    // 超过阈值后强制重置锚点，快速恢复。
    int        m_consecutiveDiscards   = 0;
    std::unique_ptr<PacketAssemblyBuffer> m_workerAssembly;
    std::atomic<int>      m_assemblyReceived{0};
    std::atomic<bool>     m_hasFlushedOnceAtomic{false};
    std::atomic<uint16_t> m_lastFlushedTriggerSeqAtomic{0};
    std::atomic<int>      m_consecutiveDiscardsAtomic{0};

    // 新一帧/新一轮测量触发序号重置识别阈值：
    // 触发序号相对上一帧末触发大幅回退（远大于乱序抖动窗口）时，
    // 立即重置锚点并接受首触发，避免整帧首个触发被当作迟到包丢弃
    // （表现为“触发计数少一次但丢包为零”）。
    static constexpr int kTriggerResetBackJumpThreshold = 256;
};
