#include "RingTimevarLowpass.h"
#include "RingTimevarPreprocess.h"
#include <chrono>
#include <cmath>
#include <iostream>
using Clock=std::chrono::steady_clock;
double elapsed(Clock::time_point t){return std::chrono::duration<double,std::milli>(Clock::now()-t).count();}
int main() {
    std::cout<<"N,channels,mode,phase,iteration,block_ms,design_ms,cache_bytes,lines,checksum\n";
    for(int n:{5000,7500})for(int channels:{4,8})for(int mode=0;mode<3;++mode) {
        RingTimevarLowpassConfig c;c.enabled=mode!=0;
        if(mode==2){c.windowLength=256;c.overlapRatio=.75;c.fftLength=256;}
        RingTimevarLowpass f;std::string e;auto start=Clock::now();
        if(!f.configure(c,250e6,n,&e)){std::cerr<<e;return 1;}const double design=elapsed(start);
        std::vector<double> input(n),line(n);
        for(int i=0;i<n;++i)input[i]=std::sin(i*.071)+.4*std::cos(i*1.7);
        ringrecon::PreprocessParams pp;pp.systemDelay=358;pp.dbrRemove=true;pp.maskLength=300;
        pp.delayCut=true;std::vector<float> output;double checksum=0;
        for(int iteration=-1;iteration<30;++iteration) {
            start=Clock::now();
            for(int col=0;col<channels*200;++col) {
                std::copy(input.begin(),input.end(),line.begin());
                if(!ringTimevarPreprocess(f,line,pp,output,&e)){std::cerr<<e;return 1;}
                checksum+=output[(col*7)%output.size()];
            }
            std::cout<<n<<','<<channels<<','<<mode<<','<<(iteration<0?"cold":"warm")<<','<<iteration<<','
                <<elapsed(start)<<','<<design<<','<<f.cachedBytes()<<','<<channels*200<<','<<checksum<<'\n'<<std::flush;
        }
    }
}
