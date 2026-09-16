#pragma once
#include <QThread>
#include <QFile>
#include <atomic>
#include <functional>
#include "DataTypes.h"
#include "third_party/concurrentqueue.h"

// Low-frequency observability for file rollovers. Distinguishes capacity
// rollovers (triggersPerFile) from physical-round rollovers
// (TriggerGroup::roundGeneration) as required by the round-boundary contract.
struct FileRolloverInfo {
    bool happened = false;
    QString reason;                 // "capacity" | "physical_round"
    uint64_t oldRoundGeneration = 0;
    uint64_t newRoundGeneration = 0;
    int oldFileSequence = 0;
    int newFileSequence = 0;
    int oldFileTriggerCount = 0;
    bool manualMode = true;         // sessionGen == 0
};

// ============================================================
// FileSaver  异步存储线程（每卡一个实例）
//
// 文件格式：纯二进制，无文件头
//   每个 .dat 文件 = M 次触发数据顺序追加
//   每次触发 = float16[sampleCount]（A/B 通道分两个文件）
//
// 文件命名：Card{N}_Ch{A|B}_{suffix}_{seq:03d}.dat
//   N = cardId + 1（1-based）
//   seq 从 000 开始，每满 triggersPerFile 后递增
// ============================================================
class FileSaver : public QThread {
    Q_OBJECT

public:
    explicit FileSaver(int cardId, QObject* parent = nullptr);
    ~FileSaver() override;

    //  控制接口（NetworkController / MainWindow 调用）
    void startSaving(const QString& directory,
                     int triggersPerFile = 1000,
                     const QString& suffix = "");
    void stopSaving();
    // 请求线程退出（设置 m_running=false，使 run() 循环退出）
    void requestStop() {
        m_saving.store(false, std::memory_order_release);
        m_running.store(false, std::memory_order_release);
    }

    // 会话主动落盘请求（方案A）：队列排空后刷盘并关闭当前会话文件，
    // m_saving 保持不变，下一会话触发到达时按会话代自动重新打开文件
    void requestClose() { m_closeRequest.store(true, std::memory_order_release); }

    //  热路径接口（DataProcessor 线程调用，无锁入队）
    void saveTriggerGroup(const TriggerGroupPtr& group);

    // PAimage host adaptation: called only by the single source saving worker,
    // with this FileSaver's QThread left unstarted. Reuses the same file format,
    // routing, batching and float16 conversion as the historical run() path.
    bool consumeTriggerGroup(const TriggerGroupPtr& group);
    void serviceCloseRequest();
    void suspendForSourceRestart();
    void resumeAfterSourceRestart();

    // 自动保存会话代目录解析：按触发组携带的 sessionGen 查询保存目录。
    // gen=0（手动/无会话代）返回空串=保持当前目录不变。
    void setSessionDirResolver(std::function<QString(uint64_t)> resolver) {
        m_dirResolver = std::move(resolver);
    }

    // 提供队列指针供 DataProcessor 直接入队（DataProcessor 构造时注入）
    moodycamel::ConcurrentQueue<TriggerGroupPtr>* saveQueue() { return &m_saveQueue; }

    //  状态查询
    bool     isSaving()      const { return m_saving.load(); }
    int      queueDepth()    const;
    uint64_t savedCount()    const { return m_savedCount.load(); }

    // Physical-round rollover observability (low frequency, rollover only).
    const FileRolloverInfo& lastFileRollover() const { return m_lastRollover; }
    uint64_t fileRolloverCount()         const { return m_rolloverCount; }
    uint64_t physicalRoundRolloverCount() const { return m_roundRolloverCount; }
    bool     hasCurrentPhysicalRound()   const { return m_haveCurrentPhysicalRound; }
    uint64_t currentPhysicalRoundGeneration() const { return m_currentPhysicalRoundGeneration; }

signals:
    void statusMessage(const QString& message);
    void errorOccurred(const QString& error);
    // Emitted on every file rollover (capacity or physical round). Bridged to
    // the diagnostic recorder by the owner; tests may connect directly.
    void fileRolled(int cardId, const QString& reason,
                    quint64 oldRoundGeneration, quint64 newRoundGeneration,
                    int oldFileSequence, int newFileSequence,
                    int oldFileTriggerCount, bool manualMode);

protected:
    void run() override;

private:
    void openNewFiles(uint32_t sourceIPv4);
    void closeFiles();
    QString generateFileName(const QString& channel) const;
    // Physical-round file state lifecycle: cleared on start/stop/suspend/resume
    // and on auto-save session-gen change so an old measurement can never
    // pollute a new measurement's round boundaries.
    void resetPhysicalRoundState();
    void recordRollover(const FileRolloverInfo& info);

    // float32  float16 批量转换（软件实现，Release 下可选开启 F16C）
    static uint16_t float32ToFloat16(float value);
    static void     convertBatch(const float* src, uint16_t* dst, int count);

    int      m_cardId;
    std::atomic<bool>     m_running{false};
    std::atomic<bool>     m_saving{false};
    std::atomic<uint64_t> m_savedCount{0};

    QString  m_saveDirectory;
    QString  m_fileSuffix;
    int      m_triggersPerFile      = 1000;
    int      m_currentFileTriggers  = 0;
    int      m_fileSequence         = 0;
    uint32_t m_currentSourceIPv4    = 0;

    // 自动保存会话代路由：当前处理的会话代 + 目录解析器
    uint64_t m_currentGen = 0;
    std::function<QString(uint64_t)> m_dirResolver;
    std::atomic<bool> m_closeRequest{false};   // 会话主动落盘请求（方案A）
    uint64_t m_dropGen = 0;                    // 目录未注册需丢弃的会话代（方案C，0=无）

    QFile*   m_fileChannelA = nullptr;
    QFile*   m_fileChannelB = nullptr;

    // Physical-round file boundary state (data-plane authoritative):
    // TriggerGroup::roundGeneration drives the rollover before the first group
    // of a new generation is written; triggersPerFile remains the capacity cap.
    bool     m_haveCurrentPhysicalRound = false;
    uint64_t m_currentPhysicalRoundGeneration = 0;
    FileRolloverInfo m_lastRollover;
    uint64_t m_rolloverCount = 0;
    uint64_t m_roundRolloverCount = 0;

    moodycamel::ConcurrentQueue<TriggerGroupPtr> m_saveQueue;

    // float16 转换缓冲区（避免每次触发重新分配）
    std::vector<uint16_t> m_f16BufA;
    std::vector<uint16_t> m_f16BufB;

    // 写盘积累缓冲区：多触发合并为一次大 I/O，降低频繁小写盘开销
    // 每 WRITE_BUFFER_TRIGGERS 个触发（约 800 KB/通道）刷一次盘
    static constexpr int WRITE_BUFFER_TRIGGERS = 16;
    std::vector<uint16_t> m_writeAccumA;
    std::vector<uint16_t> m_writeAccumB;
    int m_accumTriggers = 0;

    void flushWriteBuffers();  // 将积累缓冲写入当前打开的文件并清空
};
