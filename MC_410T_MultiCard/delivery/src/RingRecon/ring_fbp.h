#pragma once
#include "ring_recon_cuda.h"

// Separate, size/version checked ABI. The accepted DAS ABI is unchanged.
typedef struct RingFbpConfig {
    unsigned size;
    unsigned version;
    double densities[8];                 // inner -> outer, kg/m^3
    double angularStepRad;               // actual per-wavelength trajectory step
    int maxNt;
    int tableAngularSamples;
} RingFbpConfig;

#ifdef __cplusplus
extern "C" {
#endif
RING_RECON_CUDA_API int ring_fbp_create(const RingReconCudaConfig*, const RingFbpConfig*, void**);
RING_RECON_CUDA_API void ring_fbp_destroy(void*);
// Build the exact kernel and reserve matrix workspace before acquisition starts.
RING_RECON_CUDA_API int ring_fbp_prepare(void*, int nt);
// One channel/wavelength per call; column-major P, already median/DC/delay processed.
RING_RECON_CUDA_API int ring_fbp_append(void*, const float*, int nt, int nd,
    const float* anglesDeg, double radius, double sectorStartDeg, double sectorWidthDeg);
RING_RECON_CUDA_API int ring_fbp_snapshot(void*, float*);
RING_RECON_CUDA_API int ring_fbp_reset(void*);
RING_RECON_CUDA_API const char* ring_fbp_error(void);
#ifdef __cplusplus
}
#endif
