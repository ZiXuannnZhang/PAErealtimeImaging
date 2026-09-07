// ring_recon_cuda.cu - dual-wavelength ring-scan DAS CUDA kernel + host wrapper.
// Mathematically identical to the M1 CPU dasReconAppend: per-pixel x per A-line
// backprojection, linear/nearest interpolation, distance weighting, FOV mask,
// and incremental acc/accW accumulation.
#include "ring_recon_cuda.h"

#include <cuda_runtime.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr float  kTwoPiF = 6.2831854820251465f;
constexpr float  kPiF = 3.1415927410125732f;

char g_lastError[512] = {0};

void setError(const char* msg) {
    std::snprintf(g_lastError, sizeof(g_lastError), "%s", msg);
}

struct Handle {
    RingReconCudaConfig cfg;
    int nx = 0;
    int ny = 0;
    int nBlock = 0;
    float pre = 0.0f;
    float R = 0.0f;
    float R2 = 0.0f;
    float minDist = 0.0f;
    float fovRad = 0.0f;
    float fovTh0Rad = 0.0f;
    int   useFovMask = 0;
    int   spliceMask = 0;             // 拼接模式：1=逐 A-line 扇区反投影
    float sectorStartRad = 0.0f;      // 全局首扇区起点（rad）
    int   sectorCount = 0;            // 扇区数（启用通道数）
    int   linear = 1;
    int   maskOob = 1;
    float wExponent = 1.0f;
    int   nBound = 0;                 // 分层声速边界数
    float preOuter = 0.0f;            // fs/c_outer（分层）或 fs/c（单声速）
    float preCoeff[8] = {0.0f};       // fs*(1/c_i - 1/c_{i+1})
    float rb2s[8] = {0.0f};           // 边界半径平方

    float* d_acc = nullptr;
    float* d_accW = nullptr;
    float* d_xv = nullptr;
    float* d_yv = nullptr;
    float* d_bscan = nullptr;
    float* d_detx = nullptr;
    float* d_dety = nullptr;
    float* d_radius = nullptr;       // 逐根探测器半径（多扫描半径配准）
    float* d_wscale = nullptr;
    int*   d_sectorIdx = nullptr;    // 逐 A-line 扇区索引（拼接模式）
    float* d_pixPhi = nullptr;       // 每像素极角（rad，拼接模式复用）
    float* d_blendPrevW = nullptr;   // 拼接羽化：本像素向前一扇区的过渡权重
    float* d_blendNextW = nullptr;   // 拼接羽化：本像素向后一扇区的过渡权重
    float  blendSectorWidthDeg = -1.0f; // d_blendPrevW/NextW 已按此扇区宽度计算
    float* d_accV2 = nullptr;        // CF 门控：|w|·v² 二阶矩累积
    float* d_cfLut = nullptr;        // CF 门控：cf^gain 256 点查找表
    int    cfMask = 0;               // CF 门控启用
    float  cfEps = 1e-12f;           // CF 分母保护
    float* d_accP = nullptr;         // 非线性增强：Σ sign(x)·sqrt(|x|)
    float* d_accQ = nullptr;         // signed DMAS：Σ |x|（=Σ z²）
    int    nlMask = 0;               // 非线性增强启用
    int    dmasMask = 0;             // signed DMAS 启用
    int    nAlineAccum = 0;          // 本圈已累积 A-line 数
    float* d_preCoeff = nullptr;
    float* d_rb2s = nullptr;
    float* d_disp = nullptr;       // 显示快照输出（设备缓冲）
    int    d_bscanCap = 0;
    int    d_detCap = 0;
    int    d_dispCap = 0;
};

}  // namespace

extern "C" RING_RECON_CUDA_API void ring_recon_cuda_set_defaults(RingReconCudaConfig* cfg) {
    if (!cfg) return;
    std::memset(cfg, 0, sizeof(*cfg));

    // ---- acquisition / data source (MATLAB main script 14.dat defaults) ----
    cfg->dataNum = 14;
    cfg->sampDepth = 4000;
    cfg->reconDepth = 4000;
    cfg->spaceN = 1;
    cfg->frameNum = 1;
    cfg->numRevolutions = 1;
    cfg->daqHz = 200e6;
    cfg->radius = 6.57e-3;
    for (int i = 0; i < 8; ++i) cfg->radiusPerChannel[i] = 6.57e-3;
    cfg->multiRadius = 0;
    cfg->spliceMode = 0;
    cfg->spliceBlendDeg = 1.5;
    cfg->coherenceGate = 0;
    cfg->coherenceGain = 0.5;
    cfg->coherenceEpsilon = 1e-12;
    cfg->nonlinearMode = 0;
    cfg->shiftWL2 = 1;
    cfg->alinesPerFrame = 8000;
    cfg->alinesPerBlock = 200;
    for (int i = 0; i < 8; ++i) cfg->enabledChannels[i] = 1;
    cfg->enabledChannelCount = 8;
    cfg->alinesPerChannelPerBlock = 200;
    cfg->alinesPerChannelPerFrame = 500;
    cfg->sectorStartDeg = 180.0;
    cfg->sectorCcw = 1;
    cfg->triggerWlOdd = 1;
    cfg->timeoutResetSec = 0.0;   // 0=关闭超时重置

    // ---- sound speed model ----
    cfg->soundSpeedRadiiCount = 0;
    cfg->soundSpeedsCount = 2;
    cfg->soundSpeeds[0] = 1490.0;
    cfg->soundSpeeds[1] = 1540.0;

    // ---- imaging grid and DAS ----
    cfg->fov = 36e-3;
    cfg->gridSize = 0.01e-3;
    cfg->fovDeg = 360.0;
    cfg->fovTheta0Deg = 0.0;
    cfg->normalizeByW = 0;
    cfg->apodType = 0;
    cfg->distanceWeightExponent = 1.0;
    cfg->interpolation = 0;     // linear
    cfg->minDistance = 0.0;
    cfg->maskOutOfRange = 1;

    // ---- preprocessing ----
    cfg->trigdejit = 0;
    cfg->phaseDecon = 0;
    cfg->gaussfil = 0;
    cfg->gaussfilRowstart = 1900;
    cfg->gaussfilRowsend = 2400;
    cfg->filterLow = 0;
    cfg->wLow = 0.4e6;
    cfg->n1 = 4;
    cfg->filterHigh = 0;
    cfg->wHigh = 40e6;
    cfg->n2 = 4;
    cfg->med = 0;
    cfg->arcRemove = 0;
    cfg->dbrSigRemove = 1;
    cfg->maskLength = 300;
    cfg->singalImpair = 0;
    cfg->imValue[0] = 2000.0;
    cfg->imValue[1] = 400.0;
    cfg->delayCut = 1;
    cfg->sysDelay[0] = 358;
    cfg->sysDelay[1] = 371;
    cfg->dejitcheck = 0;

    // ---- DAS-specific ----
    cfg->scanArtifactRemove = 0;
    cfg->decay = 0.05;
    cfg->scanN = 4000;
    cfg->scanK = 2280;
    cfg->scanDmin = 340.0;
    cfg->scanDref = 1905.0;
    cfg->scanWin = 50.0;

    // ---- display / storage ----
    cfg->displayRecon = 1;
    cfg->bscanViewOrig = 0;
    cfg->bAline1 = 1;
    cfg->bAline2 = 4500;
    cfg->paDataView = 0;
    cfg->cBscan[0] = 200.0;
    cfg->cBscan[1] = 200.0;
    cfg->save1 = 0;
    cfg->simulateTiming = 0;
    cfg->refreshPauseSec = 0.05;
    cfg->verifyFinal = 0;
    cfg->curveNum[0] = 1;
    cfg->curveNum[1] = 2;
    cfg->curveNum[2] = 3;
    cfg->curveNum[3] = 4;
    cfg->curveNumCount = 4;
    cfg->figXylim[0] = 501.0;
    cfg->figXylim[1] = 1500.0;
    cfg->figXylim[2] = 300.0;
    cfg->figXylim[3] = 900.0;
    cfg->figClim[0] = -200.0;
    cfg->figClim[1] = 200.0;
}

