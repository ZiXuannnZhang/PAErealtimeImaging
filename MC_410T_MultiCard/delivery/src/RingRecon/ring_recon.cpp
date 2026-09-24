#include "ring_recon.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

namespace ringrecon {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr float  kTiny = 1e-12f;
}  // namespace

void makeGrid(double fov, double gridSize,
              std::vector<float>& xv, std::vector<float>& yv) {
    const int n = static_cast<int>(std::ceil(fov / gridSize));
    xv.resize(n);
    yv.resize(n);
    const double half = fov * 0.5;
    for (int i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(n - 1);
        const float v = static_cast<float>(-half + t * fov);
        xv[i] = v;
        yv[i] = v;
    }
}

bool readRawBlock(const std::string& path, int64_t sampDepth, int wlOffset,
                  int alinesPerBlock, int blockIndex, std::vector<double>& raw) {
    const int64_t colStart1 = static_cast<int64_t>(wlOffset) +
                              static_cast<int64_t>(blockIndex) * alinesPerBlock;
    const int64_t byteOff = (colStart1 - 1) * sampDepth * 8;
    const int64_t count = static_cast<int64_t>(sampDepth) * alinesPerBlock;
    raw.resize(static_cast<size_t>(count));
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(byteOff);
    f.read(reinterpret_cast<char*>(raw.data()),
           static_cast<std::streamsize>(count * 8));
    return f.good() || f.gcount() == static_cast<std::streamsize>(count * 8);
}

bool readFrameLastWL2(const std::string& path, int64_t sampDepth, int wlOffset,
                      int alinesPerFrame, std::vector<double>& last) {
    const int64_t col1 = static_cast<int64_t>(wlOffset) + alinesPerFrame - 1;
    const int64_t byteOff = (col1 - 1) * sampDepth * 8;
    last.resize(static_cast<size_t>(sampDepth));
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(byteOff);
    f.read(reinterpret_cast<char*>(last.data()),
           static_cast<std::streamsize>(sampDepth * 8));
    return f.good() || f.gcount() == static_cast<std::streamsize>(sampDepth * 8);
}

void splitBlock(const std::vector<double>& raw, int sampDepth, int nWlBlock,
                bool shiftWL2,
                const std::vector<double>& prevWL2Last,
                const std::vector<double>& frameLastWL2,
                std::vector<double>& wl1, std::vector<double>& wl2,
                std::vector<double>& curWL2Last) {
    const size_t n = static_cast<size_t>(sampDepth) * nWlBlock;
    wl1.resize(n);
    wl2.resize(n);
    for (int c = 0; c < nWlBlock; ++c) {
        const double* src0 = raw.data() + static_cast<size_t>(2 * c) * sampDepth;
        const double* src1 = src0 + sampDepth;
        std::memcpy(wl1.data() + static_cast<size_t>(c) * sampDepth, src0,
                    static_cast<size_t>(sampDepth) * sizeof(double));
        std::memcpy(wl2.data() + static_cast<size_t>(c) * sampDepth, src1,
                    static_cast<size_t>(sampDepth) * sizeof(double));
    }

    curWL2Last.assign(wl2.end() - sampDepth, wl2.end());

    // ShiftWL2 决策（2026-08-07）：首块不做偏移（真实采集无帧末列），
    // 从第 2 块起用上一块原始末列前插对齐。
    if (shiftWL2) {
        std::vector<double> prev;
        if (!prevWL2Last.empty()) {
            prev = prevWL2Last;
        } else {
            prev.assign(wl2.begin(), wl2.begin() + sampDepth);
        }
        for (int c = nWlBlock - 1; c >= 1; --c) {
            std::memcpy(wl2.data() + static_cast<size_t>(c) * sampDepth,
                        wl2.data() + static_cast<size_t>(c - 1) * sampDepth,
                        static_cast<size_t>(sampDepth) * sizeof(double));
        }
        std::memcpy(wl2.data(), prev.data(),
                    static_cast<size_t>(sampDepth) * sizeof(double));
    }
}

