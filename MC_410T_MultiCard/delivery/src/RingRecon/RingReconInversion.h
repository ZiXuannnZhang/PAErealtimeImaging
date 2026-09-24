#pragma once

// ============================================================
// RingReconInversion  反演核与几何权重（B1「UBP 成对切换」的合同本体）
//
// 纯标量、零依赖（不引标准库容器 / Qt / CUDA），因此
// `ring_recon.cpp`（CPU reference）与 `ring_recon_cuda.cu`（生产核）可以共用同一份
// 表达式 —— 让 S2 的守卫测试真正守到生产算术，而不是只守一份手抄公式。
//
// 两条硬约束的可执行形式：
//
//   R2 成对切换：信号项与权重必须一起换。用**单一** InversionMode 字段表达，
//      结构上不可能「只换信号项不换权重」（审核 E1：两者差 R/d）。
//
//   G2 同源：反演核 b = 2p − 2r̃·p′ = −2r̃²·∂(p/r̃)/∂r̃ 成立的前提是 p 与 p′
//      来自同一个信号数组的同一位置。inversionKernel() 只接受标量 p 与 dp，
//      不接受两个数组；调用方在同一次插值中取得两者，结构上无法传入不同源。
//
// 参考：M. Xu & L. V. Wang, "Universal back-projection algorithm for
//       photoacoustic computed tomography," Phys. Rev. E 71, 016706 (2005),
//       Eq.(20)–(22)。
//
// 数值口径：权重表达式刻意与既有生产算术**逐字同形**（`wscale * dotp / …`），
//       模板按实参类型运算（float 进 float 出），以免改变全关路径的浮点结果
//       （守卫 G4 的冻结基准）。
// ============================================================

namespace ringrecon_inv {

// 成对开关：Ubp 同时切换信号项与权重，不允许只切其一。
enum class InversionMode : int {
    Das = 0,   // 现有行为：信号 = p，权重 = Δθ·cosα/d
    Ubp = 1,   // 光声反演：信号 = 2p − 2t·p′，权重 = R·Δθ·cosα/d²
};

// ---- 权重 ----
//
// 记号与生产代码一致：
//   wscale = -arc（arc = 逐线平均角步长，弧度；含变迹乘子）
//   dotp   = proj - R² = r·r_s - R²
//   dsafe  = max(d, minDist)，d = |r - r_s|
//   cosα   = (R - r·n̂)/d = (R² - proj)/(R·d)
//
// 现有 DAS 权重（= Δθ·cosα/d）：
//     w = wscale * dotp / (R * dsafe²)
// UBP 立体角权重（= R·Δθ·cosα/d²）：
//     dΩ = wscale * dotp / dsafe³        （注意：分母无 R，幂次为 3）
// 二者之比恒为 R/d（随像素变化）—— 审核 E1。

// 现有权重：与 ring_recon.cpp / ring_recon_cuda.cu 的既有表达式逐字同形。
template <typename T>
inline T dasWeight(T wscale, T dotp, T R, T dsafeP2) {
    return wscale * dotp / (R * dsafeP2);          // dsafeP2 = dsafe*dsafe
}

// UBP 立体角权重。
template <typename T>
inline T ubpWeight(T wscale, T dotp, T dsafe) {
    return wscale * dotp / (dsafe * dsafe * dsafe);
}

// 按模式取权重。成对切换的「权重」半边。
template <typename T>
inline T weight(InversionMode mode, T wscale, T dotp, T R, T dsafe, T dsafeP2) {
    return (mode == InversionMode::Ubp) ? ubpWeight(wscale, dotp, dsafe)
                                        : dasWeight(wscale, dotp, R, dsafeP2);
}

// ---- 反演信号项（成对切换的「信号」半边）----
//
//   b = 2p − 2t·p′
//
// t 用**秒**（t = r̃/c，或等价地 tf/fs）。若误用采样点序号，导数项会差 fs 倍
// （约 2.5e8），守卫 G2 有反证。
//
// 恒等式（可自验，前提 G2）：
//   b = 2p − 2r̃·∂p/∂r̃ = −2r̃²·∂/∂r̃(p/r̃)
//
// 同源约束：p 与 dp 必须来自同一数组的同一位置。本函数只收标量，不收数组。
template <typename T>
inline T inversionKernel(T p, T dp, T tSeconds) {
    return (T)2 * p - (T)2 * tSeconds * dp;
}

// 按模式取信号值。Das 直接透传，Ubp 走反演核。
template <typename T>
inline T signalValue(InversionMode mode, T p, T dp, T tSeconds) {
    return (mode == InversionMode::Ubp) ? inversionKernel(p, dp, tSeconds) : p;
}

// ---- 中心差分导数（沿采样维，逐 A-line 独立，无跨线状态）----
//
// dp/dn = (v[i+1] − v[i-1]) / 2，端点单侧；再乘 fs 得 dp/dt。
// 写成独立纯函数以便 .cpp 与 .cu 共用，并让 S2 守卫可直接断言。
//
// 【开放项】导数前是否先低通（B2）取决于实测噪声（分发任务 D1），骨架暂不加；
// 一旦 D1 给出截止区间，应在此处或其调用点补上，且不得只对导数支路滤波
// （那会破坏 G2 同源，见前提文档 S3 结论）。
template <typename T>
inline void derivativeCentral(const T* v, int n, T fs, T* out) {
    if (n <= 0) return;
    if (n == 1) { out[0] = (T)0; return; }
    out[0]     = (v[1]     - v[0])     * fs;      // 端点单侧
    out[n - 1] = (v[n - 1] - v[n - 2]) * fs;      // 端点单侧
    for (int i = 1; i < n - 1; ++i)
        out[i] = (v[i + 1] - v[i - 1]) * (T)0.5 * fs;
}

}  // namespace ringrecon_inv