// 拼接模式：预计算每像素极角（与 FOV 掩码相同的 atan2f 语义），供逐 A-line
// 扇区掩码复用，避免每个 block 重复计算全图像素角。
__global__ void ring_pixel_phi_kernel(
        const float* __restrict__ xv, const float* __restrict__ yv,
        int nx, int ny, float* __restrict__ phi) {
    const int idx = blockIdx.x * blockDim.x + threadIdx.x;
    const int total = nx * ny;
    if (idx >= total) return;

    const int ix = idx / ny;
    const int iy = idx - ix * ny;
    phi[idx] = atan2f(yv[iy], xv[ix]);
}

// 拼接羽化：计算每个像素向前/向后相邻扇区的过渡权重（余弦羽化）。
// 像素位于扇区边界过渡带时，邻扇区权重互补（本扇区权重 = 1 - prev - next）。
__global__ void ring_blend_weights_kernel(
        const float* __restrict__ pixPhi, int npix,
        float sectorStartRad, float sectorFovRad, int sectorCount, float blendRad,
        float* __restrict__ prevW, float* __restrict__ nextW) {
    const int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= npix) return;

    float u = fmodf(pixPhi[idx] - sectorStartRad, kTwoPiF);
    if (u < 0.0f) u += kTwoPiF;
    u /= sectorFovRad;                          // [0, sectorCount)
    if (u >= static_cast<float>(sectorCount)) u = static_cast<float>(sectorCount) - 1.0f;
    else if (u < 0.0f) u = 0.0f;
    const float kf = floorf(u);
    const float frac = u - kf;                  // 本扇区内归一化位置 [0,1)
    const float dc = fabsf(frac - 0.5f);        // 到本扇区中心的归一化角距
    const float bw = blendRad / sectorFovRad;   // 羽化带归一化半宽

    float nb = 0.0f;
    if (dc > 0.5f - bw) {
        float t = (dc - (0.5f - bw)) / (2.0f * bw);
        t = fminf(fmaxf(t, 0.0f), 1.0f);
        const float own = 0.5f + 0.5f * __cosf(kPiF * t);
        nb = 1.0f - own;                        // 邻扇区互补权重
    }
    // 靠向前一半边界 → 前一扇区；靠向后一半边界 → 后一扇区
    if (frac < 0.5f) { prevW[idx] = nb; nextW[idx] = 0.0f; }
    else             { prevW[idx] = 0.0f; nextW[idx] = nb; }
}

