#pragma once
#include "RingTimevarLowpass.h"
#include "ring_recon.h"
// Production service seam. Input is its private double copy of the complete A-line.
inline bool ringTimevarPreprocess(RingTimevarLowpass& filter,std::vector<double>& fullLine,
    const ringrecon::PreprocessParams& pp,std::vector<float>& output,std::string* error=nullptr) {
    if(!filter.apply(fullLine,error))return false;
    output=ringrecon::preprocessBlock(fullLine,static_cast<int>(fullLine.size()),1,pp);
    return true;
}
