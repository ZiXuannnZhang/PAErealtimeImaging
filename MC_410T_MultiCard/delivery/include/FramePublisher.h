#pragma once
#include <QThread>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "DataTypes.h"
#include "third_party/concurrentqueue.h"

// ============================================================
// FramePublisher  多卡同步汇聚 + ZeroMQ PUSH 扩展接口
// 编译时加 -DUSE_ZEROMQ 即可开启 ZeroMQ 实际发送
// ============================================================
class FramePublisher : public QThread {
    Q_OBJECT
    using Queue = moodycamel::ConcurrentQueue<TriggerGroupPtr>;
public:
    //  汇聚挂起帧结构（需要在 private 方法参数里引用，放 public 区）
    struct PendingFrame {
        std::vector<TriggerGroupPtr> cards;
        int      arrivedCount    = 0;
        uint64_t firstArrivalMs  = 0;
    };

    explicit FramePublisher(QObject* parent = nullptr);
    ~FramePublisher() override;

    void configure(bool enable, int nCards, int sampleCount);
    void submit(const TriggerGroupPtr& group);

signals:
    void framePublished(int triggerSeq, int nCards);
    void publishError(const QString& msg);

protected:
    void run() override;

private:
    void assembleAndPublish(uint16_t triggerSeq, PendingFrame& frame);
    void cleanupStaleFrames(uint64_t nowMs);

    bool m_enabled     = false;
    int  m_nCards      = 32;
    int  m_sampleCount = 25000;

    Queue m_inQueue;
    std::mutex m_mapMtx;
    std::unordered_map<uint16_t, PendingFrame> m_pending;

#ifdef USE_ZEROMQ
    void* m_zmqCtx    = nullptr;
    void* m_zmqSocket = nullptr;
#endif

    static constexpr char ZMQ_ENDPOINT[]  = "tcp://127.0.0.1:5556";
    static constexpr int  FRAME_TIMEOUT_MS = 20;
};