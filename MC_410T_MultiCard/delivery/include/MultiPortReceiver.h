#pragma once
#include <QThread>
#include <QByteArray>
#include <vector>
#include <array>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <memory>
#include "DataTypes.h"
#include "DataProcessor.h"

// ============================================================
// MultiPortReceiver  路线A：WinSock select 多路复用接收线程
//
// 每个实例负责 4 张卡（4 个 UDP 端口）
// 使用 select() 1ms 超时，有数据立即处理
// 线程亲和性设到指定 CPU 核心
// ============================================================
class MultiPortReceiver : public QThread {
    Q_OBJECT

public:
    explicit MultiPortReceiver(
        const std::vector<int>& cardIndices,    // 负责的卡号列表（0-based，最多4张）
        const std::vector<DataProcessor*>& processors,
        int cpuCore = -1,                       // 绑定的 CPU 核心号（-1 = 不绑定）
        QObject* parent = nullptr,
        bool testNoSockets = false);

    ~MultiPortReceiver() override;

    void requestStop();
    bool isActive() const { return m_active.load(); }
    // 等待接收线程完成端口绑定并进入接收循环；成功返回 true。
    // 任一端口绑定失败时线程会自行退出，返回 false。
    bool waitUntilStarted(int timeoutMs) const;
    // 紧急停止时关闭所有 socket（与 requestStop 配合，避免 terminate 泄漏 UDP 端口）
    void forceCloseSockets();

    // Receiver-owned measurement admission barriers.  The command is
    // executed by the receiver thread, including socket backlog quiescence;
    // the caller only waits for the bounded result.
    bool prepareSession(uint64_t sessionToken, int timeoutMs = 1500);
    bool armSession(uint64_t sessionToken, int timeoutMs = 1500);
    // Publish the Start fence for one card only.  The card remains closed
    // until its own local hardware Start send has succeeded.
    bool commitCardSession(uint64_t sessionToken, int cardIndex,
                           int timeoutMs = 1500);
    bool disarmSession(int timeoutMs = 1500);
    // Legacy test compatibility only; production start/stop never calls this
    // bypass and uses the bounded prepare/arm/per-card-fence barriers above.
    void setCompatibilityAdmission(bool enable, uint64_t sessionToken = 1);

    // Deterministic test hook for the same parser/admission dispatch used by
    // run().  It injects a datagram without creating an FPGA sender.
    bool dispatchDatagramForTest(const QByteArray& datagram, int socketIndex = 0);

    enum class AdmissionState { Disarmed, Preparing, Armed, Running };
    AdmissionState admissionState() const {
        return static_cast<AdmissionState>(m_admissionState.load(std::memory_order_acquire));
    }

    struct SocketObservabilitySnapshot {
        int cardIndex = -1;
        uint64_t maxDrainPackets = 0;
    };

    struct ObservabilitySnapshot {
        std::vector<int> cardIndices;
        uint64_t selectWakeups = 0;
        uint64_t selectTimeouts = 0;
        uint64_t selectErrors = 0;
        uint64_t recvHardErrors = 0;
        uint64_t recvWouldBlockTerminations = 0;
        uint64_t maxDrainPackets = 0;
        uint64_t maxDrainDurationUs = 0;
        uint64_t maxReceiverLoopGapUs = 0;
        std::vector<SocketObservabilitySnapshot> sockets;
    };

    ObservabilitySnapshot observabilitySnapshot() const;

signals:
    void statusMessage(const QString& msg);
    void errorOccurred(const QString& error);

protected:
    void run() override;

private:
    bool openSockets();
    void closeSockets();
    enum class SessionCommandKind { Prepare, Arm, CommitCard, Disarm };
    struct SessionCommandWait {
        std::mutex mutex;
        std::condition_variable condition;
        bool done = false;
        bool success = false;
    };
    struct SessionCommand {
        SessionCommandKind kind = SessionCommandKind::Prepare;
        uint64_t sessionToken = 0;
        int cardIndex = -1;
        int timeoutMs = 1500;
        std::shared_ptr<SessionCommandWait> wait;
    };
    bool postSessionCommand(SessionCommandKind kind, uint64_t sessionToken,
                            int cardIndex, int timeoutMs);
    void processSessionCommands();
    bool applySessionCommand(const SessionCommand& command);
    void updateAggregateAdmissionState();
    bool drainSocketBacklog(int timeoutMs);
    bool drainSocket(uintptr_t socket, int socketIndex, bool dispatch,
                     uint64_t* outPackets = nullptr);
    bool dispatchDatagram(const char* bytes, int length, int socketIndex);

    std::vector<int>             m_cardIndices;
    std::vector<DataProcessor*>  m_processors;
    int                          m_cpuCore;
    std::atomic<bool>            m_running{false};
    std::atomic<bool>            m_active{false};
    const bool                   m_testNoSockets = false;
    std::atomic<int>             m_admissionState{static_cast<int>(AdmissionState::Disarmed)};
    std::atomic<uint64_t>        m_sessionToken{0};
    std::array<std::atomic<int>, 4> m_cardAdmissionStates{};
    std::array<std::atomic<uint64_t>, 4> m_cardSessionTokens{};

    mutable std::mutex           m_commandMutex;
    std::deque<SessionCommand>   m_commands;
    std::condition_variable      m_commandCv;

    // WinSock 原生句柄（避免 Qt 对象跨线程问题）
    mutable std::mutex           m_socketMutex;  // 保护 m_sockets（forceCloseSockets 可能跨线程调用）
    std::vector<uintptr_t>       m_sockets;  // SOCKET 类型（Windows: UINT_PTR）
    std::vector<uint8_t>         m_recvBuf;  // 接收缓冲区

    // 观测计数只由接收线程递增，读取侧使用 relaxed load；不参与接收控制流。
    std::atomic<uint64_t> m_selectWakeups{0};
    std::atomic<uint64_t> m_selectTimeouts{0};
    std::atomic<uint64_t> m_selectErrors{0};
    std::atomic<uint64_t> m_recvHardErrors{0};
    std::atomic<uint64_t> m_recvWouldBlockTerminations{0};
    std::atomic<uint64_t> m_maxDrainPackets{0};
    std::atomic<uint64_t> m_maxDrainDurationUs{0};
    std::atomic<uint64_t> m_maxReceiverLoopGapUs{0};
    std::array<std::atomic<uint64_t>, 4> m_socketMaxDrainPackets{};

    static void updateMax(std::atomic<uint64_t>& target, uint64_t value);

    static constexpr int SELECT_TIMEOUT_US = 1000;   // 1ms
    static constexpr int RECV_BUF_SIZE     = 65536;  // 64KB 接收缓冲
};