std::vector<float> preprocessBlock(const std::vector<double>& in,
                                   int Nt, int nCol,
                                   const PreprocessParams& p) {
    std::vector<double> tmp = in;

    if (p.dbrRemove) {
        const int zeroRows = p.maskLength + p.dbrmaskExtra;
        const int zr = std::min(zeroRows, Nt);
        for (int c = 0; c < nCol; ++c) {
            std::memset(tmp.data() + static_cast<size_t>(c) * Nt, 0,
                        static_cast<size_t>(zr) * sizeof(double));
        }
    }

    if (p.signalImpair) {
        const float im = static_cast<float>(p.imValue);
        for (double& v : tmp) {
            if (v > im) v = im;
            else if (v < -im) v = -im;
        }
    }

    const int outRows = p.delayCut ? (Nt - p.systemDelay + 1) : Nt;
    std::vector<float> out(static_cast<size_t>(outRows) * nCol);
    const int srcRow0 = p.delayCut ? (p.systemDelay - 1) : 0;
    for (int c = 0; c < nCol; ++c) {
        const double* src = tmp.data() + static_cast<size_t>(c) * Nt + srcRow0;
        float* dst = out.data() + static_cast<size_t>(c) * outRows;
        for (int r = 0; r < outRows; ++r) {
            dst[r] = static_cast<float>(src[r]);
        }
    }
    return out;
}

