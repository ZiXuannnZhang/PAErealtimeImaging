#ifndef RING_RECON_CUDA_H
#define RING_RECON_CUDA_H

#include <stddef.h>

#if defined(_WIN32)
#  if defined(RING_RECON_CUDA_EXPORTS)
#    define RING_RECON_CUDA_API __declspec(dllexport)
#  else
#    define RING_RECON_CUDA_API __declspec(dllimport)
#  endif
#else
#  define RING_RECON_CUDA_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Full parameter set mirroring the MATLAB main script initialization.
// The frontend UI fills this struct. Unimplemented fields are preserved
// and rejected with a clear error when enabled.
typedef struct RingReconCudaConfig {
    // ---- acquisition / data source ----
    int    dataNum;
    int    sampDepth;
    int    reconDepth;
    int    spaceN;              // reserved
    int    frameNum;            // reserved
    int    numRevolutions;      // reserved
    double daqHz;
    double radius;
    // ---- 多扫描半径配准 ----
    double radiusPerChannel[8]; // 每通道重建半径（m，通道0..7；multiRadius=0 时全部=radiusPerChannel[0]）
    int    multiRadius;         // 1 = 各通道按各自半径重建；0 = 统一半径
    int    spliceMode;          // 1 = 各通道只反投影到各自扇区（仅 multiRadius=1 时有效）
    double spliceBlendDeg;      // 拼接羽化宽度（度，0=硬切；仅 spliceMode=1 时有效）
    int    shiftWL2;
    int    alinesPerFrame;      // legacy dual total per revolution
    int    alinesPerBlock;      // legacy dual total per block

    // ---- 8-channel sector model ----
    int    enabledChannels[8];  // 1 = physical channel participates
    int    enabledChannelCount;
    int    alinesPerChannelPerBlock;
    int    alinesPerChannelPerFrame;
    double sectorStartDeg;      // first selected channel sector start (deg)
    int    sectorCcw;           // 1 = counterclockwise
    int    triggerWlOdd;        // 1 = odd trigger is wavelength 1
    double timeoutResetSec;     // 超时重置：超过该秒数无新触发时，下一触发视为新一圈并重置旧图像（0=关闭）

    // ---- sound speed model ----
    int    soundSpeedRadiiCount;
    double soundSpeedRadii[8];
    int    soundSpeedsCount;
    double soundSpeeds[8];

    // ---- imaging grid and DAS ----
    double fov;
    double gridSize;
    double fovDeg;
    double fovTheta0Deg;
    int    normalizeByW;        // reserved
    int    apodType;            // 0=none, 1=hann, 2=hamming
    double distanceWeightExponent;
    int    interpolation;       // 0=linear, 1=nearest
    double minDistance;         // 0 = auto grid step
    int    maskOutOfRange;

    // ---- preprocessing ----
    int    trigdejit;           // reserved
    int    phaseDecon;          // reserved
    int    gaussfil;            // reserved
    int    gaussfilRowstart;
    int    gaussfilRowsend;
    int    filterLow;           // reserved
    double wLow;
    int    n1;
    int    filterHigh;          // reserved
    double wHigh;
    int    n2;
    int    med;                 // reserved
    int    arcRemove;           // reserved
    int    dbrSigRemove;
    int    maskLength;
    int    singalImpair;
    double imValue[2];
    int    delayCut;
    int    sysDelay[2];
    int    dejitcheck;          // reserved

    // ---- DAS-specific ----
    int    scanArtifactRemove;  // reserved
    double decay;
    int    scanN;
    int    scanK;
    double scanDmin;
    double scanDref;
    double scanWin;

    // ---- display / storage ----
    int    displayRecon;
    int    bscanViewOrig;
    int    bAline1;
    int    bAline2;
    int    paDataView;
    double cBscan[2];
    int    save1;
    int    simulateTiming;
    double refreshPauseSec;
    int    verifyFinal;
    int    curveNum[4];
    int    curveNumCount;
    double figXylim[4];
    double figClim[2];
} RingReconCudaConfig;