__global__ void ring_das_kernel(
        const float* __restrict__ bscan, int nt, int nd,
        const float* __restrict__ detx, const float* __restrict__ dety,
        const float* __restrict__ wscale,
        const float* __restrict__ radius,   // 逐根探测器半径（多扫描半径配准）
        const int*   __restrict__ sectorIdx, // 逐 A-line 扇区索引（拼接模式）
        float sectorStartRad, float sectorFovRad, int sectorCount, int sectorMask,
        const float* __restrict__ pixPhi,   // 每像素极角（拼接模式）
        int blendMask,                      // 拼接羽化：1=使用 prev/next 邻扇区权重
        const float* __restrict__ blendPrevW, const float* __restrict__ blendNextW,
        const float* __restrict__ xv, const float* __restrict__ yv,
        int nx, int ny,
        float pre, float minDist, float wExponent,
        int linear, int maskOob, int useFovMask, float fovRad, float fovTh0Rad,
        int nBound, float preOuter,
        const float* __restrict__ preCoeff, const float* __restrict__ rb2s,
        float* __restrict__ acc, float* __restrict__ accW,
        int cfAccum, float* __restrict__ accV2) {
    const int idx = blockIdx.x * blockDim.x + threadIdx.x;
    const int total = nx * ny;
    if (idx >= total) return;

    const int ix = idx / ny;
    const int iy = idx - ix * ny;
    const float x = xv[ix];
    const float y = yv[iy];
    const float r2 = x * x + y * y;
    const float phiS = sectorMask ? pixPhi[idx] : 0.0f;
    int pixSector = 0;
    if (sectorMask) {
        // 全局统一扇区划分：floor((phi - sectorStart)/sectorWidth)，保证无重叠/无空洞
        float dsec = fmodf(phiS - sectorStartRad, kTwoPiF);
        if (dsec < 0.0f) dsec += kTwoPiF;
        pixSector = static_cast<int>(dsec / sectorFovRad);
        if (pixSector < 0) pixSector = 0;
        if (pixSector >= sectorCount) pixSector = sectorCount - 1;
    }

    if (useFovMask) {
        float phi = atan2f(y, x);
        float dang = fmodf(phi - fovTh0Rad, kTwoPiF);
        if (dang < 0.0f) dang += kTwoPiF;
        if (dang > fovRad) return;
    }

    float accv = 0.0f;
    float accwv = 0.0f;
    float accv2 = 0.0f;
    for (int j = 0; j < nd; ++j) {
        // 拼接模式：硬切（v1）或重叠余弦羽化（δ>0）
        float blendW = 1.0f;
        if (sectorMask) {
            if (!blendMask) {
                if (pixSector != sectorIdx[j]) continue;
            } else {
                const int ds = sectorIdx[j] - pixSector;
                if (ds == 0) {
                    blendW = 1.0f - blendPrevW[idx] - blendNextW[idx];
                } else if (ds == 1 || ds == 1 - sectorCount) {
                    blendW = blendNextW[idx];
                } else if (ds == -1 || ds == sectorCount - 1) {
                    blendW = blendPrevW[idx];
                } else {
                    continue;
                }
                if (!(blendW > 0.0f)) continue;
            }
        }
        // 逐根半径：探测器相关量按 Rj/Rj² 计算（统一半径时 radius[j] 全同，数值与旧版一致）
        const float Rj = radius[j];
        const float R2j = Rj * Rj;
        const float proj = x * detx[j] + y * dety[j];
        const float dotp = proj - R2j;
        const float dist2 = (r2 - R2j) - 2.0f * dotp;
        const float dist = sqrtf(fmaxf(dist2, 0.0f));
        const float dsafe = fmaxf(dist, minDist);
        // 分层声速走时（与 MATLAB das_recon_circular_gpu_v2 一致）：
        //   tf = d/c_outer + sum_i Li*(1/c_i - 1/c_{i+1})；单声速时退化为 d/c
        float tf = dist * (nBound > 0 ? preOuter : pre);
        for (int bi = 0; bi < nBound; ++bi) {
            const float rb2 = rb2s[bi];
            const bool bothIn = (R2j <= rb2) && (r2 <= rb2);
            const float discr4 = dotp * dotp - dist2 * (R2j - rb2);
            const bool cross = discr4 > 0.0f;
            const float sd = sqrtf(fmaxf(discr4, 0.0f));
            const float dist2d = fmaxf(dist2, 1e-12f);
            const float u1 = (-dotp - sd) / dist2d;
            const float u2 = (-dotp + sd) / dist2d;
            float lc = (fminf(fmaxf(u2, 0.0f), 1.0f) -
                        fmaxf(fminf(u1, 1.0f), 0.0f)) * dist;
            lc = fmaxf(lc, 0.0f);
            float li = dist * (bothIn ? 1.0f : 0.0f);
            if (cross && !bothIn) li += lc;
            tf += li * preCoeff[bi];
        }

        float vv = 0.0f;
        if (linear) {
            const float i0f = floorf(tf);
            const float frac = tf - i0f;
            const int i0 = static_cast<int>(i0f) + 1;
            const int valid = (i0 >= 1) && (i0 <= nt - 1);
            int i0c = i0 < 1 ? 1 : (i0 > nt - 1 ? nt - 1 : i0);
            const size_t base = static_cast<size_t>(j) * nt;
            const float v0 = bscan[base + i0c - 1];
            const float v1 = bscan[base + i0c];
            vv = v0 + frac * (v1 - v0);
            if (maskOob && !valid) vv = 0.0f;
        } else {
            const int i0n = static_cast<int>(floorf(tf + 0.5f)) + 1;
            const int valid = (i0n >= 1) && (i0n <= nt);
            int i0c = i0n < 1 ? 1 : (i0n > nt ? nt : i0n);
            vv = bscan[static_cast<size_t>(j) * nt + i0c - 1];
            if (maskOob && !valid) vv = 0.0f;
        }

        const float pw = wExponent + 1.0f;
        float dsafeP;
        if (pw == 2.0f) {
            dsafeP = dsafe * dsafe;
        } else if (pw == 1.0f) {
            dsafeP = dsafe;
        } else {
            dsafeP = powf(dsafe, pw);
        }
        const float wBase = wscale[j] * dotp / (Rj * dsafeP);
        const float w = blendMask ? wBase * blendW : wBase;
        accv += w * vv;
        accwv += fabsf(w);
        if (cfAccum) accv2 += fabsf(w) * vv * vv;
    }
    acc[idx] += accv;
    accW[idx] += accwv;
    if (cfAccum) accV2[idx] += accv2;
}

