#pragma once

// =====================================================================
// ZeroPhaseFilter — 双精度 Butterworth SOS 零相位滤波（阶段 B1）
//
// 阶段 A 冻结约定（CODEX_REPORTS/ring-zero-phase-pa-inversion-20260919/
// matlab/refZeroPhase.m 的 C++ 对照实现）：
//   - 双精度 Butterworth SOS 设计（butter→zp2sos 等价），n = 单程阶数（1–8）
//   - Wn = 2*fc/fs（fc 为单程 −3dB 截止）
//   - 奇对称延拓：两端各 nfact = 3n 样本
//   - 逐节稳态初始化（DF2T 对常输入的稳态），前向 → 翻转 → 前向 → 翻转
//   - 零相位复合增益 g²（前后向各一次增益）
//   - 输入必须有限、有效线长 > 3n，否则报错（不静默单向滤波/跳过）
//
// 本模块无跨 A-line 状态：每根完整 A-line 独立调用 apply()，工作区
// （延拓缓冲）在 FilterSet 内复用，系数按配置设计一次并缓存。
// =====================================================================

#include <cmath>
#include <string>
#include <vector>

namespace zerophase {

// 单个 SOS 节：y[k] = (b0*x[k] + b1*x[k-1] + b2*x[k-2] - a1*y[k-1] - a2*y[k-2]) / a0
// （设计后 a0 恒归一化为 1）
struct SosSection {
    double b[3] = {0.0, 0.0, 0.0};
    double a[3] = {1.0, 0.0, 0.0};
};

enum class Kind { Highpass, Lowpass };

// 滤波器设计参数与合法性校验结果
struct Design {
    Kind kind = Kind::Lowpass;
    int n = 0;                  // 单程阶数（1–8）；0 = 未设计/未启用
    double fcHz = 0.0;          // 截止频率（Hz，单程 −3dB）
    double fsHz = 0.0;          // 运行时采样率（Hz）
    double gain = 1.0;          // SOS 级联单程增益 g（零相位复合增益 = g²）
    std::vector<SosSection> sos;
    std::string error;          // 非空 = 设计失败原因

    bool ok() const { return error.empty() && n > 0 && !sos.empty(); }
};

// 设计一个单程 Butterworth 滤波器（SOS）。失败时 design.error 非空。
// 要求：1 <= n <= 8、fs > 0、0 < fc < fs/2、系数有限。
Design designFilter(Kind kind, int n, double fcHz, double fsHz);

// 一组按顺序应用的滤波器（HP 先、LP 后；各自可关闭）。
// enabled 语义 = 设计结果有效（ok 且 sos 非空）；全关时两个 Design 均为
// 默认构造（n=0、sos 空），不触发任何滤波。
struct FilterSet {
    Design hp;
    Design lp;
    bool hpEnabled() const { return hp.n > 0 && hp.ok(); }
    bool lpEnabled() const { return lp.n > 0 && lp.ok(); }
    bool anyEnabled() const { return hpEnabled() || lpEnabled(); }
};

// 设计组合（hpHz/lpHz < 0 或 0 表示该滤波器关闭；两者全关时不做任何
// 校验——调用方负责"全关不因无效截止值阻塞旧路径"）。
// 双开时要求 hpHz < lpHz。返回的 FilterSet 若 !ok() 给出具体原因。
FilterSet designSet(bool hpOn, double hpHz, int hpOrder,
                   bool lpOn, double lpHz, int lpOrder,
                   double fsHz, std::string *error);

// 零相位应用一个已设计滤波器（对齐 refZeroPhase.m）。
// x 长度必须 > 3*n（含在 ext 中复用的尾样本），否则返回 false 并设置 error。
// 输出写回 x（原地）。fsHz/系数已在 Design 中固化。
bool applyZeroPhase(const Design &d, std::vector<double> &x, std::string *error);

// 对一根完整 A-line 按顺序应用启用滤波器（HP → LP）。
// 全部成功返回 true；任一失败返回 false 并设置 error（x 可能已被部分改写，
// 调用方在失败路径上不得使用该线）。
bool applySet(const FilterSet &fs, std::vector<double> &x, std::string *error);

// 最短有效输入长度（各启用滤波器 3n 的最大值；无启用滤波器返回 0）
int minRequiredSamples(const FilterSet &fs);

// 配置组合前置校验（阶段 A C2 冻结规则表，逐通道/波长）。
// E = 该通道/波长实际 DBR 置零样本数（真实预处理语义，调用方按
// min(maskLength+dbrmaskExtra, Nt) 计算并传入；DBR 关闭传 0）。
// 规则：任一滤波启用且 E=0 → 允许；E>0 且 delayCut=true → 仅 E<D 允许；
//       E>0 且 delayCut=false → 拒绝。全关不引入新拒绝（返回 true）。
// name 用于报错信息（如 "通道3/波长2"）。
bool checkDbrCombination(bool filterEnabled, int E, int D, bool delayCut,
                        const std::string &name, std::string *error);

// C2 规则使用的实际置零长度：E = min(maskLength + dbrmaskExtra, Nt)，下限 0。
int actualZeroRows(bool dbrEnabled, int maskLength, int dbrmaskExtra, int Nt);

}  // namespace zerophase
