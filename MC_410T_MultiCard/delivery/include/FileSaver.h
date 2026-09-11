#pragma once
#include <QThread>
#include <QFile>
#include <QJsonObject>
#include <atomic>
#include <functional>
#include "DataTypes.h"
#include "third_party/concurrentqueue.h"

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
    void resumeAfterSourceRestart(){m_saving.store(true,std::memory_order_release);}

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
    // savedCount/writtenCount advance only after both A/B files and the index
    // commit have completed. acceptedCount is the earlier queue-consumption
    // count used to diagnose a buffered-but-not-yet-written tail.
    uint64_t savedCount()    const { return m_writtenCount.load(); }
    uint64_t writtenCount()  const { return m_writtenCount.load(); }
    uint64_t acceptedCount() const { return m_acceptedCount.load(); }

signals:
    void statusMessage(const QString& message);
    void errorOccurred(const QString& error);

protected:
    void run() override;

private:
    void openNewFiles(uint32_t sourceIPv4);
    void closeFiles();
    QString generateFileName(const QString& channel) const;

    // float32  float16 批量转换（软件实现，Release 下可选开启 F16C）
    static uint16_t float32ToFloat16(float value);
    static void     convertBatch(const float* src, uint16_t* dst, int count);

    int      m_cardId;
    std::atomic<bool>     m_running{false};
    std::atomic<bool>     m_saving{false};
    std::atomic<uint64_t> m_acceptedCount{0};

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
    QFile*   m_indexFile = nullptr;

    struct PendingIndexRecord {
        std::uint64_t acceptSequence=0;
        std::uint64_t session=0,wireTrigger=0,expandedTrigger=0;
        int card=-1,sampleCount=0;
        qint64 aOffset=0,aBytes=0,bOffset=0,bBytes=0;
        bool qualityUnknown=false,assemblyComplete=false,packetCoverageComplete=false;
        QString missingReason;
    };
    std::vector<PendingIndexRecord> m_pendingIndex;
    std::uint64_t m_acceptSequence=0,m_batchSequence=0;
    qint64 m_fileOffsetA=0,m_fileOffsetB=0;
    std::atomic<std::uint64_t> m_writtenCount{0};
    bool m_writeFailed=false;
    QString m_lastWriteError;
    QString m_currentIndexPath,m_currentManifestPath;

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

    bool flushWriteBuffers();  // A/B 写成功后追加 index commit，再清空积累缓冲
    bool appendIndexLine(const QJsonObject& object);
    void writeManifest(bool closed);
    void markWriteFailure(const QString& message);
};
