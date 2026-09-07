#pragma once

#include <QString>

// =====================================================================
// ImagingParams — 成像参数结构体（三组：通用 / 扫描 / 重建）
// 对应《成像功能扩展设计方案》v2.0 §3.2
// =====================================================================

// ---- 第一组：通用参数 ----
struct GeneralParams {
    // ADC 采样率 250 MSa/s（硬件固定）
    float  daq_hz            = 250e6f;

    // 采样时间 / 深度（由左侧采集参数计算：采集时间 / 采样间隔）
    float  sampletime_s      = 200e-6f;
    int    depth             = 50000;

    // 扫描模式（互斥单选）
    bool   isFullScan        = false;
    bool   isFastScan        = true;

    // 光纤复用模式（互斥单选）
    // isMultiFiber = true （多光纤复用）:
    //   4张采集卡，每卡2物理通道→共8物理通道，每通道复用8根光纤→共64根光纤
    //   算法参数：card_num=4（每卡1数据块，depth内含8光纤复用到64纤）, channel_num=64
    // isMultiFiber = false（单光纤直连）:
    //   4张采集卡，每卡2物理通道→共8物理通道，每通道1根光纤→共8根光纤
    //   算法参数：card_num=8（每物理通道1数据块）, channel_num=8
    bool   isMultiFiber      = false;

    // 采集卡数据块数（4 张卡×每卡 1 数据块，内部脉冲缓冲使用）
    int    cardNum           = 4;

    // 物理通道数（算法 card_num，4 卡×2 通道=8，可编辑方便测试）
    int    physicalChannels  = 8;

    // 光纤通道总数（算法 channel_num，多光纤复用→64，单光纤直连→8）
    int    channelNum        = 64;

    bool   isSaveSampledData = false;
    bool   isSaveReconData   = false;
};

// ---- 第二组：扫描参数 ----
struct ScanParams {
    float  stepsize_um      = 10.0f;
    int    move_aline       = 100;
    float  channel_space_um = 500.0f;
    int    channel_aline    = 50;
    float  scan_range_um    = 32000.0f;
    int    scan_aline       = 3200;
    float  laserFreq_Hz     = 200.0f;
    float  frameSpeed_Hz    = 2.0f;
    float  x0_m             = -6e-3f;
    float  y0_m             = 4e-3f;
    float  dx_um            = 10.0f;
    float  dy_um            = 10.0f;
    int    nx               = 1200;
    int    ny               = 800;
};

// ---- 第三组：重建参数 ----
struct ReconParams {
    int    delay              = 141;
    float  sos1_mps           = 1500.0f;
    float  sos2_mps           = 1560.0f;
    float  density1_kgm3      = 1000.0f;
    float  density2_kgm3      = 1100.0f;
    float  mediumLinePos_m    = -1e-6f;
    bool   isDualSoS          = false;
    bool   isPlotEveryFiber   = false;
    bool   isCutoffLoc        = false;
    int    cutoffLoc          = 2501;
    bool   isReadFiberPosition = true;
    bool   isSpliceRecon      = false;
    QString filterType        = "Bandpass";
    float  filterFreqLow_Hz   = 1e6f;
    float  filterFreqHigh_Hz  = 40e6f;
    float  threshold          = 1000.0f;
    float  dynRange_db        = 50.0f;
    QString outputType        = "RF";
    // 时延与周期参数
    int    delayTimePoint    = 1601;  // 时延补偿点（采样点）
    int    s1Period          = 2500;  // S1 周期（采样点）

    // 中心对齐
    bool   isCenterAlign     = true;  // 中心对齐后边界清零

    // 配准数据（由加载的 peizhun 文件填充）
    QVector<int> xPeizhun;                    // x 方向通道位置校准
    QVector<int> yPeizhun;                    // y 方向通道位置校准
    QVector<int> caliCardDelay{0,2,2,2,4,6,6,8};        // 采集卡标定时延
    QVector<int> caliFiberDelay{534,484,428,372,322,264,282,202}; // 光纤标定时延
};
