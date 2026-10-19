#include "RingTimevarPreprocess.h"
#ifndef FILTERED_BASELINE_CAPTURE
#include "FilteredDasPrefilter.h"
#endif
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
int main() {try {
    std::ostringstream result;result<<"N,timevar,flags,delay,fnv1a64\n";
    for(int n:{2030,4000,5000,7500})for(bool tv:{false,true})for(int flags=0;flags<8;++flags)for(int delay:{1,7,358,371,448,464}) {
        RingTimevarLowpassConfig c;c.enabled=tv;RingTimevarLowpass filter;std::string e;
        if(!filter.configure(c,250e6,n,&e))throw std::runtime_error(e);
        std::vector<double> x(n);for(int i=0;i<n;++i)x[i]=std::sin(i*.071)+.4*std::cos(i*1.7);
        ringrecon::PreprocessParams pp;pp.systemDelay=delay;pp.dbrRemove=flags&1;pp.delayCut=flags&2;pp.signalImpair=flags&4;
        pp.maskLength=300;pp.dbrmaskExtra=13;pp.imValue=.3;std::vector<float> y;
        if(!ringTimevarPreprocess(filter,x,pp,y,&e))throw std::runtime_error(e);
#ifndef FILTERED_BASELINE_CAPTURE
        FilteredDasConfig off;off.enabled=false;FilteredDasPrefilter fd;
        if(!fd.configure(off,250e6,{},&e))throw std::runtime_error(e);
        std::vector<std::vector<float>> lines;lines.push_back(std::move(y));
        if(!fd.apply(lines,&e)||fd.transforms()||fd.cachedBytes())throw std::runtime_error("OFF did FFT/allocation");
        y=std::move(lines[0]);
#endif
        uint64_t hash=14695981039346656037ULL;
        for(size_t i=0;i<y.size()*4;++i){hash^=reinterpret_cast<unsigned char*>(y.data())[i];hash*=1099511628211ULL;}
        result<<n<<','<<tv<<','<<flags<<','<<delay<<','<<hash<<'\n';
    }
#ifdef FILTERED_BASELINE_CAPTURE
    std::cout<<result.str();
#else
    std::ifstream f(FILTERED_OFF_BASELINE);std::stringstream expected;expected<<f.rdbuf();
    if(!f||result.str()!=expected.str())throw std::runtime_error("eed98f0 OFF byte fingerprints changed");
    std::cout<<"PASS exact eed98f0 OFF timevar OFF/ON 384 fingerprints\n";
#endif
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
