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

//  由环形扫描参数推导「每圈设计触发数」的唯一入口。
//
//  公式：每圈设计触发数 = 单圈总A-line数 / 启用通道数
//  依据 RingBlockAssembler 的几何约定：每枚全局触发给每个启用通道 1 根 A-line，
//  全局触发 g 按奇偶交替 wl1/wl2，故每通道每波长每圈 A-line 数 = 总数/(通道数×2)，
//  而每圈触发枚数 = 2 × 每通道每波长 = 总数/通道数。RingConfigDialog 的
//  alinesPerChannelPerFrame 换算与此逐字一致。
//
//  该结果就是 HostOutput 前端刷新闸门（logicalTriggerIndex >= 阈值即不再推前端）
//  比对的阈值。环形模式下必须由它产生，任何写死的常量都不得参与：监听启动注入、
//  RingConfigDialog::applyConfig 下发、configureRingAssembler 三处共用本函数，
//  避免各处口径漂移。
//
//  几何非法（通道数<=0、总数<=0、不能整除、商<=0）返回 0，调用方保持原值不下发。
inline int ringLogicalTriggersPerRound(int alinesPerFrame, int enabledChannelCount) noexcept {
    if (alinesPerFrame <= 0 || enabledChannelCount <= 0) return 0;
    if (alinesPerFrame % enabledChannelCount != 0) return 0;
    return alinesPerFrame / enabledChannelCount;
}

struct AcqConfig {
    // 仅作线性模式 / 尚无环形配置时的回退值。环形模式下每圈设计触发数一律由
    // ringLogicalTriggersPerRound() 从「单圈总A-line数」推导，本常量不得生效；
    // 它没有 UI 入口（只经 AcquisitionParams/LogicalTriggersPerRound 读写）。
    static constexpr int kDefaultLogicalTriggersPerRound = 4000;
    int         nCards       = 4;   // 启用的卡数（默认4；自动识别时由扫描在线数决定）
    std::string localBindIP;        // 控制 socket 本地绑定IP（空=INADDR_ANY）
    // 目标采集卡 IP 列表（网段扫描自动识别结果；非空时优先使用，空则按 192.168.0.2 起递增生成）
    std::vector<std::string> targetIPs;
    int  acqTimeNs       = 200000;   // 采集时间窗口（单位：ns，如 200000 = 200 µs）
    int  triggerHz       = 200;      // 触发频率上限（Hz）
    int  delayA          = 1000;     // A 通道延时（ns）
    int  delayB          = 1000;     // B 通道延时（ns）
    // Startup admission policy in milliseconds: 0 = bypass (diagnostic
    // default, no startup cache/idle filter), 1000 = legacy regression.
    // Single source of truth for both Backend Settings and diagnostic logs.
    int  startupIdleMs   = 0;
    bool enablePublisher = false;    // 是否启用 ZeroMQ FramePublisher
    bool diagnosticTraceEnabled = true; // Production default; deterministic replay comparison may disable.
    // 0=原始入口轨迹，1=轻量接收时序，2=轻量时序+外部系统抓取索引。
    int diagnosticLevel = 1;
    // Experimental per-socket timestamping: 0=off (default), 1=software,
    // 2=hardware, 3=auto (hardware then explicit software fallback).
    int socketTimestampMode = 0;

    // Canonical operational logical-trigger count for one physical round.
    // 环形模式下取值来自 ringLogicalTriggersPerRound()（= 单圈总A-line数 /
    // 启用通道数），在监听启动注入，且此后每次 RingConfigDialog::applyConfig
    // 都重新下发；禁止回落到 kDefaultLogicalTriggersPerRound。线性模式/尚无
    // 环形配置时才使用 AcquisitionParams/LogicalTriggersPerRound 的持久化值。
    // The normalizer never owns or guesses this product setting.
    int logicalTriggersPerRound = kDefaultLogicalTriggersPerRound;

    // ══ 数据格式参数（真实采集固定 250 MSa/s 满速率，与 Constants.h 一致）══
    // 32bit Q16.16 差分相位，采样间隔 4.0 ns，不抽取。
    int  bitsPerChannel    = 32;
    // sampleIntervalNs: 采样间隔（ns）= 4.0 = 250 MSa/s（FPGA_ADC_INTERVAL_NS）
    double sampleIntervalNs = FPGA_ADC_INTERVAL_NS;
    // 解调（电压→声波时域信号）已在采集卡 FPGA 完成；UDP 载荷即为已解调
    // 时域信号，监听程序只做一次精度转换（int16/int32 → float32），
    // 不再做差分相位→频率二次换算（历史模拟回放路径已移除）。

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
