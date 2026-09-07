#pragma once

#include <cstdint>
#include <string>
#include <vector>

// =====================================================================
// RingRecon — 双波长环形扫描 DAS 实时重建（MATLAB Handoff 的 C++ CPU 移植）
//
// 与 Handoff 逐块链路的对应关系：
//   simulateAcquisition('next')          -> readRawBlock / splitBlock
//   preprocessBlock.m                    -> preprocessBlock
//   das_recon_incremental_loop + core    -> dasReconAppend + normalizedImage
//
// 图像内存布局与 MATLAB 输出一致：vector 大小为 nx*ny，
// 索引 = ix*ny + iy（column-major，对应 MATLAB img(y,x) 的线性下标）。
// =====================================================================

namespace ringrecon {

struct ReconParams {
    double fs = 250e6;              // 采样率 [Hz]
    double c = 1490.0;              // 声速 [m/s]
    double R = 6.57e-3;             // 环形阵列半径 [m]
    double fov = 36e-3;             // 成像视场直径 [m]
    double gridSize = 0.1e-3;       // 网格尺寸 [m]
    double coverageDeg = 360.0;     // 本块覆盖角 [deg]（整帧时=总覆盖）
    double theta0Deg = 0.0;         // 覆盖起始角 [deg]
    double fovDeg = 360.0;          // 成像扇区角宽 [deg]
    double fovTheta0Deg = 0.0;      // 成像扇区起始角 [deg]
    double distanceWeightExponent = 1.0;
    double minDistance = 0.0;       // 0 = 自动取网格步长
    bool   maskOutOfRange = true;
    std::string interpolation = "linear";  // linear / nearest
    std::vector<double> soundSpeedRadii;   // 分层声速边界 [m]（空=单声速）
    std::vector<double> soundSpeeds;       // 分层声速 [m/s]（空=用 c）
};

struct PreprocessParams {
    int    systemDelay = 171;       // 本波长延时截断起点（1-based，对应 MATLAB sysDelay(w)）
    int    dbrmaskExtra = 0;        // wl2 额外扣除行数 = sysDelay(2)-sysDelay(1)
    int    maskLength = 300;        // DBR 强信号扣除行数
    bool   dbrRemove = true;
    bool   delayCut = true;
    bool   signalImpair = false;
    double imValue = 2000.0;
};

struct StreamConfig {
    int    sampDepth = 4000;
    int    alinesPerFrame = 4000;   // 双波长合计
    int    alinesPerBlock = 200;    // 双波长合计（必须偶数）
    int    wlOffset = 151;          // 帧内第一根 wl1 原始列（1-based）
    bool   shiftWL2 = true;
    int    nWavelengths = 2;        // 1 或 2
    PreprocessParams pre[2];        // [0]=wl1, [1]=wl2
    ReconParams recon;
};

struct IncrementalState {
    std::vector<float> acc;
    std::vector<float> accW;
    int nBlock = 0;
};

// 生成与 MATLAB linspace(-fov/2, fov/2, ceil(fov/gridSize)) 一致的网格
void makeGrid(double fov, double gridSize,
              std::vector<float>& xv, std::vector<float>& yv);

// 从 .dat（小端 double，一列一根 A-line）读取第 blockIndex 块原始数据
// blockIndex 从 0 开始；成功返回 true。
bool readRawBlock(const std::string& path, int64_t sampDepth, int wlOffset,
                  int alinesPerBlock, int blockIndex, std::vector<double>& raw);

// 读取帧内最后一根 wl2 原始列（ShiftWL2 首块 wrap 用）
bool readFrameLastWL2(const std::string& path, int64_t sampDepth, int wlOffset,
                      int alinesPerFrame, std::vector<double>& last);

// 原始块解交织：wl1/wl2 各 [sampDepth x nWlBlock]；可选 ShiftWL2 列对齐
void splitBlock(const std::vector<double>& raw, int sampDepth, int nWlBlock,
                bool shiftWL2,
                const std::vector<double>& prevWL2Last,
                const std::vector<double>& frameLastWL2,
                std::vector<double>& wl1, std::vector<double>& wl2,
                std::vector<double>& curWL2Last);

// 逐块预处理（输入 double，输出 float，与 MATLAB 先 double 预处理再 single 进 DAS 一致）
std::vector<float> preprocessBlock(const std::vector<double>& in,
                                   int Nt, int nCol,
                                   const PreprocessParams& p);

// 本块 DAS 增量反投影（等价 das_recon_incremental_loop 的一次调用）
// blockStartDeg / blockSpanDeg 与 MATLAB BlockStartDeg/BlockSpanDeg 一致。
void dasReconAppend(const std::vector<float>& bscan, int Nt, int nd,
                    const ReconParams& rp, double blockStartDeg, double blockSpanDeg,
                    const std::vector<float>& xv, const std::vector<float>& yv,
                    IncrementalState& st);

// 归一化显示图：acc ./ max(accW, 1e-12)
std::vector<float> normalizedImage(const IncrementalState& st);

}  // namespace ringrecon