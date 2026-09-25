#pragma once

// ============================================================
// RingPhantomForward  合成体模前向模型（**参考实现**，非生产代码）
//
// 用途：为 D6（B1 A/B 对照基线）与 D7（B1 收益上限评估）提供**已知真值**的合成数据。
//
// 独立性（硬要求）：
//   本模块**不得调用任何生产/重建函数**（ringrecon::、ringrecon_inv::、CUDA）。
//   走时与几何全部自行实现 —— 否则 D6/D7 会变成「用生产代码验证生产代码」的自证。
//   重建侧仍可调用生产实现作为**被测对象**，那是正确用法。
//
// 模型口径（明确声明：这是**模型自洽的前向模型**，不是物理仿真器）：
//
//   bscan[j*Nt + i] = Σ_s  A_s · shape(t_i − τ_sj) · spread_sj · dir_sj
//
//   τ_sj    = |r_s − r_0j| / c + startDelaySec          直线走时
//   t_i     = i / fs                                     采样时刻
//   spread  = (d_ref / d_sj)^spreadExponent              几何扩散（0 = 关闭）
//   dir     = directivity ? max(0, cosα_sj) : 1          探头只收面向它的声
//   cosα_sj = (R_j − r_s·n̂_j) / d_sj                    n̂_j 为探测器单位径向
//
// 不建模：折射/透射、分层声速（前向侧暂用单一声速 c）、有限探头尺寸、噪声、量化。
// 上述每项都是 D6/D7 的已知限制，不得当作模型能力宣称。
//
// 脉冲波形 shape(u)，u = t − τ：
//   Delta      理想冲激（仅 u 最近采样点非零）—— 用于解析核对
//   GaussDeriv 高斯一阶导 −(u/σ²)·exp(−u²/2σ²) —— **N 形（正后随负）**，默认
//   Ricker     (1 − u²/σ²)·exp(−u²/2σ²) —— 带限墨西哥帽
//
// 选择 GaussDeriv 作默认，是因为文献指出实测 PA 波形是**双极 N 形**（正峰后跟负峰），
// 而 UBP 的反演核 b = 2p − 2t·p′ 正是针对这种波形设计的；用冲激做默认会让
// D7 测不出反演核的差异。
// ============================================================

#include <cstddef>
#include <vector>

namespace ringphantom {

// 点吸收体（已知真值）
struct Source {
    double x = 0.0;          // [m]
    double y = 0.0;          // [m]
    double amplitude = 1.0;  // 初始声压幅值（任意单位；线性进入前向）
};

// 采集几何（与项目默认值一致，见 ring_recon_cuda_set_defaults）
struct Geometry {
    double fs = 250e6;       // 采样率 [Hz]（项目 FPGA_ADC_FREQ_HZ）
    double c = 1490.0;       // 声速 [m/s]（单声速；分层为已知限制）
    int    sampDepth = 4000; // 每根 A-line 采样点数
};

enum class PulseShape {
    Delta = 0,       // 理想冲激
    GaussDeriv = 1,  // 高斯一阶导（N 形，默认）
    Ricker = 2,      // 带限墨西哥帽
};

struct ForwardParams {
    PulseShape pulse = PulseShape::GaussDeriv;
    double pulseWidthSec = 4.0e-9;   // σ [s]（4 ns @250MHz ≈ 1 个采样点）
    double spreadExponent = 0.0;     // 0 = 不计几何扩散
    double referenceDistance = 1.0;  // spread 的参考距离 [m]
    bool   directivity = false;      // true = 只接收面向探头的声（cosα 加权）
    double startDelaySec = 0.0;      // 时间轴原点偏移 [s]（可模拟 systemDelay）
};

// 脉冲波形（纯函数，便于 D6/D7 单独核对）
double pulseValue(PulseShape shape, double u, double sigma);

// 前向仿真：生成 bscan，列主序 bscan[j*Nt + i]，与 dasReconAppend 输入布局一致。
//   thetaRad  —— 每根 A-line 的探测器方位角 [rad]，长度 nd
//   radii     —— 每根 A-line 的探测器半径 [m]，长度 nd（multiRadius 用）
//   返回值长度 = Nt * nd
std::vector<float> simulate(const std::vector<Source>& sources,
                            const std::vector<double>& thetaRad,
                            const std::vector<double>& radii,
                            const Geometry& geo,
                            const ForwardParams& fp);

// 便捷：均匀圆盘体模（D7 用；返回的是点源集合，可直接喂 simulate）
std::vector<Source> uniformDisk(double centerX, double centerY, double radius,
                                double amplitude, int nPointsPerRadius = 8);

}  // namespace ringphantom