// 非线性增强专用内核：与 ring_das_kernel 相同，并额外累积
// accP=Σsign(x)·sqrt(|x|) 与 accQ=Σ|x|（仅 DMAS）。
// 单独成核是为了保证 nonlinearMode=0 时旧内核代码路径与改动前逐字节一致。
__global__ void ring_das_kernel_nl(
        const float* __restrict__ bscan, int nt, int nd,
        const float* __restrict__ detx, const float* __restrict__ dety,
        const float* __restrict__ wscale,
        const float* __restrict__ radius,
        const int*   __restrict__ sectorIdx,
        float sectorStartRad, float sectorFovRad, int sectorCount, int sectorMask,
        const float* __restrict__ pixPhi,
        int blendMask,
        const float* __restrict__ blendPrevW, const float* __restrict__ blendNextW,
        const float* __restrict__ xv, const float* __restrict__ yv,
        int nx, int ny,
        float pre, float minDist, float wExponent,
        int linear, int maskOob, int useFovMask, float fovRad, float fovTh0Rad,
        int nBound, float preOuter,
        const float* __restrict__ preCoeff, const float* __restrict__ rb2s,
        float* __restrict__ acc, float* __restrict__ accW,
        int cfAccum, float* __restrict__ accV2,
        int dmasMask, float* __restrict__ accP, float* __restrict__ accQ) {
    const int idx = blockIdx.x * blockDim.x + threadIdx.x;
    const int total = nx * ny;
    if (idx >= total) return;

    const int ix = idx / ny;
    const int iy = idx - ix * ny;
    const float x = xv[ix];
    const float y = yv[iy];
    const float r2 = x * x + y * y;
    const float phiS = sectorMask ? pixPhi[idx] : 0.0f;
    int pixSector = 0;
    if (sectorMask) {
        float dsec = fmodf(phiS - sectorStartRad, kTwoPiF);
        if (dsec < 0.0f) dsec += kTwoPiF;
        pixSector = static_cast<int>(dsec / sectorFovRad);
        if (pixSector < 0) pixSector = 0;
        if (pixSector >= sectorCount) pixSector = sectorCount - 1;
    }

    if (useFovMask) {
        float phi = atan2f(y, x);
        float dang = fmodf(phi - fovTh0Rad, kTwoPiF);
        if (dang < 0.0f) dang += kTwoPiF;
        if (dang > fovRad) return;
    }

    float accv = 0.0f;
    float accwv = 0.0f;
    float accv2 = 0.0f;
    float accp = 0.0f;
    float accq = 0.0f;
    for (int j = 0; j < nd; ++j) {
        float blendW = 1.0f;
        if (sectorMask) {
            if (!blendMask) {
                if (pixSector != sectorIdx[j]) continue;
            } else {
                const int ds = sectorIdx[j] - pixSector;
                if (ds == 0) {
                    blendW = 1.0f - blendPrevW[idx] - blendNextW[idx];
                } else if (ds == 1 || ds == 1 - sectorCount) {
                    blendW = blendNextW[idx];
                } else if (ds == -1 || ds == sectorCount - 1) {
                    blendW = blendPrevW[idx];
                } else {
                    continue;
                }
                if (!(blendW > 0.0f)) continue;
            }
        }
        const float Rj = radius[j];
        const float R2j = Rj * Rj;
        const float proj = x * detx[j] + y * dety[j];
        const float dotp = proj - R2j;
        const float dist2 = (r2 - R2j) - 2.0f * dotp;
        const float dist = sqrtf(fmaxf(dist2, 0.0f));
        const float dsafe = fmaxf(dist, minDist);
        float tf = dist * (nBound > 0 ? preOuter : pre);
        for (int bi = 0; bi < nBound; ++bi) {
            const float rb2 = rb2s[bi];
            const bool bothIn = (R2j <= rb2) && (r2 <= rb2);
            const float discr4 = dotp * dotp - dist2 * (R2j - rb2);
            const bool cross = discr4 > 0.0f;
            const float sd = sqrtf(fmaxf(discr4, 0.0f));
            const float dist2d = fmaxf(dist2, 1e-12f);
            const float u1 = (-dotp - sd) / dist2d;
            const float u2 = (-dotp + sd) / dist2d;
            float lc = (fminf(fmaxf(u2, 0.0f), 1.0f) -
                        fmaxf(fminf(u1, 1.0f), 0.0f)) * dist;
            lc = fmaxf(lc, 0.0f);
            float li = dist * (bothIn ? 1.0f : 0.0f);
            if (cross && !bothIn) li += lc;
            tf += li * preCoeff[bi];
        }

        float vv = 0.0f;
        if (linear) {
            const float i0f = floorf(tf);
            const float frac = tf - i0f;
            const int i0 = static_cast<int>(i0f) + 1;
            const int valid = (i0 >= 1) && (i0 <= nt - 1);
            int i0c = i0 < 1 ? 1 : (i0 > nt - 1 ? nt - 1 : i0);
            const size_t base = static_cast<size_t>(j) * nt;
            const float v0 = bscan[base + i0c - 1];
            const float v1 = bscan[base + i0c];
            vv = v0 + frac * (v1 - v0);
            if (maskOob && !valid) vv = 0.0f;
        } else {
            const int i0n = static_cast<int>(floorf(tf + 0.5f)) + 1;
            const int valid = (i0n >= 1) && (i0n <= nt);
            int i0c = i0n < 1 ? 1 : (i0n > nt ? nt : i0n);
            vv = bscan[static_cast<size_t>(j) * nt + i0c - 1];
            if (maskOob && !valid) vv = 0.0f;
        }

        const float pw = wExponent + 1.0f;
        float dsafeP;
        if (pw == 2.0f) {
            dsafeP = dsafe * dsafe;
        } else if (pw == 1.0f) {
            dsafeP = dsafe;
        } else {
            dsafeP = powf(dsafe, pw);
        }
        const float wBase = wscale[j] * dotp / (Rj * dsafeP);
        const float w = blendMask ? wBase * blendW : wBase;
        accv += w * vv;
        accwv += fabsf(w);
        if (cfAccum) accv2 += fabsf(w) * vv * vv;
        // signed DMAS/pCF 公共项：z = sign(x)·sqrt(|x|)，x = w·v
        const float xnl = w * vv;
        accp += (xnl >= 0.0f) ? sqrtf(xnl) : -sqrtf(-xnl);
        if (dmasMask) accq += fabsf(xnl);
    }
    acc[idx] += accv;
    accW[idx] += accwv;
    if (cfAccum) accV2[idx] += accv2;
    accP[idx] += accp;
    if (dmasMask) accQ[idx] += accq;
}

// 显示快照：acc/accW 归一化后按 step×step 块平均（边界按实际像素数平均），
// 与现 UI showRingImage 的降采样语义一致；方案A 下 step=1、dn=nx 即全分辨率归一化。
__global__ void ring_snapshot_kernel(
        const float* __restrict__ acc, const float* __restrict__ accW,
        int nx, int ny, int dn, int step,
        int cfEnable, float cfEps, const float* __restrict__ accV2,
        const float* __restrict__ cfLut,
        int nlMode, const float* __restrict__ accP, const float* __restrict__ accQ,
        float nA, float nPairDenom,
        float* __restrict__ out) {
    const int idx = blockIdx.x * blockDim.x + threadIdx.x;
    const int total = dn * dn;
    if (idx >= total) return;

    const int dy = idx / dn;
    const int dx = idx - dy * dn;
    const int y0 = dy * step;
    const int x0 = dx * step;
    float s = 0.0f;
    int cnt = 0;
    for (int yy = 0; yy < step; ++yy) {
        const int sy = y0 + yy;
        if (sy >= ny) break;
        const size_t row = static_cast<size_t>(sy) * nx;
        for (int xx = 0; xx < step; ++xx) {
            const int sx = x0 + xx;
            if (sx >= nx) break;
            const float w = fmaxf(accW[row + sx], 1e-12f);
            float v = acc[row + sx] / w;
            if (nlMode == 2) {
                // signed DMAS：D = (accP² - accQ) / max(N·(N-1),1)
                const size_t pi = row + sx;
                const float raw = accP[pi] * accP[pi] - accQ[pi];
                v = raw / fmaxf(nPairDenom, 1e-12f);
            } else if (nlMode == 1) {
                // pCF(p=0.5)：P = sign(accP)·accP² / max(N·accW, ε)
                const size_t pi = row + sx;
                const float ap = accP[pi];
                v = copysignf(ap * ap, ap) / fmaxf(nA * accW[pi], 1e-12f);
            }
            if (cfEnable) {
                const float denom = fmaxf(w * accV2[row + sx], cfEps);
                float cf = (acc[row + sx] * acc[row + sx]) / denom;
                cf = fminf(fmaxf(cf, 0.0f), 1.0f);
                const int li = static_cast<int>(cf * 255.0f + 0.5f);
                v *= cfLut[li];
            }
            s += v;
            ++cnt;
        }
    }
    out[idx] = (cnt > 0) ? (s / static_cast<float>(cnt)) : 0.0f;
}

