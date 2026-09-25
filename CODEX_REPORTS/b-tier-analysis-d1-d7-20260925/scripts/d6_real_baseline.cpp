// d6_real_baseline.cpp — D6 现有 main 行为在两种成像模式下的质量指标基线
//
// 数据：testdata/01（2026-08-16 历史实测采集，FileSaver float16 定长记录）。
// 度量：pa_metrics.h（D4 口径，独立实现）。
// 重建：生产 CPU 参考 ringrecon::dasReconAppend 作被测对象，inversion=Das（现有行为）。
// 不做逐图归一化、不做显示拉伸（R1）。
//
// 编译：
//   g++ -std=c++17 -O2 -o d6_real_baseline d6_real_baseline.cpp ring_recon.cpp
#include "ring_recon.h"
#include "pa_metrics.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

static const double FS = 250e6;
static const double C_SOUND = 1490.0;
static const double R_RING = 6.57e-3;
static const double FOV = 36e-3;
static const int    SAMP_DEPTH = 4000;     // 重建侧
static const int    REC_LEN = 10000;       // 实测记录长度（D1 判定）
static const int    N_CH = 8;
static const double SECTOR_START_DEG = 180.0;
static const double SECTOR_WIDTH_DEG = 45.0;
static const double STEP_DEG = 0.09;       // sectorWidth/500（项目 MainWindow.cpp:5369）
static const int    SYS_DELAY = 358;       // wl1 延时截断起点（1-based）
static const int    MASK_LEN = 300;        // DBR 强信号扣除行数

static float f16(uint16_t u) {
    const int s = (u >> 15) & 1, e = (u >> 10) & 31, m = u & 1023;
    if (e == 0) return (float)((s ? -1 : 1) * std::ldexp((double)m / 1024.0, -14));
    if (e == 31) return (float)((s ? -1 : 1) * INFINITY);
    return (float)((s ? -1 : 1) * std::ldexp(1.0 + m / 1024.0, e - 15));
}

static const char* CHAN[N_CH] = {
    "Card1_ChA", "Card1_ChB", "Card2_ChA", "Card2_ChB",
    "Card3_ChA", "Card3_ChB", "Card4_ChA", "Card4_ChB",
};

struct RunResult {
    std::vector<float> img;
    int nx = 0, ny = 0;
    double x0 = 0, y0 = 0, dx = 0, dy = 0;
};

static RunResult toRowMajor(const ringrecon::IncrementalState& st,
                            const std::vector<float>& xv, const std::vector<float>& yv) {
    RunResult r;
    r.nx = (int)xv.size(); r.ny = (int)yv.size();
    r.x0 = xv.front(); r.y0 = yv.front();
    r.dx = (r.nx > 1) ? xv[1] - xv[0] : 0.0;
    r.dy = (r.ny > 1) ? yv[1] - yv[0] : 0.0;
    const std::vector<float> norm = ringrecon::normalizedImage(st);
    r.img.assign((size_t)r.nx * r.ny, 0.0f);
    for (int ix = 0; ix < r.nx; ++ix)
        for (int iy = 0; iy < r.ny; ++iy)
            r.img[(size_t)iy * r.nx + ix] = norm[(size_t)ix * r.ny + iy];
    return r;
}

