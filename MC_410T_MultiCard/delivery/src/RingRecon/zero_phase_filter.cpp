#include "zero_phase_filter.h"

#include <algorithm>
#include <sstream>

namespace zerophase {

namespace {

constexpr double kPi = 3.14159265358979323846;

// ---- Butterworth 模拟原型极点（s 域，截止 1 rad/s）----
// 低通：|H(jω)|² = 1/(1+ω^{2n})，极点 s_k = exp(jπ(2k+n+1)/(2n))，k=0..n-1（左半平面）
// 高通：由低通经 s → 1/s 变换（双线性/解析均可；此处用 s 域解析变换，
//        与 MATLAB butter(2,'high') 模拟原型一致的极点倒置结构）。
//
// ---- 数字化：双线性变换 ----
// 低通（fc 归一化 Wn = 2*fc/fs）：T=2（标准双线性），预畸变常数
//   c = 1/tan(π*Wn/2)（即 MATLAB butter 使用的 warped = tan(π*Wn/2) 的倒数）
//   s → c*(1-z)/(1+z)
// 高通：极点先取倒数（p → 1/p，零点 s=∞→原点），再做标准双线性 s → (1+z)/(1-z)
//   （即 c=1 的高通双线性形状，MATLAB butter(n,Wn,'high') 的标准路径）
//
// 与 MATLAB butter 的对应：MATLAB 用 zp2butter 生成模拟零极点后经 bilinear 变换，
// 再 zp2sos 配对。为保证数值一致（供阶段 A 参考向量对照），这里按同样的
// 极点配对（就近共轭配对）与增益缩放实现，并对 n<=8 的常用参数做有限性校验。

struct Complex {
    double re = 0.0, im = 0.0;
};

// 双二阶（biquad）节系数（a0 已归一化为 1）
struct Biquad {
    double b[3] = {0.0, 0.0, 0.0};
    double a[2] = {0.0, 0.0};   // a1, a2
    double g = 1.0;             // 本节增益（乘入分子）
};

// 一阶节（奇数阶的实极点）：y[k] = g*(b0*x[k] + b1*x[k-1]) / (1 + a1 z^-1)
struct FirstOrder {
    double b[2] = {0.0, 0.0};
    double a1 = 0.0;
    double g = 1.0;
};

// Butterworth 模拟原型极点（buttap 语义，|s_k|=1，左半平面，奇数阶含 -1）
std::vector<Complex> analogPoles(int n) {
    std::vector<Complex> poles(static_cast<size_t>(n));
    for (int k = 0; k < n; ++k) {
        const double theta = kPi * static_cast<double>(2 * k + n + 1) /
                             static_cast<double>(2 * n);
        poles[static_cast<size_t>(k)] = {std::cos(theta), std::sin(theta)};
    }
    return poles;
}

// 数字化（与 MATLAB butter 逐位一致的路径，R2023a butter.m 源码核对）：
//   fs = 2；u = 2·fs·tan(π·Wn/fs) = 4·tan(πWn/2)  （预畸变）
//   低通模拟极点 pl = u·p_a（lp2lp）；高通 ph = u/p_a（lp2hp）
//   双线性（fs=2，T=1/fs=0.5）：z = (2 + p/2)/(2 − p/2)
//   分子零点：低通全在 z=−1，高通全在 z=+1
//   增益：低通 H(1)=1 → k=Π(1−z_p)/2ⁿ；高通 H(−1)=1 → k=Π(1+z_p)/2ⁿ
// 逐对共轭极点的二阶节：
//   分母 = (1 − z_p z⁻¹)(1 − z_p* z⁻¹) → [1, −2Re(z_p), |z_p|²]
//   分子：低通 g·[1,2,1]，高通 g·[1,−2,1]；每节增益按节分配（乘积=整体 k）
// 节增益分配：与 zp2sos 一致把总增益乘入各节分子（数值上任意分配等价，
//   这里每节等比开 nSos 次方，或直接乘入首节；取每节 g_i = k^(1/nSos)）。
struct DigitalPolePair {
    double re = 0.0, im = 0.0;   // 数字共轭极点 z_p
};

// 计算 n 阶滤波器的全部数字极点（返回共轭对 + 可选实极点）
std::vector<Complex> digitalPoles(Kind kind, int n, double Wn,
                                  bool *hasReal, double *realPole) {
    const std::vector<Complex> pa = analogPoles(n);
    const double u = 4.0 * std::tan(kPi * Wn * 0.5);
    std::vector<Complex> out;
    out.reserve(pa.size());
    *hasReal = false;
    *realPole = 0.0;
    for (const Complex &p : pa) {
        Complex s;
        if (kind == Kind::Lowpass) {
            s = {u * p.re, u * p.im};          // lp2lp: u·p_a
        } else {
            // lp2hp: u/p_a = u·conj(p_a)/|p_a|² = u·conj(p_a)（|p_a|=1）
            s = {u * p.re, -u * p.im};
        }
        // bilinear fs=2: z = (2 + s/2)/(2 - s/2)
        const double denr = 2.0 - 0.5 * s.re;
        const double deni = -0.5 * s.im;
        const double dd = denr * denr + deni * deni;
        const double numr = 2.0 + 0.5 * s.re;
        const double numi = 0.5 * s.im;
        out.push_back({(numr * denr + numi * deni) / dd,
                       (numi * denr - numr * deni) / dd});
    }
    // 分离实极点（奇数阶）：极点列表中 im≈0 的项
    for (auto it = out.begin(); it != out.end();) {
        if (std::fabs(it->im) < 1e-12) {
            *hasReal = true;
            *realPole = it->re;
            it = out.erase(it);
        } else {
            ++it;
        }
    }
    return out;   // 仅剩共轭对（每对一个代表，配对时取一半）
}

bool allFinite(const Biquad &bq) {
    const double v[] = {bq.b[0], bq.b[1], bq.b[2], bq.a[0], bq.a[1], bq.g};
    for (double x : v)
        if (!std::isfinite(x)) return false;
    return true;
}

bool allFinite(const FirstOrder &fo) {
    const double v[] = {fo.b[0], fo.b[1], fo.a1, fo.g};
    for (double x : v)
        if (!std::isfinite(x)) return false;
    return true;
}

}  // namespace

Design designFilter(Kind kind, int n, double fcHz, double fsHz) {
    Design d;
    d.kind = kind;
    d.n = n;
    d.fcHz = fcHz;
    d.fsHz = fsHz;

    if (n < 1 || n > 8) {
        d.error = "滤波器阶数必须在 1–8 之间（当前 " + std::to_string(n) + "）";
        return d;
    }
    if (!(fsHz > 0.0) || !std::isfinite(fsHz)) {
        d.error = "采样率必须为正有限值";
        return d;
    }
    if (!(fcHz > 0.0) || !std::isfinite(fcHz) || !(fcHz < fsHz * 0.5)) {
        std::ostringstream oss;
        oss << "截止频率必须在 (0, " << fsHz * 0.5 << ") Hz 内（当前 "
            << fcHz << " Hz）";
        d.error = oss.str();
        return d;
    }

    const double Wn = 2.0 * fcHz / fsHz;   // MATLAB butter 归一化频率
    if (!(Wn > 0.0) || !(Wn < 1.0) || !std::isfinite(Wn)) {
        d.error = "归一化截止频率必须在 (0,1) 内";
        return d;
    }

    // 数字极点（MATLAB butter 路径：u·p 或 u/p 预畸变 + bilinear fs=2）
    bool hasReal = false;
    double realPole = 0.0;
    const std::vector<Complex> cp = digitalPoles(kind, n, Wn, &hasReal, &realPole);
    // cp 按模拟极点生成顺序排列：k 与 n-1-k 的极点互为共轭相邻成对
    //（analogPoles 顺序 exp(jπ(2k+n+1)/2n) 对 k=0..n-1 依次逆时针，
    // 数字化后保持共轭对相邻；此处不重排序，直接两两取对）。

    // 每节增益：总增益 k（低通 Π(1−p)/2ⁿ、高通 Π(1+p)/2ⁿ，含共轭对的
    // 复数乘积，即共轭对贡献 |1∓z|²）按节数均分（g_i = k^(1/nSections)）。
    double kTotal = 1.0;
    for (size_t i = 0; i < cp.size(); i += 2) {
        // 共轭对乘积 |1−z|²（低通）/ |1+z|²（高通）
        const Complex &z = cp[i];
        const double dr = (kind == Kind::Lowpass) ? (1.0 - z.re) : (1.0 + z.re);
        const double di = (kind == Kind::Lowpass) ? (-z.im) : z.im;
        kTotal *= dr * dr + di * di;
    }
    if (hasReal) {
        const double dr = (kind == Kind::Lowpass) ? (1.0 - realPole) : (1.0 + realPole);
        kTotal *= dr;
    }
    kTotal /= std::pow(2.0, static_cast<double>(n));
    if (!std::isfinite(kTotal) || kTotal <= 0.0) {
        d.error = "滤波器增益计算异常（数值病态）";
        return d;
    }

    const int nSections = static_cast<int>(cp.size() / 2) + (hasReal ? 1 : 0);
    const double gPer = std::pow(kTotal, 1.0 / static_cast<double>(nSections));

    // 组装二阶节：分母 [1, −2Re(z), |z|²]；分子低通 [1,2,1] / 高通 [1,−2,1]
    for (size_t i = 0; i < cp.size(); i += 2) {
        const Complex &z = cp[i];
        SosSection s;
        const double zz = z.re * z.re + z.im * z.im;
        s.a[0] = 1.0;
        s.a[1] = -2.0 * z.re;
        s.a[2] = zz;
        if (kind == Kind::Lowpass) {
            s.b[0] = gPer; s.b[1] = 2.0 * gPer; s.b[2] = gPer;
        } else {
            s.b[0] = gPer; s.b[1] = -2.0 * gPer; s.b[2] = gPer;
        }
        d.sos.push_back(s);
    }
    // 一阶节（奇数阶实极点）：分母 [1, −z_r]；分子低通 [1,1] / 高通 [1,−1]
    if (hasReal) {
        SosSection s;
        s.a[0] = 1.0;
        s.a[1] = -realPole;
        s.a[2] = 0.0;
        if (kind == Kind::Lowpass) {
            s.b[0] = gPer; s.b[1] = gPer;
        } else {
            s.b[0] = gPer; s.b[1] = -gPer;
        }
        s.b[2] = 0.0;
        d.sos.push_back(s);
    }

    for (const SosSection &s : d.sos) {
        const double v[] = {s.b[0], s.b[1], s.b[2], s.a[0], s.a[1], s.a[2]};
        for (double x : v) {
            if (!std::isfinite(x)) {
                d.sos.clear();
                d.error = "滤波器系数计算结果非有限（截止/阶数组合数值病态）";
                return d;
            }
        }
    }

    // 通带增益自检（双线性结构理论上精确为 1）
    double dcGain = 1.0, nyqGain = 1.0;
    for (const SosSection &s : d.sos) {
        dcGain *= (s.b[0] + s.b[1] + s.b[2]) / (s.a[0] + s.a[1] + s.a[2]);
        nyqGain *= (s.b[0] - s.b[1] + s.b[2]) / (s.a[0] - s.a[1] + s.a[2]);
    }
    const double wantGain = (kind == Kind::Lowpass) ? dcGain : nyqGain;
    if (!std::isfinite(wantGain) || std::fabs(wantGain - 1.0) > 1e-6) {
        d.sos.clear();
        d.error = "滤波器通带增益归一失败（数值病态）";
        return d;
    }
    if (std::fabs(wantGain - 1.0) > 1e-9) {
        const double corr = 1.0 / wantGain;
        for (auto &s : d.sos) {
            for (int i = 0; i < 3; ++i) s.b[i] *= corr;
        }
    }
    d.gain = 1.0;   // 通带增益已归一；filtfilt 复合增益 = gain² = 1
    return d;
}

FilterSet designSet(bool hpOn, double hpHz, int hpOrder,
                   bool lpOn, double lpHz, int lpOrder,
                   double fsHz, std::string *error) {
    // 全关：两个 Design 均为默认构造（n=0、sos 空）→ anyEnabled()=false，
    // 不做校验（旧路径保持；调用方不因无效截止值阻塞）。
    FilterSet fs;
    if (hpOn) {
        fs.hp = designFilter(Kind::Highpass, hpOrder, hpHz, fsHz);
        if (!fs.hp.ok()) {
            if (error) *error = "高通: " + fs.hp.error;
            return fs;
        }
    }
    if (lpOn) {
        fs.lp = designFilter(Kind::Lowpass, lpOrder, lpHz, fsHz);
        if (!fs.lp.ok()) {
            if (error) *error = "低通: " + fs.lp.error;
            return fs;
        }
    }
    if (hpOn && lpOn && !(hpHz < lpHz)) {
        std::ostringstream oss;
        oss << "高通截止 " << hpHz << " Hz 必须低于低通截止 " << lpHz << " Hz";
        if (error) *error = oss.str();
        fs.hp.error = oss.str();
        return fs;
    }
    if (error) error->clear();
    return fs;
}

bool applyZeroPhase(const Design &d, std::vector<double> &x, std::string *error) {
    if (!d.ok() || d.sos.empty()) {
        if (error) *error = d.ok() ? "滤波器未设计" : d.error;
        return false;
    }
    const int n = d.n;
    const int nfact = 3 * n;
    const int Nx = static_cast<int>(x.size());
    if (Nx <= nfact) {
        std::ostringstream oss;
        oss << "有效线长 " << Nx << " 必须大于延拓长度 " << nfact
            << "（3×单程阶数 " << n << "）";
        if (error) *error = oss.str();
        return false;
    }
    for (double v : x) {
        if (!std::isfinite(v)) {
            if (error) *error = "输入 A-line 含 NaN/Inf";
            return false;
        }
    }

    const int nSos = static_cast<int>(d.sos.size());
    const int extLen = Nx + 2 * nfact;
    std::vector<double> ext(static_cast<size_t>(extLen));

    // 奇对称延拓（与 refZeroPhase.m 一致，一基 MATLAB 语义转零基）：
    //   ext(1..nfact) = 2*x(1) - x(nfact+1:-1:2)
    //   ext(nfact+1..nfact+Nx) = x
    //   ext(末 nfact) = 2*x(end) - x(end-1:-1:end-nfact)
    for (int i = 0; i < nfact; ++i) {
        ext[static_cast<size_t>(i)] = 2.0 * x[0] - x[static_cast<size_t>(nfact - i)];
    }
    for (int i = 0; i < Nx; ++i) ext[static_cast<size_t>(nfact + i)] = x[static_cast<size_t>(i)];
    for (int i = 0; i < nfact; ++i) {
        ext[static_cast<size_t>(nfact + Nx + i)] =
            2.0 * x[static_cast<size_t>(Nx - 1)] - x[static_cast<size_t>(Nx - 1 - (i + 1))];
    }

    // 逐节级联整段滤波（原地），含逐节稳态初始化（对常输入 x0 的 DF2T 稳态，
    // 与 refZeroPhase.m 的 sosZiSteady 相同公式：级联传递上节稳态输出）。
    // DF2T 递推（MATLAB filter 语义）：
    //   y[k] = b0*x[k] + s1
    //   s1'  = b1*x[k] - a1*y[k] + s2
    //   s2'  = b2*x[k] - a2*y[k]
    // 稳态（常输入 xin）：yk = (Σb/Σa)·xin；
    //   s1 = yk - b0·xin；s2 = yk·(1+a1) - xin·(b0+b1)
    auto filterAll = [&d, nSos, extLen](std::vector<double> &buf, double x0) {
        std::vector<double> s1(static_cast<size_t>(nSos), 0.0);
        std::vector<double> s2(static_cast<size_t>(nSos), 0.0);
        {
            double xin = x0;
            for (int k = 0; k < nSos; ++k) {
                const SosSection &sec = d.sos[static_cast<size_t>(k)];
                const double sb = sec.b[0] + sec.b[1] + sec.b[2];
                const double sa = sec.a[0] + sec.a[1] + sec.a[2];
                const double yk = (sb / sa) * xin;
                s1[static_cast<size_t>(k)] = yk - sec.b[0] * xin;
                s2[static_cast<size_t>(k)] = yk * (1.0 + sec.a[1]) -
                                             xin * (sec.b[0] + sec.b[1]);
                xin = yk;
            }
        }
        for (int k = 0; k < nSos; ++k) {
            const SosSection &sec = d.sos[static_cast<size_t>(k)];
            double w1 = s1[static_cast<size_t>(k)];   // s1
            double w2 = s2[static_cast<size_t>(k)];   // s2
            for (int i = 0; i < extLen; ++i) {
                const double xi = buf[static_cast<size_t>(i)];
                const double y = sec.b[0] * xi + w1;
                const double ns1 = sec.b[1] * xi - sec.a[1] * y + w2;
                const double ns2 = sec.b[2] * xi - sec.a[2] * y;
                buf[static_cast<size_t>(i)] = y;
                w1 = ns1;
                w2 = ns2;
            }
        }
    };

    // 前向 → 翻转 → 前向 → 翻转（refZeroPhase 约定；每程起点取当前首样本）
    filterAll(ext, ext[0]);
    std::reverse(ext.begin(), ext.end());
    filterAll(ext, ext[0]);
    std::reverse(ext.begin(), ext.end());

    // 复合增益 g²（本实现通带增益归一为 1，g=1；保留乘法以对齐约定）
    const double g2 = d.gain * d.gain;
    for (int i = 0; i < Nx; ++i) {
        x[static_cast<size_t>(i)] = g2 * ext[static_cast<size_t>(nfact + i)];
    }
    for (double v : x) {
        if (!std::isfinite(v)) {
            if (error) *error = "滤波输出含 NaN/Inf";
            return false;
        }
    }
    return true;
}

bool applySet(const FilterSet &fs, std::vector<double> &x, std::string *error) {
    if (fs.hpEnabled() && !applyZeroPhase(fs.hp, x, error)) return false;
    if (fs.lpEnabled() && !applyZeroPhase(fs.lp, x, error)) return false;
    return true;
}

int minRequiredSamples(const FilterSet &fs) {
    int m = 0;
    if (fs.hpEnabled()) m = std::max(m, 3 * fs.hp.n);
    if (fs.lpEnabled()) m = std::max(m, 3 * fs.lp.n);
    return m;
}

bool checkDbrCombination(bool filterEnabled, int E, int D, bool delayCut,
                         const std::string &name, std::string *error) {
    if (error) error->clear();
    if (!filterEnabled) return true;
    if (E <= 0) return true;
    if (delayCut) {
        if (E >= D) {
            std::ostringstream oss;
            oss << "DBR 置零末端必须早于延时裁剪起点（" << name
                << "：实际置零 E=" << E << " ≥ sysDelay D=" << D
                << "，且零相位滤波启用、延时裁剪开启）";
            if (error) *error = oss.str();
            return false;
        }
        return true;
    }
    std::ostringstream oss;
    oss << "零相位滤波与未裁剪的 DBR 置零前缀不兼容，请启用延时裁剪或取消 DBR 置零（"
        << name << "：滤波启用、实际置零 E=" << E
        << ">0、延时裁剪关闭——本版不支持该组合，"
           "不是数学禁忌；可通过主动调整裁剪/DBR 组合解决）";
    if (error) *error = oss.str();
    return false;
}

int actualZeroRows(bool dbrEnabled, int maskLength, int dbrmaskExtra, int Nt) {
    if (!dbrEnabled) return 0;
    if (maskLength < 0) maskLength = 0;
    if (dbrmaskExtra < 0) dbrmaskExtra = 0;
    const long long zr = static_cast<long long>(maskLength) + dbrmaskExtra;
    if (zr <= 0) return 0;
    if (zr >= Nt) return Nt;
    return static_cast<int>(zr);
}

}  // namespace zerophase