extern "C" RING_RECON_CUDA_API int ring_recon_cuda_create(const RingReconCudaConfig* cfg, void** out) {
    if (!cfg || !out) {
        setError("null config/out");
        return 1;
    }
    if (cfg->fov <= 0.0 || cfg->gridSize <= 0.0) {
        setError("fov/gridSize must be positive");
        return 1;
    }
    const int nBound = (cfg->soundSpeedRadiiCount > 0)
                       ? (cfg->soundSpeedRadiiCount < 8 ? cfg->soundSpeedRadiiCount : 8)
                       : 0;
    if (nBound > 0) {
        if (cfg->soundSpeedsCount < nBound + 1) {
            setError("SoundSpeeds must have at least SoundSpeedRadiiCount+1 entries");
            return 1;
        }
        for (int i = 0; i < nBound; ++i) {
            if (cfg->soundSpeedRadii[i] <= 0.0) {
                setError("SoundSpeedRadii must be positive");
                return 1;
            }
            if (i > 0 && cfg->soundSpeedRadii[i] <= cfg->soundSpeedRadii[i - 1]) {
                setError("SoundSpeedRadii must be strictly ascending");
                return 1;
            }
        }
        for (int i = 0; i <= nBound; ++i) {
            if (cfg->soundSpeeds[i] <= 0.0) {
                setError("SoundSpeeds must be positive");
                return 1;
            }
        }
    }
    if (cfg->radius <= 0.0 || cfg->daqHz <= 0.0) {
        setError("radius/daqHz must be positive");
        return 1;
    }
    const double c = (cfg->soundSpeedsCount > 0) ? cfg->soundSpeeds[0] : 1490.0;
    if (c <= 0.0) {
        setError("sound speed must be positive");
        return 1;
    }

    Handle* h = new Handle();
    h->cfg = *cfg;
    h->nx = static_cast<int>(std::ceil(cfg->fov / cfg->gridSize));
    h->ny = h->nx;
    h->pre = static_cast<float>(cfg->daqHz / c);
    h->R = static_cast<float>(cfg->radius);
    h->R2 = h->R * h->R;
    h->nBound = nBound;
    if (nBound > 0) {
        h->preOuter = static_cast<float>(cfg->daqHz / cfg->soundSpeeds[nBound]);
        for (int i = 0; i < nBound; ++i) {
            const double rb2 = cfg->soundSpeedRadii[i] * cfg->soundSpeedRadii[i];
            h->preCoeff[i] = static_cast<float>(
                cfg->daqHz * (1.0 / cfg->soundSpeeds[i] - 1.0 / cfg->soundSpeeds[i + 1]));
            h->rb2s[i] = static_cast<float>(rb2);
        }
    } else {
        h->preOuter = static_cast<float>(cfg->daqHz / c);
    }
    h->minDist = static_cast<float>(cfg->minDistance > 0.0 ? cfg->minDistance : cfg->gridSize);
    h->useFovMask = (cfg->fovDeg < 359.9999) ? 1 : 0;
    h->spliceMask = (cfg->spliceMode != 0) ? 1 : 0;
    h->cfMask = (cfg->coherenceGate != 0) ? 1 : 0;
    h->cfEps = static_cast<float>(cfg->coherenceEpsilon > 0.0 ? cfg->coherenceEpsilon : 1e-12);
    h->nlMask = (cfg->nonlinearMode != 0) ? 1 : 0;
    h->dmasMask = (cfg->nonlinearMode == 2) ? 1 : 0;
    h->nAlineAccum = 0;
    h->sectorStartRad = static_cast<float>(cfg->sectorStartDeg * kPi / 180.0);
    h->sectorCount = cfg->enabledChannelCount;
    h->fovRad = static_cast<float>(cfg->fovDeg * kPi / 180.0);
    h->fovTh0Rad = static_cast<float>(cfg->fovTheta0Deg * kPi / 180.0);
    h->linear = (cfg->interpolation == 0) ? 1 : 0;
    h->maskOob = cfg->maskOutOfRange ? 1 : 0;
    h->wExponent = static_cast<float>(cfg->distanceWeightExponent);

    std::vector<float> xv(h->nx), yv(h->ny);
    for (int i = 0; i < h->nx; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(h->nx - 1);
        const float v = static_cast<float>(-cfg->fov * 0.5 + t * cfg->fov);
        xv[i] = v;
        yv[i] = v;
    }

    const size_t npix = static_cast<size_t>(h->nx) * h->ny;
    cudaError_t err = cudaMalloc(&h->d_acc, npix * sizeof(float));
    if (err == cudaSuccess) err = cudaMalloc(&h->d_accW, npix * sizeof(float));
    if (err == cudaSuccess) err = cudaMalloc(&h->d_xv, static_cast<size_t>(h->nx) * sizeof(float));
    if (err == cudaSuccess) err = cudaMalloc(&h->d_yv, static_cast<size_t>(h->ny) * sizeof(float));
    if (err == cudaSuccess) err = cudaMemcpy(h->d_xv, xv.data(), static_cast<size_t>(h->nx) * sizeof(float), cudaMemcpyHostToDevice);
    if (err == cudaSuccess) err = cudaMemcpy(h->d_yv, yv.data(), static_cast<size_t>(h->ny) * sizeof(float), cudaMemcpyHostToDevice);
    if (err == cudaSuccess) err = cudaMemset(h->d_acc, 0, npix * sizeof(float));
    if (err == cudaSuccess) err = cudaMemset(h->d_accW, 0, npix * sizeof(float));
    if (err == cudaSuccess) err = cudaMalloc(&h->d_preCoeff, 8 * sizeof(float));
    if (err == cudaSuccess) err = cudaMalloc(&h->d_rb2s, 8 * sizeof(float));
    if (err == cudaSuccess) {
        err = cudaMemcpy(h->d_preCoeff, h->preCoeff, 8 * sizeof(float), cudaMemcpyHostToDevice);
    }
    if (err == cudaSuccess) {
        err = cudaMemcpy(h->d_rb2s, h->rb2s, 8 * sizeof(float), cudaMemcpyHostToDevice);
    }
    if (h->spliceMask) {
        if (err == cudaSuccess) err = cudaMalloc(&h->d_pixPhi, npix * sizeof(float));
        if (err == cudaSuccess) {
            const int threads = 256;
            const int blocks = (static_cast<int>(npix) + threads - 1) / threads;
            ring_pixel_phi_kernel<<<blocks, threads>>>(
                h->d_xv, h->d_yv, h->nx, h->ny, h->d_pixPhi);
            err = cudaDeviceSynchronize();
        }
    }
    if (h->cfMask) {
        if (err == cudaSuccess) err = cudaMalloc(&h->d_accV2, npix * sizeof(float));
        if (err == cudaSuccess) err = cudaMemset(h->d_accV2, 0, npix * sizeof(float));
        if (err == cudaSuccess) err = cudaMalloc(&h->d_cfLut, 256 * sizeof(float));
        if (err == cudaSuccess) {
            std::vector<float> lut(256);
            const double gain = cfg->coherenceGain > 0.0 ? cfg->coherenceGain : 0.0;
            for (int i = 0; i < 256; ++i)
                lut[i] = static_cast<float>(std::pow(static_cast<double>(i) / 255.0, gain));
            err = cudaMemcpy(h->d_cfLut, lut.data(), 256 * sizeof(float), cudaMemcpyHostToDevice);
        }
    }
    if (h->nlMask) {
        if (err == cudaSuccess) err = cudaMalloc(&h->d_accP, npix * sizeof(float));
        if (err == cudaSuccess) err = cudaMemset(h->d_accP, 0, npix * sizeof(float));
        if (err == cudaSuccess && h->dmasMask)
            err = cudaMalloc(&h->d_accQ, npix * sizeof(float));
        if (err == cudaSuccess && h->dmasMask)
            err = cudaMemset(h->d_accQ, 0, npix * sizeof(float));
    }
    if (err != cudaSuccess) {
        setError(cudaGetErrorString(err));
        ring_recon_cuda_destroy(h);
        return 1;
    }
    *out = h;
    return 0;
}