int main(int argc, char** argv) {
    const std::string root = (argc > 1) ? argv[1] : "testdata/01";
    const double gridSize = (argc > 2) ? std::atof(argv[2]) : 0.1e-3;

    std::printf("==================================================================\n");
    std::printf("D6  现有 main 行为在两种成像模式下的质量指标基线\n");
    std::printf("==================================================================\n\n");
    std::printf("数据：%s（2026-08-16 历史实测采集）\n", root.c_str());
    std::printf("记录格式：float16 定长记录，每记录 %d 采样点 = %.1f us @ %.0f MSa/s（D1 判定）\n",
                REC_LEN, REC_LEN / FS * 1e6, FS / 1e6);
    std::printf("重建：生产 CPU 参考 ringrecon::dasReconAppend，inversion=Das（现有行为）\n");
    std::printf("度量：pa_metrics.h（D4 口径）；输入为原始线性值，未做任何归一化/拉伸\n\n");

    std::printf("提取口径（与项目预处理约定一致）：\n");
    std::printf("  每根 A-line 取记录的第 [%d, %d) 采样点（sysDelay=%d 起，sampDepth=%d）\n",
                SYS_DELAY - 1, SYS_DELAY - 1 + SAMP_DEPTH, SYS_DELAY, SAMP_DEPTH);
    std::printf("  前 %d 点置零（maskLength，DBR 强信号扣除）\n", MASK_LEN);
    std::printf("  波长：全局触发 g 奇偶交替，triggerWlOdd=1 => g 偶 = wl1(532nm)\n");
    std::printf("  角度：sectorStart=%.1f deg，扇区 %.1f deg，角步长 %.2f deg\n\n",
                SECTOR_START_DEG, SECTOR_WIDTH_DEG, STEP_DEG);

    // ---- 读数据：每通道每波长一列 A-line ----
    // 每通道 8 个文件 x 100 记录 = 800 根，奇偶分波长 => 每波长 400 根
    struct ChanData {
        std::vector<std::vector<float>> wl[2];
        double noiseRms = 0, peak = 0;
        int nRec = 0;
    };
    std::vector<ChanData> cd(N_CH);
    long long totalRec = 0;

    for (int c = 0; c < N_CH; ++c) {
        double preSum = 0; long preN = 0; double pk = 0;
        for (int f = 0; f < 8; ++f) {
            char name[256];
            std::snprintf(name, sizeof name, "%s/%s_test_%03d.dat", root.c_str(), CHAN[c], f);
            std::ifstream in(name, std::ios::binary);
            if (!in) { std::printf("  [warn] 打不开 %s\n", name); continue; }
            std::vector<uint16_t> raw((size_t)REC_LEN * 100);
            in.read(reinterpret_cast<char*>(raw.data()), (std::streamsize)(raw.size() * 2));
            const size_t got = (size_t)in.gcount() / 2;
            const int recs = (int)(got / REC_LEN);
            for (int r = 0; r < recs; ++r) {
                const int g = f * recs + r;
                const int wl = (g % 2 == 0) ? 0 : 1;
                std::vector<float> line(SAMP_DEPTH);
                for (int i = 0; i < SAMP_DEPTH; ++i) {
                    float v = f16(raw[(size_t)r * REC_LEN + (SYS_DELAY - 1) + i]);
                    if (i < MASK_LEN) v = 0.0f;
                    line[i] = v;
                }
                for (int i = 0; i < 400; ++i) {          // 触发前静默段：噪声底
                    const float v = f16(raw[(size_t)r * REC_LEN + i]);
                    preSum += (double)v * v; ++preN;
                    pk = std::max(pk, (double)std::fabs(v));
                }
                for (int i = 0; i < SAMP_DEPTH; ++i)
                    pk = std::max(pk, (double)std::fabs(f16(raw[(size_t)r * REC_LEN + i])));
                cd[c].wl[wl].push_back(std::move(line));
                ++cd[c].nRec; ++totalRec;
            }
        }
        cd[c].noiseRms = preN ? std::sqrt(preSum / preN) : 0;
        cd[c].peak = pk;
    }
    std::printf("读入 %lld 条 A-line 记录。\n", totalRec);
    std::printf("| 通道 | 记录数 | wl1 根数 | wl2 根数 | 噪声底RMS | 记录峰值 | max|v|/噪声RMS | 信号 |\n");
    int sigCh = 0;
    for (int c = 0; c < N_CH; ++c) {
        const double ratio = cd[c].noiseRms > 0 ? cd[c].peak / cd[c].noiseRms : 0;
        const bool sig = ratio > 4.0;
        if (sig) ++sigCh;
        std::printf("| %s | %d | %zu | %zu | %.3f | %.1f | %.3f | %s |\n",
                    CHAN[c], cd[c].nRec, cd[c].wl[0].size(), cd[c].wl[1].size(),
                    cd[c].noiseRms, cd[c].peak, ratio, sig ? "有" : "无");
    }
    std::printf("\n【数据充分性】%d / %d 个物理通道含真实光声信号。\n", sigCh, N_CH);
    std::printf("其余通道为纯均匀量化噪声（max/sigma≈1.73=√3，D1 已证），\n");
    std::printf("它们进入重建只会贡献量化噪声，不贡献结构。下表结果据此解读。\n\n");

    // ---- 两种模式重建 ----
    const int wlSel = (argc > 3) ? std::atoi(argv[3]) : 0;   // 0=wl1, 1=wl2
    std::vector<float> xv, yv;
    ringrecon::makeGrid(FOV, gridSize, xv, yv);
    const int nx = (int)xv.size(), ny = (int)yv.size();

    const char* modeName[2] = {"spliceMode=0（全局反投影）", "spliceMode=1（逐通道扇区拼接）"};
    RunResult rr[2];
    for (int mode = 0; mode < 2; ++mode) {
        ringrecon::ReconParams rp;
        rp.fs = FS; rp.c = C_SOUND; rp.R = R_RING;
        rp.fov = FOV; rp.gridSize = gridSize;
        rp.distanceWeightExponent = 1.0; rp.minDistance = 0.0;
        rp.maskOutOfRange = true; rp.interpolation = "linear";
        rp.inversion = ringrecon_inv::InversionMode::Das;
        ringrecon::IncrementalState st;
        for (int c = 0; c < N_CH; ++c) {
            const auto& lines = cd[c].wl[wlSel];
            const int nd = (int)lines.size();
            if (nd < 2) continue;
            std::vector<float> bscan((size_t)nd * SAMP_DEPTH);
            std::vector<float> ang(nd), rad(nd);
            const double start = SECTOR_START_DEG + c * SECTOR_WIDTH_DEG;
            for (int j = 0; j < nd; ++j) {
                std::memcpy(bscan.data() + (size_t)j * SAMP_DEPTH,
                            lines[j].data(), SAMP_DEPTH * sizeof(float));
                ang[j] = (float)(start + j * STEP_DEG);
                rad[j] = (float)R_RING;
            }
            if (mode == 0) {
                rp.fovDeg = 360.0; rp.fovTheta0Deg = 0.0;
                // 全局：把每通道的 A-line 都累加到同一状态，角度由 blockStart 推导
                ringrecon::IncrementalState tmp;
                // 用逐通道调用但不加扇区门控，等价于全局累加
                rp.fovDeg = 360.0;
                ringrecon::dasReconAppend(bscan, SAMP_DEPTH, nd, rp,
                                          start, STEP_DEG * (nd - 1), xv, yv, st);
            } else {
                rp.fovDeg = SECTOR_WIDTH_DEG;
                rp.fovTheta0Deg = start;
                ringrecon::dasReconAppend(bscan, SAMP_DEPTH, nd, rp,
                                          start, STEP_DEG * (nd - 1), xv, yv, st);
            }
        }
        rr[mode] = toRowMajor(st, xv, yv);
        (void)nx; (void)ny;
    }

    // ---- 度量 ----
    pametric::Peak pkAll = pametric::findPeak(pametric::Image{
        rr[0].img.data(), rr[0].nx, rr[0].ny, rr[0].x0, rr[0].y0, rr[0].dx, rr[0].dy });
    std::printf("峰值位置（spliceMode=0）：(%.3f, %.3f) mm, 值=%.4g\n\n",
                pkAll.x * 1e3, pkAll.y * 1e3, pkAll.value);

    pametric::Roi tgt{ pkAll.x, pkAll.y, 1.5e-3, 1.5e-3 };
    pametric::Roi bg{ 12.0e-3, 12.0e-3, 3.0e-3, 3.0e-3 };
    std::printf("ROI（显式、参数化，不硬编码到具体体模）：\n");
    std::printf("  目标 ROI %s\n", tgt.describe().c_str());
    std::printf("  背景 ROI %s\n\n", bg.describe().c_str());

    std::printf("| 指标 | spliceMode=0 | spliceMode=1 | 差值 | 单位 |\n");
    struct Row { const char* name; const char* unit; double v[2]; };
    std::vector<Row> rows;
    for (int mode = 0; mode < 2; ++mode) {
        const pametric::Image im{ rr[mode].img.data(), rr[mode].nx, rr[mode].ny,
                                  rr[mode].x0, rr[mode].y0, rr[mode].dx, rr[mode].dy };
        const pametric::Peak p = pametric::findPeak(im);
        const auto res = pametric::resolution(im, p.x, p.y, 0.0, 0.0);
        const double cr = pametric::contrastDb(im, bg);
        const auto gr = pametric::gcnr(im, tgt, bg, 64);
        const auto bn = pametric::backgroundNoise(im, bg);
        const double cn = pametric::cnr(im, tgt, bg);
        if (mode == 0) {
            rows.push_back({"轴向 -6 dB 分辨率", "mm", {0, 0}});
            rows.push_back({"切向 -6 dB 分辨率", "mm", {0, 0}});
            rows.push_back({"CR", "dB", {0, 0}});
            rows.push_back({"gCNR", "-", {0, 0}});
            rows.push_back({"背景噪声 std", "（原始单位）", {0, 0}});
            rows.push_back({"背景噪声 std/峰值", "-", {0, 0}});
            rows.push_back({"CNR", "-", {0, 0}});
            rows.push_back({"峰值", "（原始单位）", {0, 0}});
        }
        rows[0].v[mode] = res.axialMm;
        rows[1].v[mode] = res.tangentialMm;
        rows[2].v[mode] = cr;
        rows[3].v[mode] = gr.gcnr;
        rows[4].v[mode] = bn.sd;
        rows[5].v[mode] = bn.relativeToPeak;
        rows[6].v[mode] = cn;
        rows[7].v[mode] = p.value;
    }
    for (auto& r : rows)
        std::printf("| %s | %.6g | %.6g | %+.6g | %s |\n",
                    r.name, r.v[0], r.v[1], r.v[1] - r.v[0], r.unit);

    std::printf("\n【两模式差值的含义】原规格 §3.1：两模式差值 = 有限视角贡献的直接量化。\n");
    std::printf("本数据只有 %d/%d 通道含信号，故该差值主要反映【单通道扇区】相对\n", sigCh, N_CH);
    std::printf("【全局】的视角限制，而不是 8 通道满孔径的有限视角效应。如实标注。\n\n");

    // ---- 双波长比值守卫 ----
    std::printf("=== 双波长比值守卫（R1）===\n");
    std::printf("口径：同一重建算子分别作用于 wl1 / wl2，断言 I1/I2 在 ROI 内为常数。\n");
    std::printf("本数据两波长幅值比**未知**（无真值），故只断言「为常数」，不校验数值。\n");
    for (int mode = 0; mode < 2; ++mode) {
        RunResult a, b;
        for (int w = 0; w < 2; ++w) {
            ringrecon::ReconParams rp;
            rp.fs = FS; rp.c = C_SOUND; rp.R = R_RING;
            rp.fov = FOV; rp.gridSize = gridSize;
            rp.distanceWeightExponent = 1.0; rp.minDistance = 0.0;
            rp.maskOutOfRange = true; rp.interpolation = "linear";
            rp.inversion = ringrecon_inv::InversionMode::Das;
            ringrecon::IncrementalState st;
            for (int c = 0; c < N_CH; ++c) {
                const auto& lines = cd[c].wl[w];
                const int nd = (int)lines.size();
                if (nd < 2) continue;
                std::vector<float> bscan((size_t)nd * SAMP_DEPTH);
                for (int j = 0; j < nd; ++j)
                    std::memcpy(bscan.data() + (size_t)j * SAMP_DEPTH,
                                lines[j].data(), SAMP_DEPTH * sizeof(float));
                const double start = SECTOR_START_DEG + c * SECTOR_WIDTH_DEG;
                rp.fovDeg = (mode == 0) ? 360.0 : SECTOR_WIDTH_DEG;
                rp.fovTheta0Deg = (mode == 0) ? 0.0 : start;
                ringrecon::dasReconAppend(bscan, SAMP_DEPTH, nd, rp,
                                          start, STEP_DEG * (nd - 1), xv, yv, st);
            }
            (w == 0 ? a : b) = toRowMajor(st, xv, yv);
        }
        const auto g = pametric::dualWavelengthRatioGuard(
            pametric::Image{a.img.data(), a.nx, a.ny, a.x0, a.y0, a.dx, a.dy},
            pametric::Image{b.img.data(), b.nx, b.ny, b.x0, b.y0, b.dx, b.dy},
            pametric::Roi{0, 0, 18e-3, 18e-3}, 1.0, 0.25, 1e9, 1e-3);
        std::printf("  %s : %s\n    %s\n", modeName[mode], g.ok ? "PASS" : "FAIL", g.detail.c_str());
        std::printf("    （期望值传 1.0，relErr 容差 1e9 关闭该校验；只看 relStd）\n");
    }

    std::printf("\n=== 证据口径（四层分离）===\n");
    std::printf("source/code correctness             未声称（本任务不改代码）\n");
    std::printf("automated tests/build/selftest      脚本可复跑 = 是\n");
    std::printf("real hardware validation            如实标注：用的是 testdata/01（2026-08-16\n");
    std::printf("                                    历史实测采集，provenance 未正式确认）；\n");
    std::printf("                                    非合成数据，但也不是本轮实机验收数据。\n");
    std::printf("hardware root-cause attribution     未声称\n");
    std::printf("\n=== 未验证项 / 限制 ===\n");
    std::printf("* 本数据仅 %d/%d 个物理通道含真实信号，其余为纯量化噪声。\n", sigCh, N_CH);
    std::printf("* 两波长幅值比未知，比值守卫只验「常数性」，不验数值。\n");
    std::printf("* 记录长度 %d 与重建 sampDepth %d 不一致，需按 sysDelay 开窗；开窗选择已声明。\n",
                REC_LEN, SAMP_DEPTH);
    std::printf("* 未走完整生产预处理（trigdejit/phaseDecon/gaussfil 等均为关），只做 delayCut + maskLength。\n");
    std::printf("* gridSize 用 %.3f mm（项目默认 0.01 mm），是计算量取舍；分辨率受网格量化下限约束。\n",
                gridSize * 1e3);
    std::printf("* 不声称 B1 更优或更差（那是 D7）；本表只建立 A/B 基线。\n");
    return 0;
}
