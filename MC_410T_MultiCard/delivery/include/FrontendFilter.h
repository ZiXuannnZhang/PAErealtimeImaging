#pragma once
#include <cstddef>
#include <string>
#include <vector>

#include "Constants.h"

// ============================================================
// FrontendFilter  逐 A-line 零相位高/低通滤波（Frontend Preprocessing Stage）
//
// 只服务于 FrontendPreprocessor::processFrontendSignal() 这唯一插入点。
// 滤波只作用于两条前端显示路径（时域/频域信号显示、实时成像）消费的 frontend
// clone；保存路径与发布路径消费的 raw TriggerGroup 不经过本模块。
//
// 结构（任务 3.3）：
//   * 高通、低通各自是一个独立的 N 阶 Butterworth 数字滤波器，以二阶节（SOS）
//     级联表示，每节 [b0 b1 b2 a0 a1 a2] 且 a0 = 1；
//   * 节内实现为直接 II 型转置结构，按节顺序级联；
//   * 采用前向 + 后向（filtfilt）执行得到零相位结果。
//
// 采样率固定 FPGA_ADC_FREQ_HZ（250 MHz），不是可调参数。本模块刻意不使用
// SAMPLE_FREQ_HZ / SAMPLE_INTERVAL_NS / DIFF_SAMPLE_RATE_HZ / FREQ_SCALE_KHZ
// 参与任何换算（见任务 3.2）。
//
// 逐 A-line 独立：一次 apply() 的输入是一条通道在一个 TriggerGroup 内的完整
// 采样序列。Bank 设计完成后即不可变，不在 A-line 之间、触发之间、卡之间保留
// 或传递任何滤波状态、历史样本或拼接样本；每条 A-line 的滤波器初始状态恒为零。
// ============================================================

namespace frontend_filter {

//  采样率与阶数边界
constexpr double kSampleRateHz = FPGA_ADC_FREQ_HZ;   // 250e6，固定
constexpr double kNyquistMhz   = 125.0;              // 界面截止频率开区间 (0, 125) MHz
constexpr int    kMinOrder     = 1;
constexpr int    kMaxOrder     = 8;

//  滤波参数（任务 3.4）。界面以 MHz 为单位，设计计算一律换算为 Hz。
struct Config {
    bool   hpEnable     = true;
    double hpCutoffMhz  = 0.4;
    int    hpOrder      = 2;
    bool   lpEnable     = true;
    double lpCutoffMhz  = 60.0;
    int    lpOrder      = 2;

