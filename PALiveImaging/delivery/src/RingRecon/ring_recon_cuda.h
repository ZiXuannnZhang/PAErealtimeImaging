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
    int    readMode;            // 0=incremental, 1=whole
    int    reconMode;           // 1=wl1, 2=wl2, 3=both
    int    shiftWL2;
    int    wlOffset;            // file simulation only
    int    alinesPerFrame;      // legacy dual total per revolution
    int    alinesPerBlock;      // legacy dual total per block
    int    rawColsPerBlock;
    double alineRateHz;

    // ---- 8-channel sector model ----
    int    enabledChannels[8];  // 1 = physical channel participates
    int    enabledChannelCount;
    int    alinesPerChannelPerBlock;
    int    alinesPerChannelPerFrame;
    double sectorStartDeg;      // first selected channel sector start (deg)
    int    sectorCcw;           // 1 = counterclockwise
    int    triggerWlOdd;        // 1 = odd trigger is wavelength 1

    // ---- sound speed model ----
    int    soundSpeedRadiiCount;
    double soundSpeedRadii[8];
    int    soundSpeedsCount;
    double soundSpeeds[8];

    // ---- imaging grid and DAS ----
    double fov;
    double gridSize;
    double coverageDeg;
    double theta0Deg;
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
    int    refCol;              // reserved
    int    corrows[2];          // reserved
    double interpFactor;        // reserved
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
    double cRecon[2];
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
RING_RECON_CUDA_API int ring_recon_cuda_get_state(void* handle, float* acc,
                                                  float* accW, int* nBlock);
RING_RECON_CUDA_API int ring_recon_cuda_grid_size(void* handle, int* nx, int* ny);
RING_RECON_CUDA_API const char* ring_recon_cuda_last_error(void);

#ifdef __cplusplus
}
#endif

#endif  // RING_RECON_CUDA_H