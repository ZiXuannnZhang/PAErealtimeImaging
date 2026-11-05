#pragma once
#include "RingTimevarLowpass.h"
#include "ring_timevar_cuda.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// One service-owned batch context; CPU remains the design and reference path.
class RingTimevarGpu {
public:
    RingTimevarGpu()=default;
    ~RingTimevarGpu(){ring_timevar_cuda_destroy(m_handle);}
    RingTimevarGpu(const RingTimevarGpu&)=delete;
    RingTimevarGpu& operator=(const RingTimevarGpu&)=delete;
    RingTimevarGpu(RingTimevarGpu&& other) noexcept {swap(other);}
    RingTimevarGpu& operator=(RingTimevarGpu&& other) noexcept {
        RingTimevarGpu next(std::move(other));swap(next);return *this;
    }
    bool configure(const RingTimevarLowpass& cpu,double fs,int blockLines,std::string* error){
        try{
        RingTimevarGpu next;
        if(!cpu.config().enabled){swap(next);return true;}
        if(!std::isfinite(fs)||fs<=0||blockLines<1){
            if(error)*error="Invalid GPU STFT sample rate or batch size";return false;
        }
        auto v=cpu.planView();
        const size_t spectralBytes=size_t(v.frames)*v.fftLength*2*sizeof(double);
        const int cap=spectralBytes?int(std::min<size_t>(128,(64u*1024*1024)/spectralBytes)):0;
        if(cap<1||blockLines<1){if(error)*error="GPU STFT spectrum exceeds bounded workspace";return false;}
        next.m_capacity=std::min(cap,blockLines);
        std::vector<int> index(v.samples);std::vector<double> weight(v.samples);
        for(int i=0;i<v.samples;++i){
            if(v.outputLength==v.samples){index[i]=i;weight[i]=0;continue;}
            const double t=i/fs;
            if(t>(v.outputLength-1)/fs){index[i]=-1;continue;}
            const int lo=std::min(v.outputLength-2,int(t*fs));
            const double a=lo/fs,b=(lo+1)/fs;
            index[i]=lo;weight[i]=(t-a)/(b-a);
        }
        RingTimevarCudaPlan plan{sizeof(RingTimevarCudaPlan),1,v.samples,v.windowLength,
            v.fftLength,v.hop,v.frames,v.outputLength,next.m_capacity,
            v.window,v.mask,v.norm,index.data(),weight.data()};
        if(ring_timevar_cuda_create(&plan,&next.m_handle)){
            if(error)*error=ring_timevar_cuda_error();return false;
        }
        swap(next);return true;
        }catch(const std::exception& e){if(error)*error=e.what();return false;}
    }
    bool active() const{return m_handle!=nullptr;}
    void* handle() const{return m_handle;}
    int capacity() const{return m_capacity;}
    void apply(double* data,int lines){
        if(ring_timevar_cuda_apply(m_handle,data,lines,data))
            throw std::runtime_error(ring_timevar_cuda_error());
    }
private:
    void swap(RingTimevarGpu& o) noexcept{std::swap(m_handle,o.m_handle);std::swap(m_capacity,o.m_capacity);}
    void* m_handle=nullptr;int m_capacity=0;
};
