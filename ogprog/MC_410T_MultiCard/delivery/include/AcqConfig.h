#pragma once
#include "Constants.h"
#include <string>
#include <vector>

// ============================================================
// AcqConfig  采集配置参数（UI → 后端参数传递结构体）
//
// nCards 无 UI 接口，通过注册表直接修改：
//   HKCU\Software\MC410T\MC410T_Receiver
//     AcquisitionParams\NCards  (DWORD，默认4，有效范围1~MAX_CARDS)
//
// localBindIP 无 UI 接口，通过注册表直接修改：
//   HKCU\Software\MC410T\MC410T_Receiver
//     NetworkParams\LocalBindIP  (String，默认空=INADDR_ANY)
//   双口网卡（如 ConnectX-5 MCX512A-ACAT）只接一个口时必须填写，
//   否则 Windows 路由可能将控制包从断开的口发出，导致采集卡无法收到命令。
//   例：只接了"以太网10"（IP=192.168.0.100），设为 "192.168.0.100"
//
// 接收路线：WinSock（编译期确定，不可运行时切换）
// ============================================================
struct AcqConfig {
    int         nCards       = 4;   // 启用的卡数（默认4；自动识别时由扫描在线数决定）
    std::string localBindIP;        // 控制 socket 本地绑定IP（空=INADDR_ANY）
    // 目标采集卡 IP 列表（网段扫描自动识别结果；非空时优先使用，空则按 192.168.0.2 起递增生成）
    std::vector<std::string> targetIPs;
    int  acqTimeNs       = 200000;   // 采集时间窗口（单位：ns，如 200000 = 200 µs）
    int  triggerHz       = 200;      // 触发频率上限（Hz）
    int  delayA          = 1000;     // A 通道延时（ns）
    int  delayB          = 1000;     // B 通道延时（ns）
    int  displayPoints   = 1000;     // UI 显示点数（降采样后）
    bool enablePublisher = false;    // 是否启用 ZeroMQ FramePublisher

    // ══ 数据格式参数（可根据 FPGA 固件配置调整）══
    // bitsPerChannel: 每通道数据位宽（16 或 32）
    //   16 = int16 Q0.15 差分相位（需 2抽1 抽取后 125MSa/s）
    //   32 = int32 Q16.16 差分相位（满速率 250MSa/s，不抽取，除以2^16得弧度）
    int  bitsPerChannel    = 16;
    // sampleIntervalNs: 采样间隔（ns），直接影响每触发采样点数
    //   8.0 = 125 MSa/s（2抽1 抽取后）
    //   4.0 = 250 MSa/s（满速率，不抽取）
    double sampleIntervalNs = 8.0;

    //  派生参数（计算字段）
    // 每采样对字节数（A+B两通道）
    int bytesPerSamplePair() const {
        return bitsPerChannel * 2 / 8;
    }
    // 单次触发每通道采样点数
    int samplesPerTrig() const {
        return static_cast<int>(acqTimeNs / sampleIntervalNs);
    }
    // 单次触发期望的 UDP 包数
    int packetsPerTrig() const {
        int bytesPerTrig = samplesPerTrig() * bytesPerSamplePair();
        return (bytesPerTrig + UDP_PAYLOAD_BYTES - 1) / UDP_PAYLOAD_BYTES;
    }
};
