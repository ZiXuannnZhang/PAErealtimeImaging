#include "RingTimevarGpu.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <vector>

namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
double worstAbs=0,worstL2=0;
void compare(const std::vector<double>& actual,const std::vector<double>& expected){
    require(actual.size()==expected.size(),"output length changed");
    double maximum=0,scale=1,norm=0,difference=0;
    for(size_t i=0;i<actual.size();++i){
        require(std::isfinite(actual[i])&&std::isfinite(expected[i]),"nonfinite comparison");
        maximum=std::max(maximum,std::abs(actual[i]-expected[i]));
        scale=std::max(scale,std::abs(expected[i]));
        difference+=(actual[i]-expected[i])*(actual[i]-expected[i]);norm+=expected[i]*expected[i];
    }
    const double relative=std::sqrt(difference)/std::max(1.,std::sqrt(norm));
    worstAbs=std::max(worstAbs,maximum/scale);worstL2=std::max(worstL2,relative);
    if(maximum>1e-8*scale||relative>1e-8){
        std::cerr<<"normalizedAbs="<<maximum/scale<<" relativeL2="<<relative<<'\n';
        throw std::runtime_error("CPU/GPU STFT tolerance exceeded");
    }
}
std::vector<double> input(int samples,int lines){
    std::mt19937 random(20261104);std::uniform_real_distribution<double> uniform(-1,1);
    std::vector<double> x(size_t(samples)*lines);
    for(int line=0;line<lines;++line)for(int j=0;j<samples;++j){
        double value;
        switch(line%6){
        case 0:value=std::sin(j*.071+line)+.4*std::cos(j*1.7);break;
        case 1:value=uniform(random)*3000;break;
        case 2:value=(j==0||j==samples-1||j==samples/2)?10000:0;break;
        case 3:value=1;break;
        case 4:value=(j&1)?-2000:2000;break;
        default:value=0;break;
        }
        x[size_t(line)*samples+j]=value;
    }
    return x;
}
std::vector<double> cpuReference(RingTimevarLowpass& cpu,const std::vector<double>& original,int samples){
    auto result=original;std::string error;
    for(size_t offset=0;offset<result.size();offset+=samples){
        std::vector<double> line(result.begin()+offset,result.begin()+offset+samples);
        require(cpu.apply(line,&error),error.c_str());
        std::copy(line.begin(),line.end(),result.begin()+offset);
    }
    return result;
}
struct NativePlan {
    std::vector<int> index;
    std::vector<double> weight;
    RingTimevarCudaPlan config{};
    explicit NativePlan(const RingTimevarLowpass& cpu,int cap){
        const auto v=cpu.planView();index.resize(v.samples);weight.resize(v.samples);
        for(int i=0;i<v.samples;++i){
            if(v.samples==v.outputLength){index[i]=i;continue;}
            const double t=i/250e6;
            if(t>(v.outputLength-1)/250e6){index[i]=-1;continue;}
            index[i]=std::min(v.outputLength-2,int(t*250e6));
            const double a=index[i]/250e6,b=(index[i]+1)/250e6;
            weight[i]=(t-a)/(b-a);
        }
        config={sizeof(RingTimevarCudaPlan),1,v.samples,v.windowLength,v.fftLength,v.hop,
            v.frames,v.outputLength,cap,v.window,v.mask,v.norm,index.data(),weight.data()};
    }
};
void runCase(RingTimevarGpu& gpu,RingTimevarLowpassConfig cfg,int samples,int lines){
    RingTimevarLowpass cpu;std::string error;cfg.enabled=true;
    require(cpu.configure(cfg,250e6,samples,&error),error.c_str());
    require(gpu.configure(cpu,250e6,lines,&error),error.c_str());
    require(gpu.active()&&gpu.capacity()>=lines,"unexpected GPU capacity");
    const auto original=input(samples,lines),expected=cpuReference(cpu,original,samples);
    auto actual=original;gpu.apply(actual.data(),lines);compare(actual,expected);
    const auto view=cpu.planView();
    for(int line=0;line<lines;++line)for(int i=view.outputLength;i<samples;++i)
        require(actual[size_t(line)*samples+i]==0,"reference zero tail changed");

    // Use the same fixed-capacity plan for partial batches, then full again.
    auto split=original;
    for(int line=0;line<lines;line+=3)gpu.apply(split.data()+size_t(line)*samples,std::min(3,lines-line));
    compare(split,actual);
    auto zeros=std::vector<double>(original.size(),0.);gpu.apply(zeros.data(),lines);
    require(std::all_of(zeros.begin(),zeros.end(),[](double v){return v==0;}),"state crossed batches");
    actual=original;gpu.apply(actual.data(),lines);compare(actual,expected);
    std::cout<<"PASS N="<<samples<<" window="<<cfg.windowLength<<" fft="<<ring_timevar::fftLength(cfg)
             <<" overlap="<<cfg.overlapRatio<<" lines="<<lines<<'\n';
}
void invalidCalls(){
    RingTimevarLowpassConfig cfg;cfg.enabled=true;
    RingTimevarLowpass cpu;std::string error;
    require(cpu.configure(cfg,250e6,7500,&error),error.c_str());
    NativePlan native(cpu,4);
    void* handle=nullptr;
    require(!ring_timevar_cuda_create(&native.config,&handle),ring_timevar_cuda_error());
    struct Cleanup{void* p;~Cleanup(){ring_timevar_cuda_destroy(p);}} cleanup{handle};
    auto x=input(7500,4),out=std::vector<double>(x.size(),123.);
    const auto sentinel=out;
    require(ring_timevar_cuda_apply(handle,x.data(),0,out.data())!=0,"zero batch accepted");
    require(out==sentinel,"bad count changed output");
    require(ring_timevar_cuda_apply(handle,x.data(),5,out.data())!=0,"oversize batch accepted");
    require(out==sentinel,"oversize count changed output");
    for(int position:{0,7488,7499,7500,29999}){
        auto invalid=x;invalid[position]=std::numeric_limits<double>::quiet_NaN();
        const auto before=invalid;
        require(ring_timevar_cuda_apply(handle,invalid.data(),4,invalid.data())!=0,"NaN input accepted");
        require(!std::memcmp(invalid.data(),before.data(),invalid.size()*sizeof(double)),"NaN alias changed on failure");
    }
    auto invalid=x;invalid[1]=std::numeric_limits<double>::infinity();
    require(ring_timevar_cuda_apply(handle,invalid.data(),4,out.data())!=0,"infinite input accepted");
    require(out==sentinel,"nonfinite input changed output");
    invalid.assign(x.size(),1e308);
    require(ring_timevar_cuda_apply(handle,invalid.data(),4,out.data())!=0,"overflowing finite input accepted");
    require(out==sentinel,"nonfinite output was partially published");
    require(!ring_timevar_cuda_apply(handle,x.data(),4,out.data()),ring_timevar_cuda_error());
    compare(out,cpuReference(cpu,x,7500));

    for(int kind=0;kind<9;++kind){
        auto bad=native.config;
        switch(kind){
        case 0:bad.version=2;break;
        case 1:bad.capacity=0;break;
        case 2:bad.capacity=129;break;
        case 3:bad.fftLength=1;break;
        case 4:bad.frames=std::numeric_limits<int>::max();break;
        case 5:bad.outputLength=std::numeric_limits<int>::max();break;
        case 6:bad.hop=0;break;
        case 7:bad.window=nullptr;break;
        default:bad.samples=std::numeric_limits<int>::max();break;
        }
        void* rejected=reinterpret_cast<void*>(uintptr_t(1));
        require(ring_timevar_cuda_create(&bad,&rejected)!=0,"malformed plan accepted");
        require(rejected==nullptr,"failed create published context");
    }
    native.weight[0]=std::numeric_limits<double>::infinity();
    void* rejected=nullptr;
    require(ring_timevar_cuda_create(&native.config,&rejected)!=0,"nonfinite interpolation accepted");
    native.weight[0]=0;native.index[0]=7500;
    require(ring_timevar_cuda_create(&native.config,&rejected)!=0,"out-of-bounds interpolation accepted");
    native.index[0]=0;
    auto bad=native.config;
    std::vector<double> norm(bad.norm,bad.norm+bad.outputLength);norm[0]=0;bad.norm=norm.data();
    require(ring_timevar_cuda_create(&bad,&rejected)!=0,"zero normalization accepted");
    bad=native.config;
    std::vector<double> window(bad.window,bad.window+bad.windowLength);
    window[0]=std::numeric_limits<double>::quiet_NaN();bad.window=window.data();
    require(ring_timevar_cuda_create(&bad,&rejected)!=0,"NaN window accepted");
    std::cout<<"PASS invalid input/configuration output atomicity\n";
}
}
int main(){
    try{
        RingTimevarGpu gpu;
        RingTimevarLowpassConfig c;c.enabled=true;
        runCase(gpu,c,7500,80);
        runCase(gpu,c,2048,7);
        c.windowLength=129;c.fftLength=192;c.overlapRatio=.625;c.filterOrder=2.5;c.transition="linear";
        runCase(gpu,c,2030,9);
        c.windowLength=127;c.fftLength=129;c.overlapRatio=.6;c.p2=800;
        runCase(gpu,c,1057,5);
        c=RingTimevarLowpassConfig{};c.windowLength=32;c.fftLength=32;c.overlapRatio=0;c.p2=1000;
        runCase(gpu,c,1024,6);
        c=RingTimevarLowpassConfig{};c.windowLength=256;c.fftLength=256;
        runCase(gpu,c,7500,8);
        c=RingTimevarLowpassConfig{};c.windowLength=2;c.fftLength=2;c.overlapRatio=0;c.p1=4;c.p2=400;
        runCase(gpu,c,509,6);
        c.windowLength=17;c.fftLength=31;c.overlapRatio=.9;
        runCase(gpu,c,513,7);
        invalidCalls();
        RingTimevarLowpass disabled;std::string error;
        require(disabled.configure(RingTimevarLowpassConfig{},250e6,7500,&error),error.c_str());
        require(gpu.configure(disabled,250e6,80,&error)&&!gpu.active(),"disabled config retained GPU state");
        std::cout<<"PASS GPU STFT worstNormalizedAbs="<<worstAbs<<" worstRelativeL2="<<worstL2<<'\n';
        return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
