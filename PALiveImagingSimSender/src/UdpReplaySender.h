#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

// UDP 数据发送核心：把已解调的 .dat 时域数据按真实采集协议回放到本机
// 监听程序（8001~8004），载荷为 B/A 交织 int16/int32 采样对。
class UdpReplaySender {
public:
    struct Config {
        std::string dataPath;
        int id = 14;          // 11.dat / 14.dat（决定 wlOffset 与每圈列数）
        int depth = 4000;     // 数据文件单 A-line 点数（源深度，14.dat 固定 4000）
        int acqTimeNs = 20000; // 监听程序采集时间(ns)
        double sourceRateMHz = 200.0;     // 模拟数据采样率（MSa/s，14.dat 默认 200）
        double listenerRateMHz = 200.0;   // 监听程序采样率（MSa/s；后续改为读取下发值）
        int bits = 32;        // 16 或 32
        double rateHz = 40.0; // 0 = 最快发送
        int portBase = 8001;
        int triggers = 0;     // 0 = 自动（按数据集整圈）
        // 8 通道发送开关（卡1-A, 卡1-B, 卡2-A, ...）；空/不足8个时全部发送信号
        std::vector<bool> channelEnabled;
        // 未勾选通道的噪声幅度（% 该通道信号 RMS，默认 10%）
        double noisePercent = 10.0;
    };

    using Progress = std::function<void(int sent, int total)>;
    using Log = std::function<void(const std::string &line)>;

    UdpReplaySender() = default;
    ~UdpReplaySender();

    // 后台线程启动发送；返回 false 表示配置无效或已在发送。
    bool start(const Config &cfg, Progress progress, Log log);
    void requestStop();
    bool running() const { return m_running.load(); }

private:
    void run();

    Config m_cfg;
    Progress m_progress;
    Log m_log;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
};
