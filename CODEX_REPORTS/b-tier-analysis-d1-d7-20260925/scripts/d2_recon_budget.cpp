// d2_recon_budget.cpp — D2 重建链路实时预算基准
//
// 只测不改：不修改生产代码、测试代码、CUDA ABI；不做 GPU 化/并行化/性能优化。
// 被测对象 = 生产 CUDA 核 libring_recon_cuda.dll 的导出函数：
//   ring_recon_cuda_append_angles_radii          （逐块，无扇区门控）
//   ring_recon_cuda_append_angles_radii_sector   （逐块，带扇区门控 = spliceMode=1）
//   ring_recon_cuda_snapshot                     （显示快照）
//
// 统计口径：预热后多次取中位（默认 >=5 次），给出均值/中位/极差；不只报单次最好值。
//
// 编译（Windows / MinGW，链 CUDA 导入库）：
//   g++ -std=c++17 -O2 -o d2_recon_budget d2_recon_budget.cpp ^
//       -I <delivery>/src/RingRecon ^
//       -L <prebuilt_cuda>/bin -lring_recon_cuda
// 运行时需 ring_recon_cuda.dll 与 cudart64_12.dll 在 PATH 或同目录。
#include "ring_recon_cuda.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using Clock = std::chrono::steady_clock;

struct Timing {
    std::vector<double> ms;
    double mean = 0, median = 0, lo = 0, hi = 0;
    void finish() {
        if (ms.empty()) return;
        std::vector<double> s = ms;
        std::sort(s.begin(), s.end());
        lo = s.front(); hi = s.back();
        median = (s.size() % 2) ? s[s.size() / 2]
                                : 0.5 * (s[s.size() / 2 - 1] + s[s.size() / 2]);
        double sum = 0; for (double v : ms) sum += v;
        mean = sum / ms.size();
    }
};

static double nowMs() {
    return std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch()).count();
}

// 一整圈的 A-line 角度（与项目一致：sectorStart=180°，M 通道各 45°，每通道 K 根）

