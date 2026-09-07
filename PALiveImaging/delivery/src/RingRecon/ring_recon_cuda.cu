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
    int   linear = 1;
    int   maskOob = 1;
    float wExponent = 1.0f;
    int   nBound = 0;                 // 分层声速边界数
    float preOuter = 0.0f;            // fs/c_outer（分层）或 fs/c（单声速）
    float preCoeff[8] = {0.0f};       // fs*(1/c_i - 1/c_{i+1})
    float rb2s[8] = {0.0f};           // 边界半径平方
    int   sIn[8] = {0};               // 探测器半径 R^2 <= rb2

    float* d_acc = nullptr;
    float* d_accW = nullptr;
    float* d_xv = nullptr;
    float* d_yv = nullptr;
    float* d_bscan = nullptr;
    float* d_detx = nullptr;
    float* d_dety = nullptr;
    float* d_wscale = nullptr;
    float* d_preCoeff = nullptr;
    float* d_rb2s = nullptr;
    int*   d_sIn = nullptr;
    int    d_bscanCap = 0;
    int    d_detCap = 0;
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
    cfg->readMode = 0;          // incremental
    cfg->reconMode = 3;         // both
    cfg->shiftWL2 = 1;
    cfg->wlOffset = 301;
    cfg->alinesPerFrame = 8000;
    cfg->alinesPerBlock = 200;
    cfg->rawColsPerBlock = 200;
    cfg->alineRateHz = 40.0;
    for (int i = 0; i < 8; ++i) cfg->enabledChannels[i] = 1;
    cfg->enabledChannelCount = 8;
    cfg->alinesPerChannelPerBlock = 200;
    cfg->alinesPerChannelPerFrame = 500;
    cfg->sectorStartDeg = 180.0;
    cfg->sectorCcw = 1;
    cfg->triggerWlOdd = 1;

    // ---- sound speed model ----
    cfg->soundSpeedRadiiCount = 0;
    cfg->soundSpeedsCount = 2;
    cfg->soundSpeeds[0] = 1490.0;
    cfg->soundSpeeds[1] = 1540.0;

    // ---- imaging grid and DAS ----
    cfg->fov = 36e-3;
    cfg->gridSize = 0.02e-3;
    cfg->coverageDeg = 360.0;
    cfg->theta0Deg = 0.0;
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
    cfg->refCol = 1001;
    cfg->corrows[0] = 161;
    cfg->corrows[1] = 190;
    cfg->interpFactor = 10.0;
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
    cfg->cRecon[0] = 100.0;
    cfg->cRecon[1] = 100.0;
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

__global__ void ring_das_kernel(
        const float* __restrict__ bscan, int nt, int nd,
        const float* __restrict__ detx, const float* __restrict__ dety,
        const float* __restrict__ wscale,
        const float* __restrict__ xv, const float* __restrict__ yv,
        int nx, int ny,
        float pre, float R, float R2, float minDist, float wExponent,
        int linear, int maskOob, int useFovMask, float fovRad, float fovTh0Rad,
        int nBound, float preOuter,
        const float* __restrict__ preCoeff, const float* __restrict__ rb2s,
        const int* __restrict__ sIn,
        float* __restrict__ acc, float* __restrict__ accW) {
    const int idx = blockIdx.x * blockDim.x + threadIdx.x;
    const int total = nx * ny;
    if (idx >= total) return;

    const int ix = idx / ny;
    const int iy = idx - ix * ny;
    const float x = xv[ix];
    const float y = yv[iy];
    const float r2 = x * x + y * y;
    const float r2mR2 = r2 - R2;

    if (useFovMask) {
        float phi = atan2f(y, x);
        float dang = fmodf(phi - fovTh0Rad, kTwoPiF);
        if (dang < 0.0f) dang += kTwoPiF;
        if (dang > fovRad) return;
    }

    float accv = 0.0f;
    float accwv = 0.0f;
    for (int j = 0; j < nd; ++j) {
        const float proj = x * detx[j] + y * dety[j];
        const float dotp = proj - R2;
        const float dist2 = r2mR2 - 2.0f * dotp;
        const float dist = sqrtf(fmaxf(dist2, 0.0f));
        const float dsafe = fmaxf(dist, minDist);
        // 分层声速走时（与 MATLAB das_recon_circular_gpu_v2 一致）：
        //   tf = d/c_outer + sum_i Li*(1/c_i - 1/c_{i+1})；单声速时退化为 d/c
        float tf = dist * (nBound > 0 ? preOuter : pre);
        for (int bi = 0; bi < nBound; ++bi) {
            const float rb2 = rb2s[bi];
            const bool bothIn = (sIn[bi] != 0) && (r2 <= rb2);
            const float discr4 = dotp * dotp - dist2 * (R2 - rb2);
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
        const float w = wscale[j] * dotp / (R * dsafeP);
        accv += w * vv;
        accwv += fabsf(w);
    }
    acc[idx] += accv;
    accW[idx] += accwv;
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
            h->sIn[i] = (h->R2 <= rb2) ? 1 : 0;
        }
    } else {
        h->preOuter = static_cast<float>(cfg->daqHz / c);
    }
    h->minDist = static_cast<float>(cfg->minDistance > 0.0 ? cfg->minDistance : cfg->gridSize);
    h->useFovMask = (cfg->fovDeg < 359.9999) ? 1 : 0;
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
    if (err == cudaSuccess) err = cudaMalloc(&h->d_sIn, 8 * sizeof(int));
    if (err == cudaSuccess) {
        err = cudaMemcpy(h->d_preCoeff, h->preCoeff, 8 * sizeof(float), cudaMemcpyHostToDevice);
    }
    if (err == cudaSuccess) {
        err = cudaMemcpy(h->d_rb2s, h->rb2s, 8 * sizeof(float), cudaMemcpyHostToDevice);
    }
    if (err == cudaSuccess) {
        err = cudaMemcpy(h->d_sIn, h->sIn, 8 * sizeof(int), cudaMemcpyHostToDevice);
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
    if (h->d_sIn) cudaFree(h->d_sIn);
    delete h;
}