void dasReconAppend(const std::vector<float>& bscan, int Nt, int nd,
                    const ReconParams& rp, double blockStartDeg, double blockSpanDeg,
                    const std::vector<float>& xv, const std::vector<float>& yv,
                    IncrementalState& st) {
    const int nx = static_cast<int>(xv.size());
    const int ny = static_cast<int>(yv.size());
    if (nx < 2 || ny < 2 || nd < 2) return;
    if (st.acc.empty()) {
        st.acc.assign(static_cast<size_t>(nx) * ny, 0.0f);
        st.accW.assign(static_cast<size_t>(nx) * ny, 0.0f);
        st.nBlock = 0;
    }

    const float fs = static_cast<float>(rp.fs);
    const float c = static_cast<float>(rp.c);
    const float pre = fs / c;
    const float R = static_cast<float>(rp.R);
    const float R2 = R * R;
    const float minDist = static_cast<float>(
        rp.minDistance > 0.0 ? rp.minDistance : rp.gridSize);
    const float pw = static_cast<float>(rp.distanceWeightExponent + 1.0);  // 2

    // B1 成对切换（前提 R2）：反演模式下逐 A-line 计算时间导数。
    // 同源约束（前提 G2）：p 与 p′ 都从**同一个 bscan 数组**取，不在别处另存一份；
    //   故 b = 2p − 2t·p′ 与恒等式 −2r̃²∂(p/r̃)/∂r̃ 一致。
    // Das 模式不分配、不计算，全关路径逐位不变（守卫 G4）。
    const bool ubp = (rp.inversion == ringrecon_inv::InversionMode::Ubp);
    std::vector<float> dscan;
    if (ubp) {
        dscan.resize(bscan.size());
        for (int j = 0; j < nd; ++j) {
            ringrecon_inv::derivativeCentral(
                bscan.data() + static_cast<std::size_t>(j) * Nt, Nt, fs,
                dscan.data() + static_cast<std::size_t>(j) * Nt);
        }
    }

    // 分层声速（与 MATLAB das_recon_circular_gpu_v2 一致）：
    //   tf = d/c_outer + sum_i Li*(1/c_i - 1/c_{i+1})；无边界时退化为单声速
    const int nBound = static_cast<int>(rp.soundSpeedRadii.size());
    const bool useLayers = nBound > 0 &&
        static_cast<int>(rp.soundSpeeds.size()) >= nBound + 1;
    float preOuter = pre;
    std::vector<float> preCoeff(nBound, 0.0f);
    std::vector<float> rb2s(nBound, 0.0f);
    std::vector<int> sIn(nBound, 0);
    if (useLayers) {
        preOuter = static_cast<float>(fs / rp.soundSpeeds[static_cast<size_t>(nBound)]);
        for (int i = 0; i < nBound; ++i) {
            const double rb2 = rp.soundSpeedRadii[static_cast<size_t>(i)] *
                               rp.soundSpeedRadii[static_cast<size_t>(i)];
            preCoeff[static_cast<size_t>(i)] = static_cast<float>(fs *
                (1.0 / rp.soundSpeeds[static_cast<size_t>(i)] -
                 1.0 / rp.soundSpeeds[static_cast<size_t>(i + 1)]));
            rb2s[static_cast<size_t>(i)] = static_cast<float>(rb2);
            sIn[static_cast<size_t>(i)] = (R2 <= rb2) ? 1 : 0;
        }
    }

    const double deg2rad = kPi / 180.0;
    const double stepDeg = blockSpanDeg / static_cast<double>(std::max(nd - 1, 1));
    const float arc = static_cast<float>(blockSpanDeg / std::max(nd - 1, 1) * deg2rad);
    const float wscale = -arc;

    const bool linear = (rp.interpolation == "linear");
    const bool maskOob = rp.maskOutOfRange;
    const bool useFovMask = rp.fovDeg < 359.9999;
    const float fovRad = static_cast<float>(rp.fovDeg * deg2rad);
    const float fovTh0Rad = static_cast<float>(rp.fovTheta0Deg * deg2rad);
    const float twoPi = static_cast<float>(2.0 * kPi);

    std::vector<float> detx(nd), dety(nd);
    for (int j = 0; j < nd; ++j) {
        const float th = static_cast<float>((blockStartDeg + j * stepDeg) * deg2rad);
        detx[j] = R * std::cos(th);
        dety[j] = R * std::sin(th);
    }

    for (int ix = 0; ix < nx; ++ix) {
        const float x = xv[ix];
        for (int iy = 0; iy < ny; ++iy) {
            const float y = yv[iy];
            const size_t idx = static_cast<size_t>(ix) * ny + iy;
            const float r2 = x * x + y * y;
            const float r2mR2 = r2 - R2;

            if (useFovMask) {
                float phi = std::atan2(y, x);
                float dang = std::fmod(phi - fovTh0Rad, twoPi);
                if (dang < 0.0f) dang += twoPi;
                if (dang > fovRad) continue;  // 掩膜外像素不参与累加
            }

            float acc = 0.0f;
            float accw = 0.0f;
            for (int j = 0; j < nd; ++j) {
                const float proj = x * detx[j] + y * dety[j];
                const float dotp = proj - R2;
                const float dist2 = r2mR2 - 2.0f * dotp;
                const float dist = std::sqrt(std::max(dist2, 0.0f));
                const float dsafe = std::max(dist, minDist);
                float tf = dist * (useLayers ? preOuter : pre);
                for (int bi = 0; bi < nBound; ++bi) {
                    const float rb2 = rb2s[static_cast<size_t>(bi)];
                    const bool bothIn = (sIn[static_cast<size_t>(bi)] != 0) && (r2 <= rb2);
                    const float discr4 = dotp * dotp - dist2 * (R2 - rb2);
                    const bool cross = discr4 > 0.0f;
                    const float sd = std::sqrt(std::max(discr4, 0.0f));
                    const float dist2d = std::max(dist2, 1e-12f);
                    const float u1 = (-dotp - sd) / dist2d;
                    const float u2 = (-dotp + sd) / dist2d;
                    float lc = (std::min(std::max(u2, 0.0f), 1.0f) -
                                std::max(std::min(u1, 1.0f), 0.0f)) * dist;
                    lc = std::max(lc, 0.0f);
                    float li = dist * (bothIn ? 1.0f : 0.0f);
                    if (cross && !bothIn) li += lc;
                    tf += li * preCoeff[static_cast<size_t>(bi)];
                }

                float vv = 0.0f;
                if (linear) {
                    const float i0f = std::floor(tf);
                    const float frac = tf - i0f;
                    const int i0 = static_cast<int>(i0f) + 1;  // 1-based
                    const bool valid = (i0 >= 1) && (i0 <= Nt - 1);
                    int i0c = i0;
                    i0c = std::max(i0c, 1);
                    i0c = std::min(i0c, Nt - 1);
                    const float v0 = bscan[static_cast<size_t>(i0c - 1) +
                                           static_cast<size_t>(j) * Nt];
                    const float v1 = bscan[static_cast<size_t>(i0c) +
                                           static_cast<size_t>(j) * Nt];
                    vv = v0 + frac * (v1 - v0);
                    if (maskOob && !valid) vv = 0.0f;
                } else {
                    const int i0n = static_cast<int>(std::floor(tf + 0.5f)) + 1;
                    const bool valid = (i0n >= 1) && (i0n <= Nt);
                    int i0c = i0n;
                    i0c = std::max(i0c, 1);
                    i0c = std::min(i0c, Nt);
                    vv = bscan[static_cast<size_t>(i0c - 1) +
                               static_cast<size_t>(j) * Nt];
                    if (maskOob && !valid) vv = 0.0f;
                }

                float dsafeP;
                if (pw == 2.0f) {
                    dsafeP = dsafe * dsafe;
                } else if (pw == 1.0f) {
                    dsafeP = dsafe;
                } else {
                    dsafeP = std::pow(dsafe, pw);
                }
                // 成对切换：信号项与权重由同一个 InversionMode 决定（R2）。
                // Das 时 sv == vv、weight 走 dasWeight，与改动前逐字同形。
                float sv = vv;
                if (ubp) {
                    float dvv = 0.0f;
                    if (linear) {
                        const float i0f = std::floor(tf);
                        const float frac = tf - i0f;
                        const int i0 = static_cast<int>(i0f) + 1;
                        const bool valid = (i0 >= 1) && (i0 <= Nt - 1);
                        int i0c = std::max(std::min(i0, Nt - 1), 1);
                        const float d0 = dscan[static_cast<std::size_t>(i0c - 1) +
                                               static_cast<std::size_t>(j) * Nt];
                        const float d1 = dscan[static_cast<std::size_t>(i0c) +
                                               static_cast<std::size_t>(j) * Nt];
                        dvv = d0 + frac * (d1 - d0);
                        if (maskOob && !valid) dvv = 0.0f;
                    } else {
                        const int i0n = static_cast<int>(std::floor(tf + 0.5f)) + 1;
                        const bool valid = (i0n >= 1) && (i0n <= Nt);
                        const int i0c = std::max(std::min(i0n, Nt), 1);
                        dvv = dscan[static_cast<std::size_t>(i0c - 1) +
                                    static_cast<std::size_t>(j) * Nt];
                        if (maskOob && !valid) dvv = 0.0f;
                    }
                    // t 用秒（前提 G2：误用采样点会差 fs 倍）。
                    // 【开放项 D5】tf/fs 是相对存储数组起点的时间；是否需要
                    //   systemDelay / delayCut 校准由分发任务 D5 定，骨架不擅自补偿。
                    const float tSec = tf / fs;
                    sv = ringrecon_inv::signalValue(rp.inversion, vv, dvv, tSec);
                }
                const float w = ringrecon_inv::weight(rp.inversion, wscale, dotp,
                                                      R, dsafe, dsafeP);
                acc += w * sv;
                accw += std::fabs(w);
            }
            st.acc[idx] += acc;
            st.accW[idx] += accw;
        }
    }
    ++st.nBlock;
}

std::vector<float> normalizedImage(const IncrementalState& st) {
    std::vector<float> out(st.acc.size());
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = st.acc[i] / std::max(st.accW[i], kTiny);
    }
    return out;
}

}  // namespace ringrecon