#pragma once
#include <cuda_runtime.h>

// DLL-internal device paths. The existing host ABIs remain unchanged.
const double* ring_timevar_device(void*, const double*, int lines, int* invalid);
cudaStream_t ring_timevar_stream(void*);
int ring_timevar_capacity(void*, int samples);

// Synchronous: signal and angle storage remains valid until return.
int ring_fbp_append_device(void*, const float*, int nt, int nd, const float* angles,
                           double radius, double sector, double width, cudaStream_t);