static int appendImpl(void* handle, const float* bscan,
                      int nt, int nd,
                      const float* thetaDeg, double blockStartDeg,
                      double blockSpanDeg) {
    if (!handle || !bscan || nt < 2 || nd < 2) {
        setError("bad append args");
        return 1;
    }
    Handle* h = static_cast<Handle*>(handle);

    // detector positions and weights (with apodization)
    std::vector<float> detx(nd), dety(nd), wscale(nd);
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
        detx[j] = h->R * cosf(th);
        dety[j] = h->R * sinf(th);
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
        cudaError_t e1 = cudaMalloc(&h->d_detx, static_cast<size_t>(nd) * sizeof(float));
        cudaError_t e2 = cudaMalloc(&h->d_dety, static_cast<size_t>(nd) * sizeof(float));
        cudaError_t e3 = cudaMalloc(&h->d_wscale, static_cast<size_t>(nd) * sizeof(float));
        if (e1 != cudaSuccess || e2 != cudaSuccess || e3 != cudaSuccess) {
            setError("cudaMalloc det arrays failed");
            return 1;
        }
        h->d_detCap = nd;
    }

    cudaError_t err = cudaMemcpy(h->d_bscan, bscan, bscanBytes, cudaMemcpyHostToDevice);
    if (err == cudaSuccess)
        err = cudaMemcpy(h->d_detx, detx.data(), static_cast<size_t>(nd) * sizeof(float), cudaMemcpyHostToDevice);
    if (err == cudaSuccess)
        err = cudaMemcpy(h->d_dety, dety.data(), static_cast<size_t>(nd) * sizeof(float), cudaMemcpyHostToDevice);
    if (err == cudaSuccess)
        err = cudaMemcpy(h->d_wscale, wscale.data(), static_cast<size_t>(nd) * sizeof(float), cudaMemcpyHostToDevice);
    if (err != cudaSuccess) {
        setError(cudaGetErrorString(err));
        return 1;
    }

    const int total = h->nx * h->ny;
    const int threads = 256;
    const int blocks = (total + threads - 1) / threads;
    ring_das_kernel<<<blocks, threads>>>(
        h->d_bscan, nt, nd,
        h->d_detx, h->d_dety, h->d_wscale,
        h->d_xv, h->d_yv, h->nx, h->ny,
        h->pre, h->R, h->R2, h->minDist, h->wExponent,
        h->linear, h->maskOob, h->useFovMask, h->fovRad, h->fovTh0Rad,
        h->nBound, h->preOuter, h->d_preCoeff, h->d_rb2s, h->d_sIn,
        h->d_acc, h->d_accW);
    err = cudaDeviceSynchronize();
    if (err != cudaSuccess) {
        setError(cudaGetErrorString(err));
        return 1;
    }
    ++h->nBlock;
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
    return appendImpl(handle, bscan, nt, nd, theta.data(), blockStartDeg, blockSpanDeg);
}

extern "C" RING_RECON_CUDA_API int ring_recon_cuda_append_angles(void* handle, const float* bscan,
                                                                 int nt, int nd,
                                                                 const float* thetaDeg) {
    return appendImpl(handle, bscan, nt, nd, thetaDeg, 0.0, 0.0);
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
