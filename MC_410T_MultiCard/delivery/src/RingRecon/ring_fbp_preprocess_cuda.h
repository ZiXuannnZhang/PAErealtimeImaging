#pragma once
#include "ring_recon_cuda.h"

struct RingFbpPreprocessLine {
    double shift;
    int cut, zeroRows;
    float clip;
    int clipEnabled;
};

struct RingFbpPreprocessTriplet { int left, center, right; };

enum { RingFbpHistorySlots = 32 };

extern "C" {
// Workspace covers one input block (or the buffered timing prefix) plus history.
RING_RECON_CUDA_API int ring_fbp_preprocess_create(int samples, int tile, int maxLines, void**);
RING_RECON_CUDA_API void ring_fbp_preprocess_destroy(void*);
// Raw float data is immutable. Only new lines are replaced; history is unchanged.
RING_RECON_CUDA_API int ring_fbp_preprocess_block(void*, const float*, const RingFbpPreprocessLine*,
                                                int lines, int invert, void* timevar);
// Slots 0..31 refer to the previous history; slots 32+ to new lines in input order.
RING_RECON_CUDA_API int ring_fbp_preprocess_append(void*, void* fbp, const RingFbpPreprocessTriplet*,
                                                 int nt, int lines, int median, const float* angles,
                                                 double radius, double sector, double width);
// Commit retained rows to the next history bank. -1 denotes an unused slot.
RING_RECON_CUDA_API int ring_fbp_preprocess_retain(void*, const int* slots);
RING_RECON_CUDA_API const char* ring_fbp_preprocess_error();
}
