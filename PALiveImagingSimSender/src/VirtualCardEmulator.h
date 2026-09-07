#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#endif

// 虚拟采集卡模拟器：面向真实采集场景的监听程序（MC410T_Receiver）验证。
//
// 真实采集卡上电后 FPGA 网络栈初始化完成，会向主机 IP:8000 发送一个
// 18 字节 UDP 就绪包；监听程序按"发送方 IP"映射卡号，并据此标记卡片就绪。
// 本模拟器周期性地从 127.0.0.1~127.0.0.4（每卡一个独立回环源 IP）发送
// 18 字节就绪包，并监听 8080 控制端口，对 0x02 配置命令回发 60 字节反馈，
// 使监听程序的"开始监听 → 卡就绪 → 配置确认 → 开始测量"全流程可在本机手动验证。
class VirtualCardEmulator {
public:
    struct Config {
        int nCards = 4;                  // 虚拟卡数量（1~4，对应源 IP 127.0.0.1~127.0.0.4）
        std::string hostIP = "127.0.0.1";// 监听程序所在主机（就绪包/反馈包目标）
        int feedbackPort = 8000;         // 监听程序反馈/就绪监听端口
        int controlPort = 8080;          // 虚拟卡控制命令监听端口
        int readyIntervalMs = 1000;      // 就绪包发送周期
    };

    using Log = std::function<void(const std::string &line)>;

    VirtualCardEmulator() = default;
    ~VirtualCardEmulator();

    bool start(const Config &cfg, Log log);
    void requestStop();
    bool running() const { return m_running.load(); }

private:
    void run();
    void sendReady(int cardIdx, bool announce);
    void sendConfigAck(int cardIdx);
    void sendAckTo(int cardIdx, const sockaddr_in &sender);
    std::string cardSourceIP(int cardIdx) const;

    Config m_cfg;
    Log m_log;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
};