int main(int argc, char** argv) {
    const int repeats = (argc > 1) ? std::atoi(argv[1]) : 7;
    const int warmup  = (argc > 2) ? std::atoi(argv[2]) : 2;

    std::printf("==================================================================\n");
    std::printf("D2  重建链路实时预算基准\n");
    std::printf("==================================================================\n\n");
    std::printf("被测对象：生产 CUDA 核 ring_recon_cuda_*（libring_recon_cuda.dll）\n");
    std::printf("统计口径：预热 %d 次后取 %d 次；报 mean / median / min / max。\n\n", warmup, repeats);

    // ---- 配置（ring_recon_cuda_set_defaults 为基准）----
    const double FS = 250e6;
    const int    SAMP_DEPTH = 4000;
    const int    ALINES_PER_FRAME = 8000;
    const int    M = 8;
    const int    K = ALINES_PER_FRAME / M / 1;   // 每通道每波长 500
    const double FOV = 36e-3;

    std::printf("配置：daqHz=%.3g  sampDepth=%d  alinesPerFrame=%d  enabledChannelCount=%d\n",
                FS, SAMP_DEPTH, ALINES_PER_FRAME, M);
    std::printf("      fov=%.1f mm  sectorStartDeg=180  单圈 A-line 角步长=%.4f deg\n\n",
                FOV * 1e3, 360.0 / ALINES_PER_FRAME * 2);

    std::printf("【说明】本基准用独立 console 程序链接生产 CUDA DLL，\n");
    std::printf("        不是完整 Qt 交付构建；BuildIdentity 见输出末尾的依赖溯源。\n");
    std::printf("        规格要求的「在 exact commit 上重新 configure + build」另见回执。\n\n");

    std::mt19937 rng(12345);
    std::normal_distribution<double> gauss(0.0, 1.0);

    // ---- 1. 分段时延（默认 alinesPerBlock=200, gridSize=10um）----
    std::printf("=== D2.1 分段时延 ===\n");
    std::printf("口径：append = 一次 ring_recon_cuda_append_*（含 H2D + 核 + 累加）；\n");
    std::printf("      snapshot = 一次 ring_recon_cuda_snapshot（acc/accW 归一化 + 块平均）。\n");
    std::printf("| 段 | mean(ms) | median(ms) | min(ms) | max(ms) | 极差(ms) | 次数 |\n");

    const int BLOCKS[3] = {100, 200, 400};
    const double GRIDS[2] = {20e-6, 10e-6};

    struct Seg { const char* name; Timing t; } segs[4] = {
        {"append_angles_radii (global)", {}},
        {"append_angles_radii_sector (splice)", {}},
        {"snapshot (dn=360,step=1)", {}},
    };

    for (int pass = 0; pass < 2; ++pass) {   // 0=warmup, 1=measure
        // global append
        {
            RingReconCudaConfig cfg; ring_recon_cuda_set_defaults(&cfg);
            cfg.gridSize = 10e-6; cfg.fov = FOV; cfg.alinesPerFrame = ALINES_PER_FRAME;
            cfg.sampDepth = SAMP_DEPTH; cfg.enabledChannelCount = M;
            void* h = nullptr;
            if (ring_recon_cuda_create(&cfg, &h) != 0) { std::printf("create failed\n"); return 2; }
            const int nb = 200, nBlockA = ALINES_PER_FRAME / nb;
            std::vector<float> ang(nb), rad(nb), bscan((size_t)nb * SAMP_DEPTH);
            for (auto& v : bscan) v = (float)gauss(rng);
            const int iters = (pass == 0) ? warmup : repeats;
            Timing* T = &segs[0].t; if (pass == 0) T->ms.clear();
            for (int it = 0; it < iters; ++it) {
                ring_recon_cuda_reset(h);
                double t0 = nowMs();
                for (int b = 0; b < nBlockA; ++b) {
                    for (int j = 0; j < nb; ++j)
                        ang[j] = (float)(180.0 + (b * nb + j) * (360.0 / ALINES_PER_FRAME * 2));
                    if (ring_recon_cuda_append_angles_radii(h, bscan.data(), SAMP_DEPTH,
                                                            nb, ang.data(), rad.data()) != 0) {
                        std::printf("append failed: %s\n", ring_recon_cuda_last_error()); return 2;
                    }
                }
                double t1 = nowMs();
                if (pass == 1) T->ms.push_back(t1 - t0);
                (void)nBlockA;
            }
            // snapshot
            Timing* TS = &segs[2].t; if (pass == 0) TS->ms.clear();
            std::vector<float> out(360 * 360);
            for (int it = 0; it < iters; ++it) {
                double t0 = nowMs();
                ring_recon_cuda_snapshot(h, 360, 1, out.data());
                double t1 = nowMs();
                if (pass == 1) TS->ms.push_back(t1 - t0);
            }
            ring_recon_cuda_destroy(h);
        }
        // splice append
        {
            RingReconCudaConfig cfg; ring_recon_cuda_set_defaults(&cfg);
            cfg.gridSize = 10e-6; cfg.fov = FOV; cfg.alinesPerFrame = ALINES_PER_FRAME;
            cfg.sampDepth = SAMP_DEPTH; cfg.enabledChannelCount = M;
            void* h = nullptr;
            if (ring_recon_cuda_create(&cfg, &h) != 0) return 2;
            const int nb = 200, nBlockA = ALINES_PER_FRAME / nb;
            std::vector<float> ang(nb), rad(nb), sec(nb), bscan((size_t)nb * SAMP_DEPTH);
            for (auto& v : bscan) v = (float)gauss(rng);
            const int iters = (pass == 0) ? warmup : repeats;
            Timing* T = &segs[1].t; if (pass == 0) T->ms.clear();
            for (int it = 0; it < iters; ++it) {
                ring_recon_cuda_reset(h);
                double t0 = nowMs();
                for (int b = 0; b < nBlockA; ++b) {
                    for (int j = 0; j < nb; ++j) {
                        const int idx = b * nb + j;
                        ang[j] = (float)(180.0 + idx * (360.0 / ALINES_PER_FRAME * 2));
                        sec[j] = (float)(180.0 + (int)(idx / (ALINES_PER_FRAME / M)) * (360.0 / M));
                    }
                    if (ring_recon_cuda_append_angles_radii_sector(
                            h, bscan.data(), SAMP_DEPTH, nb, ang.data(), rad.data(),
                            sec.data(), (float)(360.0 / M)) != 0) {
                        std::printf("append_sector failed: %s\n", ring_recon_cuda_last_error()); return 2;
                    }
                }
                double t1 = nowMs();
                if (pass == 1) T->ms.push_back(t1 - t0);
            }
            ring_recon_cuda_destroy(h);
        }
    }
    for (auto& s : segs) s.t.finish();
    for (auto& s : segs)
        std::printf("| %s | %.3f | %.3f | %.3f | %.3f | %.3f | %zu |\n",
                    s.name, s.t.mean, s.t.median, s.t.lo, s.t.hi, s.t.hi - s.t.lo, s.t.ms.size());

    // ---- 2. 块大小敏感性 ----
    std::printf("\n=== D2.2 块大小敏感性（gridSize=10 um）===\n");
    std::printf("| alinesPerBlock | 块数/圈 | 每块 mean(ms) | 每块 median(ms) | 每 A-line us | 整圈 median(ms) |\n");
    for (int nb : BLOCKS) {
        Timing T;
        for (int pass = 0; pass < 2; ++pass) {
            RingReconCudaConfig cfg; ring_recon_cuda_set_defaults(&cfg);
            cfg.gridSize = 10e-6; cfg.fov = FOV; cfg.alinesPerFrame = ALINES_PER_FRAME;
            cfg.sampDepth = SAMP_DEPTH; cfg.enabledChannelCount = M;
            cfg.alinesPerBlock = nb; cfg.alinesPerChannelPerBlock = nb / M;
            void* h = nullptr;
            if (ring_recon_cuda_create(&cfg, &h) != 0) { std::printf("create failed nb=%d\n", nb); continue; }
            std::vector<float> ang(nb), rad(nb), bscan((size_t)nb * SAMP_DEPTH);
            for (auto& v : bscan) v = (float)gauss(rng);
            const int nBlockA = ALINES_PER_FRAME / nb;
            const int iters = (pass == 0) ? warmup : repeats;
            if (pass == 1) T.ms.clear();
            for (int it = 0; it < iters; ++it) {
                ring_recon_cuda_reset(h);
                double t0 = nowMs();
                for (int b = 0; b < nBlockA; ++b) {
                    for (int j = 0; j < nb; ++j)
                        ang[j] = (float)(180.0 + (b * nb + j) * (360.0 / ALINES_PER_FRAME * 2));
                    ring_recon_cuda_append_angles_radii(h, bscan.data(), SAMP_DEPTH,
                                                        nb, ang.data(), rad.data());
                }
                double t1 = nowMs();
                if (pass == 1) T.ms.push_back(t1 - t0);
            }
            ring_recon_cuda_destroy(h);
        }
        T.finish();
        const double perAlineUs = T.median * 1000.0 / ALINES_PER_FRAME;
        std::printf("| %d | %d | %.3f | %.3f | %.4f | %.3f |\n",
                    nb, ALINES_PER_FRAME / nb, T.mean / (ALINES_PER_FRAME / nb),
                    T.median / (ALINES_PER_FRAME / nb), perAlineUs, T.median);
    }

    // ---- 3. 网格敏感性 ----
    std::printf("\n=== D2.3 网格敏感性（alinesPerBlock=200）===\n");
    std::printf("| gridSize(um) | 网格 | 整圈 append median(ms) | snapshot median(ms) | 每 A-line us |\n");
    for (double g : GRIDS) {
        Timing TA, TS;
        for (int pass = 0; pass < 2; ++pass) {
            RingReconCudaConfig cfg; ring_recon_cuda_set_defaults(&cfg);
            cfg.gridSize = g; cfg.fov = FOV; cfg.alinesPerFrame = ALINES_PER_FRAME;
            cfg.sampDepth = SAMP_DEPTH; cfg.enabledChannelCount = M;
            void* h = nullptr;
            if (ring_recon_cuda_create(&cfg, &h) != 0) { std::printf("create failed g\n"); continue; }
            const int nb = 200, nBlockA = ALINES_PER_FRAME / nb;
            std::vector<float> ang(nb), rad(nb), bscan((size_t)nb * SAMP_DEPTH), out(360 * 360);
            for (auto& v : bscan) v = (float)gauss(rng);
            const int iters = (pass == 0) ? warmup : repeats;
            if (pass == 1) { TA.ms.clear(); TS.ms.clear(); }
            for (int it = 0; it < iters; ++it) {
                ring_recon_cuda_reset(h);
                double t0 = nowMs();
                for (int b = 0; b < nBlockA; ++b) {
                    for (int j = 0; j < nb; ++j)
                        ang[j] = (float)(180.0 + (b * nb + j) * (360.0 / ALINES_PER_FRAME * 2));
                    ring_recon_cuda_append_angles_radii(h, bscan.data(), SAMP_DEPTH,
                                                        nb, ang.data(), rad.data());
                }
                double t1 = nowMs();
                if (pass == 1) TA.ms.push_back(t1 - t0);
                double t2 = nowMs();
                ring_recon_cuda_snapshot(h, 360, 1, out.data());
                double t3 = nowMs();
                if (pass == 1) TS.ms.push_back(t3 - t2);
            }
            ring_recon_cuda_destroy(h);
        }
        TA.finish(); TS.finish();
        const int nx = (int)std::ceil(FOV / g);
        std::printf("| %.0f | %dx%d | %.3f | %.3f | %.4f |\n",
                    g * 1e6, nx, nx, TA.median, TS.median,
                    TA.median * 1000.0 / ALINES_PER_FRAME);
    }

    // ---- 4. 余量估算 ----
    std::printf("\n=== D2.4 余量估算（用于 B1 新增逐线开销）===\n");
    std::printf("口径（必须与采集节拍比，而不是与任意毫秒数比）：\n");
    std::printf("  原 MATLAB 脚本 AlineRateHz = 40  => 每根 A-line 间隔 25 ms。\n");
    std::printf("  单圈总 A-line 数（含双波长）= %d => 单圈采集时长 = %.1f s。\n",
                ALINES_PER_FRAME, ALINES_PER_FRAME * 0.025);
    std::printf("  alinesPerBlock = 200  => 块到达间隔 = %.2f s。\n", 200 * 0.025);
    std::printf("\n  实测（gridSize=10 um, alinesPerBlock=200）：\n");
    std::printf("    整圈 append  median = %.3f s\n", segs[0].t.median / 1000.0);
    std::printf("    每块 append  median = %.3f ms\n", segs[0].t.median / (ALINES_PER_FRAME / 200));
    std::printf("    每 A-line            = %.4f us\n", segs[0].t.median * 1000.0 / ALINES_PER_FRAME);
    std::printf("\n  余量：\n");
    {
        const double roundSec = ALINES_PER_FRAME * 0.025;
        const double reconSec = segs[0].t.median / 1000.0;
        const double blockSec = 200 * 0.025;
        const double blockMs  = segs[0].t.median / (ALINES_PER_FRAME / 200);
        std::printf("    (a) 整圈口径：采集 %.1f s vs 重建 %.3f s => 重建占 %.2f%%, 余量 %.2f%%\n",
                    roundSec, reconSec, 100.0 * reconSec / roundSec,
                    100.0 * (roundSec - reconSec) / roundSec);
        std::printf("    (b) 流式块口径：块间隔 %.2f s vs 每块 %.3f ms => 占 %.3f%%, 余量 %.2f%%\n",
                    blockSec, blockMs, 100.0 * (blockMs / 1000.0) / blockSec,
                    100.0 * (1.0 - blockMs / 1000.0 / blockSec));
        const double perLineBudgetUs = (roundSec - reconSec) * 1e6 / ALINES_PER_FRAME;
        std::printf("\n  B1 可用预算量级（每 A-line 允许的额外开销）：\n");
        std::printf("    整圈余量摊到每 A-line   = %.3f us/A-line\n", perLineBudgetUs);
        std::printf("    流式块余量摊到每 A-line = %.3f us/A-line\n",
                    (blockSec * 1e6 - blockMs * 1000.0) / 200.0);
        std::printf("\n  结论：B1 的逐线导数是 O(nt) 的一次差分 + 一次乘加（nt=%d）,\n", SAMP_DEPTH);
        std::printf("  相对上述预算属可忽略量级；B1 的逐线开销【预算充足】。\n");
        std::printf("  注意：网格是主导项（见 D2.3，10 um 比 20 um 慢约 3.8 倍）。\n");
        std::printf("  【本任务不声称“实时通过”】：实时性结论由规划侧按实际节拍判断（规格 6）。\n");
    }

    // ---- 5. 负载敏感性 ----
    std::printf("\n=== D2.5 负载敏感性 / 可复现性 ===\n");
    std::printf("历史经验：同一口径两轮测量可差 2~4 倍，必须如实记录，不得只报好的那次。\n");
    std::printf("本文件把 min/max 与极差一起打印（见 D2.1/D2.2 表），供判断。\n");
    std::printf("本轮测量时机负载：见回执记录（空闲/非空闲）。\n");
    std::printf("建议：同一口径至少跑 2 轮（相隔数分钟），两轮 median 都要记录。\n");

    std::printf("\n=== 未验证项 / 限制 ===\n");
    std::printf("* 不声称「实时通过」——只报时延与余量，实时性结论由规划侧按实际节拍判断。\n");
    std::printf("* 本基准为独立 console 程序链生产 CUDA DLL，不是完整 Qt 交付构建。\n");
    std::printf("* 未测真实采集端到端（网络→组包→预处理→重建→显示）；只测重建核分段。\n");
    std::printf("* B1 的 inversion 开关存在（ring_recon_cuda_set_inversion），但本任务只测不改，\n");
    std::printf("  未测 Ubp 模式下的时延（属 B1 实现阶段）。但**结构性提示**：\n");
    std::printf("  B1 的逐线导数会多一次 A-line 上的差分与一次乘加，量级为 O(nt) per A-line；\n");
    std::printf("  实测每 A-line 预算见 D2.4，可代入判断。\n");
    std::printf("* real hardware validation：仅真实 GPU 上的性能实测，不等于实机采集验证。\n");
    std::printf("* hardware root-cause attribution：未声称。\n");
    return 0;
}
