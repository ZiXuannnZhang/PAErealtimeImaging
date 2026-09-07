#pragma once
#include <QThread>
#include <vector>
#include <atomic>
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
        QObject* parent = nullptr);

    ~MultiPortReceiver() override;

    void requestStop();
    bool isActive() const { return m_active.load(); }

signals:
    void statusMessage(const QString& msg);
    void errorOccurred(const QString& error);

protected:
    void run() override;

private:
    bool openSockets();
    void closeSockets();

    std::vector<int>             m_cardIndices;
    std::vector<DataProcessor*>  m_processors;
    int                          m_cpuCore;
    std::atomic<bool>            m_running{false};
    std::atomic<bool>            m_active{false};

    // WinSock 原生句柄（避免 Qt 对象跨线程问题）
    std::vector<uintptr_t>       m_sockets;  // SOCKET 类型（Windows: UINT_PTR）
    std::vector<uint8_t>         m_recvBuf;  // 接收缓冲区

    static constexpr int SELECT_TIMEOUT_US = 1000;   // 1ms
    static constexpr int RECV_BUF_SIZE     = 65536;  // 64KB 接收缓冲
};