    bool operator==(const Config& other) const noexcept {
        return hpEnable == other.hpEnable && hpCutoffMhz == other.hpCutoffMhz &&
               hpOrder == other.hpOrder && lpEnable == other.lpEnable &&
               lpCutoffMhz == other.lpCutoffMhz && lpOrder == other.lpOrder;
    }
    bool operator!=(const Config& other) const noexcept { return !(*this == other); }
};

//  参数校验（任务 3.4 校验规则，下发前执行）
enum class Validation {
    Ok = 0,
    HpCutoffRange,   // 高通截止频率不在 (0, 125) MHz
    HpOrderRange,    // 高通阶数不在 1 ~ 8
    LpCutoffRange,   // 低通截止频率不在 (0, 125) MHz
    LpOrderRange,    // 低通阶数不在 1 ~ 8
    BandOrder        // 两路都启用时低通截止 <= 高通截止
};

//  人类可读的拒绝原因（界面提示与测试断言共用）。
const char* validationMessage(Validation result) noexcept;
Validation  validate(const Config& config) noexcept;

//  一节二阶（或一阶）直接 II 型转置结构：[b0 b1 b2 a0 a1 a2]，a0 = 1。
//  N 为奇数时级联末尾含一个一阶节，其 b2 = a2 = 0。
struct Section {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0;
    double a1 = 0.0, a2 = 0.0;
};

//  一条滤波路径设计完成后的不可变 SOS 级联系数。
struct Coefficients {
    std::vector<Section> sections;
    int order = 0;
    bool empty() const noexcept { return sections.empty(); }
};

// ── 端部延拓规则（任务 3.5 步骤 1）────────────────────────────────
//  奇对称反射，两端各延拓 L 点：
//    左端第 k 点（k = 1 … L，自 x[0] 向外计数）取 2·x[0] − x[k]；
//    右端第 j 点（j = 1 … L，自 x[n−1] 向外计数）取 2·x[n−1] − x[n−1−j]。
//  存放顺序：缓冲区最左为 k = L，紧邻 x[0] 的为 k = 1。
//
//  下列两个函数即 filtfilt 实际采用的延拓/截取规则，导出以便逐点断言
//  （任务 7 检查项 5「用可复算的已知序列断言实际采用的延拓与截取规则」）。
// ────────────────────────────────────────────────────────────────

//  实际采用的单侧延拓点数：n < 2 时为 0；否则 min(3 × 阶数, n − 1)。
std::size_t extensionLength(std::size_t n, int order) noexcept;

//  按上述规则生成长度 n + 2L 的延拓序列。
std::vector<float> oddReflectExtend(const float* x, std::size_t n, std::size_t L);

// ── 滤波器设计（任务 3.3）────────────────────────────────────────
//  采样率 fs = kSampleRateHz。给定截止频率 fc 计算预畸变模拟截止
//  Ω = 2 · fs · tan(π · fc / fs)，构造 N 阶模拟 Butterworth 原型
//  p_k = Ω · exp(j · π · (2k + N − 1) / (2N))，k = 1 … N：
//    低通：使用该原型直接得到 H_lp(s)，N 个零点位于无穷远；
//    高通：由同一原型做谱变换 s -> Ω² / s 得到 H_hp(s)，N 个零点位于 s = 0
//          （等价于对单位截止原型做 s -> Ω / s；这样高通 −3 dB 截止才落在 fc）。
//  双线性变换 s = 2 · fs · (z − 1)/(z + 1) 得到 H(z)，增益归一化：低通在 DC
//  （z = 1）处增益为 1，高通在 Nyquist（z = −1）处增益为 1，再分解为二阶节。
//  参数非法时抛出 std::invalid_argument（调用方负责不把异常外泄出 worker）。
// ────────────────────────────────────────────────────────────────
Coefficients designLowpass(int order, double cutoffHz);
Coefficients designHighpass(int order, double cutoffHz);

//  单向滤波：SOS 级联、每节直接 II 型转置、节内初始状态为零。
void filterForward(const Coefficients& coeff, float* x, std::size_t n);

//  零相位 filtfilt（任务 3.5）：
//    1. 端部延拓 L = 3 × 阶数（见上）；
//    2. 正向滤波一次，初始状态置零；
//    3. 时间反转后再滤波一次，初始状态置零；
//    4. 时间反转并截去两端各 L 点，长度回到 n，写回目标缓冲。
//  n < 2 时对本序列直接原样返回。
void filtfilt(const Coefficients& coeff, float* x, std::size_t n);

// ── 两路系数缓存（任务 3.6）──────────────────────────────────────
//  构造时按 config 一次性设计全部启用路径的 SOS 系数，之后不可变。参数变更由
//  FrontendPreprocessor 重新构造一个 Bank 并整体交换，因此 worker 线程读到的
//  始终是一致的完整系数集，且无需在每帧处理时重新设计。
// ────────────────────────────────────────────────────────────────
class Bank {
public:
    //  给定合法配置并设计系数；配置非法时抛出 std::invalid_argument。
    explicit Bank(const Config& config);

    const Config& config() const noexcept { return config_; }
    const Coefficients& highpass() const noexcept { return hp_; }
    const Coefficients& lowpass() const noexcept { return lp_; }
    //  是否至少有一路启用（未启用的路不做系数设计，也不做运算）。
    bool active() const noexcept { return !hp_.empty() || !lp_.empty(); }

    //  对一条 A-line 原地执行「先高通、后低通」。每一路各是一次完整的前向-后向
    //  滤波，高通的输出作为低通的输入；任一路未启用则整路跳过。
    //  返回是否至少有一路真正执行了滤波（n < 2 时两路都原样返回，返回 false）。
    bool apply(float* x, std::size_t n) const;

private:
    Config config_;
    Coefficients hp_;
    Coefficients lp_;
};

}  // namespace frontend_filter
