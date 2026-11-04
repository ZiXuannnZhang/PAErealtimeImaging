#pragma once
#include <string>

namespace ring_sample_window {
// DelayCut uses a one-based first retained sample, independently for both wavelengths.
// This acquisition invariant is shared by DAS and FBP, not owned by a filter.
inline bool validate(int depth, bool cut, const int delays[8][2], const int enabled[8],
                     std::string* error) {
    auto fail=[&](const char* message){if(error)*error=message;return false;};
    if(depth<2)return fail("Ring input requires at least two samples");
    bool any=false;
    for(int c=0;c<8;++c)if(enabled[c]){any=true;for(int w=0;w<2;++w){
        if(cut&&(delays[c][w]<1||delays[c][w]>=depth))
            return fail("Ring DelayCut must retain at least two samples for every channel/wavelength");
    }}
    return any?true:fail("Ring reconstruction requires an enabled channel");
}
}