extern "C" RING_RECON_CUDA_API void ring_recon_cuda_destroy(void* handle) {
    if (!handle) return;
    Handle* h = static_cast<Handle*>(handle);
    if (h->d_acc) cudaFree(h->d_acc);
    if (h->d_accW) cudaFree(h->d_accW);
    if (h->d_xv) cudaFree(h->d_xv);
    if (h->d_yv) cudaFree(h->d_yv);
    if (h->d_bscan) cudaFree(h->d_bscan);
    if (h->d_detx) cudaFree(h->d_detx);
    if (h->d_dety) cudaFree(h->d_dety);
    if (h->d_wscale) cudaFree(h->d_wscale);
    if (h->d_preCoeff) cudaFree(h->d_preCoeff);
    if (h->d_rb2s) cudaFree(h->d_rb2s);
    if (h->d_radius) cudaFree(h->d_radius);
    if (h->d_sectorIdx) cudaFree(h->d_sectorIdx);
    if (h->d_pixPhi) cudaFree(h->d_pixPhi);
    if (h->d_blendPrevW) cudaFree(h->d_blendPrevW);
    if (h->d_blendNextW) cudaFree(h->d_blendNextW);
    if (h->d_accV2) cudaFree(h->d_accV2);
    if (h->d_cfLut) cudaFree(h->d_cfLut);
    if (h->d_accP) cudaFree(h->d_accP);
    if (h->d_accQ) cudaFree(h->d_accQ);
    if (h->d_disp) cudaFree(h->d_disp);
    delete h;
}

extern "C" RING_RECON_CUDA_API int ring_recon_cuda_reset(void* handle) {
    if (!handle) {
        setError("null handle");
        return 1;
    }
    Handle* h = static_cast<Handle*>(handle);
    const size_t npix = static_cast<size_t>(h->nx) * h->ny;
    cudaError_t err = cudaMemset(h->d_acc, 0, npix * sizeof(float));
    if (err == cudaSuccess)
        err = cudaMemset(h->d_accW, 0, npix * sizeof(float));
    if (err == cudaSuccess && h->cfMask)
        err = cudaMemset(h->d_accV2, 0, npix * sizeof(float));
    if (err == cudaSuccess && h->nlMask)
        err = cudaMemset(h->d_accP, 0, npix * sizeof(float));
    if (err == cudaSuccess && h->dmasMask)
        err = cudaMemset(h->d_accQ, 0, npix * sizeof(float));
    if (err != cudaSuccess) {
        setError(cudaGetErrorString(err));
        return 1;
    }
    h->nBlock = 0;
    h->nAlineAccum = 0;
    return 0;
}

extern "C" RING_RECON_CUDA_API int ring_recon_cuda_snapshot(void* handle,
                                                            int dn, int step,
                                                            float* hostOut) {
    if (!handle || !hostOut || dn < 1 || step < 1) {
        setError("bad snapshot args");
        return 1;
    }
    Handle* h = static_cast<Handle*>(handle);

    const size_t need = static_cast<size_t>(dn) * dn * sizeof(float);
    if (need > static_cast<size_t>(h->d_dispCap)) {
        if (h->d_disp) cudaFree(h->d_disp);
        h->d_disp = nullptr;
        cudaError_t err = cudaMalloc(&h->d_disp, need);
        if (err != cudaSuccess) {
            setError(cudaGetErrorString(err));
            return 1;
        }
        h->d_dispCap = static_cast<int>(need);
    }

    const int total = dn * dn;
    const int threads = 256;
    const int blocks = (total + threads - 1) / threads;
    const float nA = static_cast<float>(h->nAlineAccum > 0 ? h->nAlineAccum : 1);
    const float nPairDenom = fmaxf(nA * (nA - 1.0f), 1.0f);
    ring_snapshot_kernel<<<blocks, threads>>>(
        h->d_acc, h->d_accW, h->nx, h->ny, dn, step,
        h->cfMask, h->cfEps, h->d_accV2, h->d_cfLut,
        h->cfg.nonlinearMode, h->d_accP, h->d_accQ, nA, nPairDenom,
        h->d_disp);
    cudaError_t err = cudaDeviceSynchronize();
    if (err != cudaSuccess) {
        setError(cudaGetErrorString(err));
        return 1;
    }
    err = cudaMemcpy(hostOut, h->d_disp, need, cudaMemcpyDeviceToHost);
    if (err != cudaSuccess) {
        setError(cudaGetErrorString(err));
        return 1;
    }
    return 0;
}

