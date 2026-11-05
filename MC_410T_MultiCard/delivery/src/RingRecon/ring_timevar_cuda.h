#pragma once
#include "ring_recon_cuda.h"
#include <stdint.h>

// Independent versioned C ABI. Coefficients are produced by the accepted CPU
// implementation and copied during create; caller storage is never retained.
struct RingTimevarCudaPlan {
    uint32_t size;
    uint32_t version;
    int samples, windowLength, fftLength, hop, frames, outputLength;
    int capacity; // Maximum lines per apply; caller splits larger blocks.
    const double* window; // windowLength
    const double* mask;   // frames * (fftLength/2+1), frame-major
    const double* norm;   // outputLength, including the reference small-weight rule
    const int* interpolationIndex; // samples; -1 means zero tail
    const double* interpolationWeight; // samples; last sample may have weight 0
};

extern "C" {
RING_RECON_CUDA_API int ring_timevar_cuda_create(const RingTimevarCudaPlan*, void**);
RING_RECON_CUDA_API void ring_timevar_cuda_destroy(void*);
// Input/output are line-major doubles and may alias. Output is unchanged on
// validation/numerical failure; no CPU/GPU partial block may reach reconstruction.
RING_RECON_CUDA_API int ring_timevar_cuda_apply(void*, const double*, int lines, double*);
RING_RECON_CUDA_API const char* ring_timevar_cuda_error();
}
