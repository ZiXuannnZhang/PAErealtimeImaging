#include "FramePublisher.h"
#include <chrono>
#include <cstring>
#include <QThread>

#ifdef USE_ZEROMQ
#  include <zmq.h>
#endif

FramePublisher::FramePublisher(QObject* parent)
    : QThread(parent) {
    setObjectName("FramePublisher");
}

FramePublisher::~FramePublisher() {
    if (isRunning()) {
        requestInterruption();
        if (!wait(500)) {
            terminate();
            wait();   // 确保线程真正结束后再析构 QThread，避免 qFatal
        }
    }
}

void FramePublisher::configure(bool enable, int nCards, int sampleCount) {
    m_enabled     = enable;
    m_nCards      = nCards;
    m_sampleCount = sampleCount;
}

// 热路径：DataProcessor 线程调用，零开销（enableExtension=false）
void FramePublisher::submit(const TriggerGroupPtr& group) {
    if (!m_enabled) return;
    m_inQueue.enqueue(group);
}

//
// FramePublisher 后台线程
//
void FramePublisher::run() {
    if (!m_enabled) return;

#ifdef USE_ZEROMQ
    // ZeroMQ 初始化
    m_zmqCtx    = zmq_ctx_new();
    m_zmqSocket = zmq_socket(m_zmqCtx, ZMQ_PUSH);
    int linger  = 0;
    zmq_setsockopt(m_zmqSocket, ZMQ_LINGER, &linger, sizeof(linger));
    zmq_bind(m_zmqSocket, ZMQ_ENDPOINT);
#endif

    TriggerGroupPtr group;
    while (!isInterruptionRequested()) {
        int dequeued = 0;
        while (m_inQueue.try_dequeue(group)) {
            ++dequeued;
            std::lock_guard<std::mutex> lock(m_mapMtx);
            auto& frame = m_pending[group->triggerSeq];
            if (frame.arrivedCount == 0) {
                frame.cards.resize(m_nCards);
                frame.firstArrivalMs = group->timestamp_ms;
            }
            if (group->cardId >= 0 && group->cardId < m_nCards &&
                !frame.cards[group->cardId]) {
                frame.cards[group->cardId] = group;
                ++frame.arrivedCount;
            }
            if (frame.arrivedCount >= m_nCards) {
                assembleAndPublish(group->triggerSeq, frame);
                m_pending.erase(group->triggerSeq);
            }
        }
        // 定期清理超时帧
        {
            auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
            std::lock_guard<std::mutex> lock(m_mapMtx);
            cleanupStaleFrames(now);
        }
        if (dequeued == 0) QThread::msleep(5);
    }

#ifdef USE_ZEROMQ
    // ZeroMQ 清理
    if (m_zmqSocket) { zmq_close(m_zmqSocket); m_zmqSocket = nullptr; }
    if (m_zmqCtx)    { zmq_ctx_destroy(m_zmqCtx); m_zmqCtx = nullptr; }
#endif
}

void FramePublisher::assembleAndPublish(uint16_t triggerSeq, PendingFrame& frame) {
    SyncFrame sf;
    sf.triggerSeq     = triggerSeq;
    sf.nCards         = m_nCards;
    sf.samplesPerCard = m_sampleCount;
    sf.timestamp_ms   = frame.firstArrivalMs;
    sf.data.resize(m_nCards * 2 * m_sampleCount, 0.0f);

    for (int c = 0; c < m_nCards; c++) {
        if (!frame.cards[c]) continue;
        const auto& g = frame.cards[c];
        int n = std::min(static_cast<int>(g->freqA.size()), m_sampleCount);
        int offset = c * 2 * m_sampleCount;
        if (n > 0) {
            std::memcpy(sf.data.data() + offset,
                        g->freqA.data(), n * sizeof(float));
            std::memcpy(sf.data.data() + offset + m_sampleCount,
                        g->freqB.data(), n * sizeof(float));
        }
    }

#ifdef USE_ZEROMQ
    // ZeroMQ zero-copy 发送（ZMQ_NOBLOCK：接收进程不在线时直接丢弃）
    if (m_zmqSocket) {
        zmq_send(m_zmqSocket, sf.data.data(),
                 sf.data.size() * sizeof(float), ZMQ_NOBLOCK);
    }
#endif

    emit framePublished(triggerSeq, m_nCards);
}

void FramePublisher::cleanupStaleFrames(uint64_t nowMs) {
    for (auto it = m_pending.begin(); it != m_pending.end(); ) {
        if (nowMs - it->second.firstArrivalMs > FRAME_TIMEOUT_MS)
            it = m_pending.erase(it);
        else
            ++it;
    }
}