static int appendImpl(void* handle, const float* bscan,
                      int nt, int nd,
                      const float* thetaDeg, const float* radii,
                      const float* sectorTheta0Deg, float sectorWidthDeg,
                      double blockStartDeg,
                      double blockSpanDeg) {
    if (!handle || !bscan || nt < 2 || nd < 2) {
        setError("bad append args");
        return 1;
    }
    Handle* h = static_cast<Handle*>(handle);

    // 拼接模式：仅当句柄使能、扇区数>1且调用方提供合法扇区宽度时逐 A-line 门控。
    const int sectorMask = (h->spliceMask && h->sectorCount > 1 && sectorTheta0Deg &&
                            sectorWidthDeg > 0.0f && sectorWidthDeg < 360.0f) ? 1 : 0;
    const float sectorFovRad = sectorMask
        ? static_cast<float>(static_cast<double>(sectorWidthDeg) * kPi / 180.0) : 0.0f;
    // 拼接羽化：δ=0 时关闭（走 v1 硬切原路径）；δ 被 clamp 到小于半扇区宽
    const int blendMask = (sectorMask && h->cfg.spliceBlendDeg > 0.0 &&
                           h->cfg.spliceBlendDeg < 0.5 * static_cast<double>(sectorWidthDeg))
                          ? 1 : 0;
    const float blendRad = blendMask
        ? static_cast<float>(h->cfg.spliceBlendDeg * kPi / 180.0) : 0.0f;

    // detector positions, per-line radii and weights (with apodization)
    std::vector<float> detx(nd), dety(nd), wscale(nd), rad(nd);
    std::vector<int> sectorIdx(nd);
    const double sectorStartDeg = h->cfg.sectorStartDeg;
    const double widthDeg = static_cast<double>(sectorWidthDeg);
    const double stepDeg = blockSpanDeg / static_cast<double>(nd - 1);
    double arc = blockSpanDeg / static_cast<double>(nd - 1) * kPi / 180.0;
    if (thetaDeg) {
        // Per-line angles: use the average per-line angular step so the
        // distance weight is non-zero and independent of sector layout.
        double totalArc = 0.0;
        for (int j = 1; j < nd; ++j)
            totalArc += std::fabs(static_cast<double>(thetaDeg[j]) -
                                  static_cast<double>(thetaDeg[j - 1]));
        arc = totalArc / static_cast<double>(nd - 1) * kPi / 180.0;
    }
    for (int j = 0; j < nd; ++j) {
        const double angDeg = thetaDeg ? static_cast<double>(thetaDeg[j])
                                       : (blockStartDeg + j * stepDeg);
        const float th = static_cast<float>(angDeg * kPi / 180.0);
        // 多扫描半径配准：radii 为空时全部使用统一半径 h->R（数值与旧版一致）
        const float Rj = radii ? radii[j] : h->R;
        rad[j] = Rj;
        detx[j] = Rj * cosf(th);
        dety[j] = Rj * sinf(th);
        if (sectorMask) {
            double delta = std::fmod(static_cast<double>(sectorTheta0Deg[j]) - sectorStartDeg, 360.0);
            if (delta < 0.0) delta += 360.0;
            int idx = static_cast<int>(delta / widthDeg + 1e-9);
            if (idx < 0) idx = 0;
            if (idx >= h->sectorCount) idx = h->sectorCount - 1;
            sectorIdx[j] = idx;
        } else {
            sectorIdx[j] = 0;
        }
        double apod = 1.0;
        if (h->cfg.apodType == 1) {
            apod = 0.5 - 0.5 * cos(2.0 * kPi * j / nd);
        } else if (h->cfg.apodType == 2) {
            apod = 0.54 - 0.46 * cos(2.0 * kPi * j / nd);
        }
        wscale[j] = static_cast<float>(-arc * apod);
    }

    const size_t bscanBytes = static_cast<size_t>(nt) * nd * sizeof(float);
    if (bscanBytes > static_cast<size_t>(h->d_bscanCap)) {
        if (h->d_bscan) cudaFree(h->d_bscan);
        h->d_bscan = nullptr;
        cudaError_t err = cudaMalloc(&h->d_bscan, bscanBytes);
        if (err != cudaSuccess) {
            setError(cudaGetErrorString(err));
            return 1;
        }
        h->d_bscanCap = static_cast<int>(bscanBytes);
    }
    if (nd > h->d_detCap) {
        if (h->d_detx) cudaFree(h->d_detx);
        if (h->d_dety) cudaFree(h->d_dety);
        if (h->d_wscale) cudaFree(h->d_wscale);
        if (h->d_radius) cudaFree(h->d_radius);
        if (h->d_sectorIdx) cudaFree(h->d_sectorIdx);
        cudaError_t e1 = cudaMalloc(&h->d_detx, static_cast<size_t>(nd) * sizeof(float));
        cudaError_t e2 = cudaMalloc(&h->d_dety, static_cast<size_t>(nd) * sizeof(float));
        cudaError_t e3 = cudaMalloc(&h->d_wscale, static_cast<size_t>(nd) * sizeof(float));
        cudaError_t e4 = cudaMalloc(&h->d_radius, static_cast<size_t>(nd) * sizeof(float));
        cudaError_t e5 = cudaMalloc(&h->d_sectorIdx, static_cast<size_t>(nd) * sizeof(int));
        if (e1 != cudaSuccess || e2 != cudaSuccess || e3 != cudaSuccess ||
            e4 != cudaSuccess || e5 != cudaSuccess) {
            setError("cudaMalloc det arrays failed");
            return 1;
        }
        h->d_detCap = nd;
    }

    const int total = h->nx * h->ny;
    const int threads = 256;
    const int blocks = (total + threads - 1) / threads;

    if (blendMask && !h->d_blendPrevW) {
        const size_t pixBytes = static_cast<size_t>(total) * sizeof(float);
        cudaError_t b1 = cudaMalloc(&h->d_blendPrevW, pixBytes);
        cudaError_t b2 = b1 == cudaSuccess ? cudaMalloc(&h->d_blendNextW, pixBytes)
                                           : cudaErrorUnknown;
        if (b1 != cudaSuccess || b2 != cudaSuccess) {
            setError("cudaMalloc blend weight arrays failed");
            return 1;
        }
    }

    cudaError_t err = cudaMemcpy(h->d_bscan, bscan, bscanBytes, cudaMemcpyHostToDevice);
    if (err == cudaSuccess)
        err = cudaMemcpy(h->d_detx, detx.data(), static_cast<size_t>(nd) * sizeof(float), cudaMemcpyHostToDevice);
    if (err == cudaSuccess)
        err = cudaMemcpy(h->d_dety, dety.data(), static_cast<size_t>(nd) * sizeof(float), cudaMemcpyHostToDevice);
    if (err == cudaSuccess)
        err = cudaMemcpy(h->d_wscale, wscale.data(), static_cast<size_t>(nd) * sizeof(float), cudaMemcpyHostToDevice);
    if (err == cudaSuccess)
        err = cudaMemcpy(h->d_radius, rad.data(), static_cast<size_t>(nd) * sizeof(float), cudaMemcpyHostToDevice);
    if (err == cudaSuccess && sectorMask)
        err = cudaMemcpy(h->d_sectorIdx, sectorIdx.data(), static_cast<size_t>(nd) * sizeof(int), cudaMemcpyHostToDevice);
    if (err == cudaSuccess && blendMask && h->blendSectorWidthDeg != sectorWidthDeg) {
        ring_blend_weights_kernel<<<blocks, threads>>>(
            h->d_pixPhi, total, h->sectorStartRad, sectorFovRad, h->sectorCount, blendRad,
            h->d_blendPrevW, h->d_blendNextW);
        err = cudaDeviceSynchronize();
        if (err == cudaSuccess) h->blendSectorWidthDeg = sectorWidthDeg;
    }
    if (err != cudaSuccess) {
        setError(cudaGetErrorString(err));
        return 1;
    }

    if (h->nlMask) {
        ring_das_kernel_nl<<<blocks, threads>>>(
            h->d_bscan, nt, nd,
            h->d_detx, h->d_dety, h->d_wscale,
            h->d_radius,
            h->d_sectorIdx, h->sectorStartRad, sectorFovRad, h->sectorCount, sectorMask,
            h->d_pixPhi,
            blendMask, h->d_blendPrevW, h->d_blendNextW,
            h->d_xv, h->d_yv, h->nx, h->ny,
            h->pre, h->minDist, h->wExponent,
            h->linear, h->maskOob, h->useFovMask, h->fovRad, h->fovTh0Rad,
            h->nBound, h->preOuter, h->d_preCoeff, h->d_rb2s,
            h->d_acc, h->d_accW, h->cfMask, h->d_accV2,
            h->dmasMask, h->d_accP, h->d_accQ);
    } else {
        ring_das_kernel<<<blocks, threads>>>(
            h->d_bscan, nt, nd,
            h->d_detx, h->d_dety, h->d_wscale,
            h->d_radius,
            h->d_sectorIdx, h->sectorStartRad, sectorFovRad, h->sectorCount, sectorMask,
            h->d_pixPhi,
            blendMask, h->d_blendPrevW, h->d_blendNextW,
            h->d_xv, h->d_yv, h->nx, h->ny,
            h->pre, h->minDist, h->wExponent,
            h->linear, h->maskOob, h->useFovMask, h->fovRad, h->fovTh0Rad,
            h->nBound, h->preOuter, h->d_preCoeff, h->d_rb2s,
            h->d_acc, h->d_accW, h->cfMask, h->d_accV2);
    }
    err = cudaDeviceSynchronize();
    if (err != cudaSuccess) {
        setError(cudaGetErrorString(err));
        return 1;
    }
    ++h->nBlock;
    h->nAlineAccum += nd;
    return 0;
}


