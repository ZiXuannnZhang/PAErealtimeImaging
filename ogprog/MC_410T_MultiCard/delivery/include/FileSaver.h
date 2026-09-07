#pragma once
#include <QThread>
#include <QFile>
#include <atomic>
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

    //  热路径接口（DataProcessor 线程调用，无锁入队）
    void saveTriggerGroup(const TriggerGroupPtr& group);

    // 提供队列指针供 DataProcessor 直接入队（DataProcessor 构造时注入）
    moodycamel::ConcurrentQueue<TriggerGroupPtr>* saveQueue() { return &m_saveQueue; }

    //  状态查询 
    bool     isSaving()      const { return m_saving.load(); }
    int      queueDepth()    const;
    uint64_t savedCount()    const { return m_savedCount.load(); }

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
    std::atomic<uint64_t> m_savedCount{0};

    QString  m_saveDirectory;
    QString  m_fileSuffix;
    int      m_triggersPerFile      = 1000;
    int      m_currentFileTriggers  = 0;
    int      m_fileSequence         = 0;
    uint32_t m_currentSourceIPv4    = 0;

    QFile*   m_fileChannelA = nullptr;
    QFile*   m_fileChannelB = nullptr;

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