RING_RECON_CUDA_API void ring_recon_cuda_set_defaults(RingReconCudaConfig* cfg);
RING_RECON_CUDA_API int ring_recon_cuda_create(const RingReconCudaConfig* cfg, void** out);
RING_RECON_CUDA_API void ring_recon_cuda_destroy(void* handle);
RING_RECON_CUDA_API int ring_recon_cuda_append(void* handle, const float* bscan,
                                               int nt, int nd,
                                               double blockStartDeg,
                                               double blockSpanDeg);
// Per-A-line angle variant (thetaDeg length nd, degrees; sector model).
RING_RECON_CUDA_API int ring_recon_cuda_append_angles(void* handle, const float* bscan,
                                                      int nt, int nd,
                                                      const float* thetaDeg);
// Per-A-line angle + per-A-line radius variant（radii 长度 nd，单位米；
// 多扫描半径配准使用，探测器位置按逐根半径计算）
RING_RECON_CUDA_API int ring_recon_cuda_append_angles_radii(void* handle, const float* bscan,
                                                            int nt, int nd,
                                                            const float* thetaDeg,
                                                            const float* radii);
// Per-A-line angle + per-A-line radius + per-A-line sector start variant.
// sectorTheta0Deg 长度 nd（扇区起点，度）；sectorWidthDeg 为统一扇区宽度（度）。
// 拼接模式：每根 A-line 只反投影到以 sectorTheta0Deg[j] 为起点、宽度 sectorWidthDeg
// 的像素扇区；sectorTheta0Deg=nullptr 或宽度非法（<=0 或 >=360）时退化为
// append_angles_radii 的全局反投影行为。
RING_RECON_CUDA_API int ring_recon_cuda_append_angles_radii_sector(
    void* handle, const float* bscan, int nt, int nd,
    const float* thetaDeg, const float* radii,
    const float* sectorTheta0Deg, float sectorWidthDeg);
RING_RECON_CUDA_API int ring_recon_cuda_get_state(void* handle, float* acc,
                                                  float* accW, int* nBlock);
// B1 成对开关（前提 R2 / 审核 E1）：同时切换反演信号项 2p − 2t·p′ 与
// 立体角权重 R·Δθ·cosα/d²。单一 mode 同时决定两者，结构上不可能只换其一。
//   mode = 0  Das（默认）：信号 p，权重 Δθ·cosα/d —— 全关时逐位保持现有基准
//   mode = 1  Ubp        ：信号 2p − 2t·p′（t 用秒），权重 R·Δθ·cosα/d²
//
// 仅允许在累积开始前调用（对应「参数只在采集/成像停止时应用」）；
// 已 append 过则返回错误码、不静默改变正在累积的图像。ring_recon_cuda_reset 后可再改。
//
// 设计说明：刻意用独立导出函数而不是给 RingReconCudaConfig 加字段 —— 结构体布局不变，
// 旧消费者无需重建；版本错配是链接期响亮失败，而不是静默读错偏移。
RING_RECON_CUDA_API int ring_recon_cuda_set_inversion(void* handle, int mode);
// 按圈清零累积器（d_acc/d_accW）并复位块计数
RING_RECON_CUDA_API int ring_recon_cuda_reset(void* handle);
// 显示快照：acc/accW 归一化到 dn×dn 输出（hostOut 长度 dn*dn，float）。
// step 为块平均步长（方案A：step=1、dn=nx，即全分辨率归一化帧）。
RING_RECON_CUDA_API int ring_recon_cuda_snapshot(void* handle, int dn, int step,
                                                 float* hostOut);
RING_RECON_CUDA_API int ring_recon_cuda_grid_size(void* handle, int* nx, int* ny);
RING_RECON_CUDA_API const char* ring_recon_cuda_last_error(void);

#ifdef __cplusplus
}
#endif

#endif  // RING_RECON_CUDA_H
