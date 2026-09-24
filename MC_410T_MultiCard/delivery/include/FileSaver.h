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
// "sequence_collision" records that the next free sequence had to be found by
// skipping names that already exist on disk (see the no-overwrite invariant).
struct FileRolloverInfo {
    bool happened = false;
    QString reason;                 // "capacity" | "physical_round" | "sequence_collision"
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
//
// ── 落盘不变量：已写入磁盘的数据永不被改写 ──────────────────────
//   打开文件一律独占创建（QIODevice::NewOnly），并在开之前由
//   resolveFreeSequence() 自 m_fileSequence 起向后落到第一个 A/B 两侧都
//   不存在的序号。因此任何一次重开（会话代边界 requestClose 之后的同代
//   残留帧、sourceIPv4 变化、startSaving 复用目录、物理轮次轮转）都只会
//   追加到新文件，绝不会截断重写同名已有文件。
//   序号不主动作废、不归零：编号保持连号，避让只在真正撞名时发生（并以
//   FileRolloverInfo::reason == "sequence_collision" 留痕）。
// ────────────────────────────────────────────────────────────────
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
    // 返回值 = 本次开文件前那次关文件刷盘是否成功；失败时不再开新文件。
    void openNewFiles(uint32_t sourceIPv4);
    // 返回值 = 关文件前那次刷盘是否成功。~FileSaver 调用时忽略结果（析构里
    // 不得发信号）；openNewFiles 用它决定要不要继续开新文件。
    bool closeFiles();
    // 指定序号下的完整路径（generateFileName 即 fileNameFor(channel, m_fileSequence)）。
    QString fileNameFor(const QString& channel, int sequence) const;
    QString generateFileName(const QString& channel) const;
    // 落盘不变量的第一道防线：自 m_fileSequence 起向后找第一个 A/B 两侧都
    // 不存在的序号并落位。撞名被跳过时记一条 "sequence_collision" 轮转。
    // 探测有界，穷尽后保持原序号，交由 NewOnly 打开失败处理（绝不截断）。
    int resolveFreeSequence();
    // 写盘故障处理：停保存 + 一次性告警（errorOccurred 已接 UI 日志）。
    // 刻意不 closeFiles()，因此不存在 closeFiles <-> flushWriteBuffers 重入，
    // 也不会被 ~FileSaver 隐式触发。由各非析构调用点显式调用。
    void handleWriteFault(const QString& where);
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

    // 将积累缓冲写入当前打开的文件并清空。
    // 返回 false = 本次刷盘失败（IO错误或短写），已把 A/B 双双回退到本次刷盘
    // 前的记录边界并丢弃本段缓冲——.dat 定长记录无文件头，半个记录会让该文件
    // 的记录边界永久错位，宁可丢整段也不留半条。本函数只返回 bool 不 emit：
    // 它也被 closeFiles() / ~FileSaver() 调用，析构里不得发信号。
    bool flushWriteBuffers();
    // 首次写盘故障只告警一次（磁盘满通常是持续性的，不要每段刷一条）。
    // startSaving() 开新会话时复位。
    bool m_writeFaulted = false;

#ifdef FILESAVER_TEST_SEAM
public:
    // 测试专用故障注入口（不编译进生产目标，与 FRONTEND_PREPROCESSOR_TEST_SEAM
    // 同套路）。mode: 0=正常 1=两通道都失败 2=仅 A 失败 3=仅 B 失败。
    // 通过把 write() 返回值改写为 -1 模拟失败；回退逻辑负责撤掉已落的字节。
    void setWriteFaultForTest(int mode) { m_writeFaultForTest = mode; }
private:
    int m_writeFaultForTest = 0;
#endif
};