extern "C" RING_RECON_CUDA_API int ring_recon_cuda_append(void* handle, const float* bscan,
                                                          int nt, int nd,
                                                          double blockStartDeg,
                                                          double blockSpanDeg) {
    std::vector<float> theta(nd);
    const double stepDeg = blockSpanDeg / static_cast<double>(nd - 1);
    for (int j = 0; j < nd; ++j)
        theta[j] = static_cast<float>(blockStartDeg + j * stepDeg);
    return appendImpl(handle, bscan, nt, nd, theta.data(), nullptr,
                      nullptr, 0.0f, blockStartDeg, blockSpanDeg);
}

extern "C" RING_RECON_CUDA_API int ring_recon_cuda_append_angles(void* handle, const float* bscan,
                                                                 int nt, int nd,
                                                                 const float* thetaDeg) {
    return appendImpl(handle, bscan, nt, nd, thetaDeg, nullptr,
                      nullptr, 0.0f, 0.0, 0.0);
}

extern "C" RING_RECON_CUDA_API int ring_recon_cuda_append_angles_radii(void* handle, const float* bscan,
                                                                       int nt, int nd,
                                                                       const float* thetaDeg,
                                                                       const float* radii) {
    return appendImpl(handle, bscan, nt, nd, thetaDeg, radii,
                      nullptr, 0.0f, 0.0, 0.0);
}

extern "C" RING_RECON_CUDA_API int ring_recon_cuda_append_angles_radii_sector(
        void* handle, const float* bscan, int nt, int nd,
        const float* thetaDeg, const float* radii,
        const float* sectorTheta0Deg, float sectorWidthDeg) {
    return appendImpl(handle, bscan, nt, nd, thetaDeg, radii,
                      sectorTheta0Deg, sectorWidthDeg, 0.0, 0.0);
}

extern "C" RING_RECON_CUDA_API int ring_recon_cuda_get_state(void* handle, float* acc,
                                                             float* accW, int* nBlock) {
    if (!handle) {
        setError("null handle");
        return 1;
    }
    Handle* h = static_cast<Handle*>(handle);
    const size_t npix = static_cast<size_t>(h->nx) * h->ny;
    if (acc) {
        cudaError_t err = cudaMemcpy(acc, h->d_acc, npix * sizeof(float), cudaMemcpyDeviceToHost);
        if (err != cudaSuccess) {
            setError(cudaGetErrorString(err));
            return 1;
        }
    }
    if (accW) {
        cudaError_t err = cudaMemcpy(accW, h->d_accW, npix * sizeof(float), cudaMemcpyDeviceToHost);
        if (err != cudaSuccess) {
            setError(cudaGetErrorString(err));
            return 1;
        }
    }
    if (nBlock) *nBlock = h->nBlock;
    return 0;
}

extern "C" RING_RECON_CUDA_API int ring_recon_cuda_get_state_ex(
        void* handle, float* acc, float* accW, float* accV2,
        float* accP, float* accQ, int* nBlock, int* nAline) {
    if (!handle) {
        setError("null handle");
        return 1;
    }
    Handle* h = static_cast<Handle*>(handle);
    const size_t npix = static_cast<size_t>(h->nx) * h->ny;
    const size_t bytes = npix * sizeof(float);

    struct Copy {
        const Handle* h;
        size_t bytes;
        bool copyTo(float* dst, const float* src) {
            if (!dst) return true;
            if (src) {
                const cudaError_t e = cudaMemcpy(dst, src, bytes, cudaMemcpyDeviceToHost);
                if (e != cudaSuccess) {
                    setError(cudaGetErrorString(e));
                    return false;
                }
            } else {
                std::memset(dst, 0, bytes);
            }
            return true;
        }
    } cp{h, bytes};

    if (!cp.copyTo(acc, h->d_acc) || !cp.copyTo(accW, h->d_accW) ||
        !cp.copyTo(accV2, h->cfMask ? h->d_accV2 : nullptr) ||
        !cp.copyTo(accP, h->nlMask ? h->d_accP : nullptr) ||
        !cp.copyTo(accQ, h->dmasMask ? h->d_accQ : nullptr)) {
        return 1;
    }
    if (nBlock) *nBlock = h->nBlock;
    if (nAline) *nAline = h->nAlineAccum;
    return 0;
}

extern "C" RING_RECON_CUDA_API int ring_recon_cuda_grid_size(void* handle, int* nx, int* ny) {
    if (!handle) return 1;
    Handle* h = static_cast<Handle*>(handle);
    if (nx) *nx = h->nx;
    if (ny) *ny = h->ny;
    return 0;
}

extern "C" RING_RECON_CUDA_API const char* ring_recon_cuda_last_error(void) {
    return g_lastError;
}
