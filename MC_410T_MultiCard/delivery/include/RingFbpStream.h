#pragma once
#include <vector>
#include <complex>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace ring_fbp_stream {
// MATLAB frac_delay convention: zero-pad to nextpow2(length+64), shift, real(ifft).
inline void fractionalAdvance(std::vector<double>& x,double late){
    if(late==0)return;
    size_t n=1;while(n<x.size()+64)n*=2;
    std::vector<std::complex<double>> a(n);for(size_t i=0;i<x.size();++i)a[i]=x[i];
    auto fft=[&](bool inverse){
        for(size_t i=1,j=0;i<n;++i){size_t b=n>>1;for(;j&b;b>>=1)j^=b;j^=b;if(i<j)std::swap(a[i],a[j]);}
        for(size_t len=2;len<=n;len*=2){auto root=std::polar(1.,(inverse?2:-2)*3.14159265358979323846/len);
            for(size_t i=0;i<n;i+=len){std::complex<double>w=1.;for(size_t j=0;j<len/2;++j){auto u=a[i+j],v=a[i+j+len/2]*w;a[i+j]=u+v;a[i+j+len/2]=u-v;w*=root;}}}
        if(inverse)for(auto&v:a)v/=double(n);
    };
    fft(false);for(size_t i=0;i<n;++i){double f=(i<=n/2?double(i):double(i)-n)/n;a[i]*=std::polar(1.,2*3.14159265358979323846*f*late);}fft(true);
    for(size_t i=0;i<x.size();++i)x[i]=float(a[i].real());
}
struct Line {std::vector<float> data;float angle=0;};
inline bool adjacent(float previous,float next,double step){
    const double delta=std::remainder(double(next)-previous,360.);
    // Missing block: never median across a gap or count a duplicate angle twice.
    if(std::abs(delta)<std::abs(step)*1e-4)throw std::runtime_error("Duplicate FBP trajectory position");
    return std::abs(delta-step)<=std::abs(step)*.02+1e-4;
}
struct Stream {
    Line left,pending;
    void reset(){left={};pending={};}
    static Line prepared(const Line&a,const Line&b,const Line&c,bool median){
        Line out=b;if(a.data.size()!=b.data.size()||c.data.size()!=b.data.size()||b.data.size()<128)throw std::runtime_error("FBP median length mismatch");
        if(median)for(size_t i=0;i<out.data.size();++i){float x=a.data[i],y=b.data[i],z=c.data[i];out.data[i]=std::max(std::min(x,y),std::min(std::max(x,y),z));}
        double mean=0;for(size_t i=99;i<out.data.size();++i)mean+=out.data[i];mean/=out.data.size()-99;
        for(auto&v:out.data)v-=float(mean);return out;
    }
    void push(Line line,double step,bool median,std::vector<Line>& out){
        if(pending.data.empty()){left=line;pending=std::move(line);return;}
        if(!adjacent(pending.angle,line.angle,step)){out.push_back(prepared(left,pending,pending,median));left=line;pending=std::move(line);return;}
        out.push_back(prepared(left,pending,line,median));left=std::move(pending);pending=std::move(line);
    }
    void finish(bool median,std::vector<Line>&out){if(!pending.data.empty())out.push_back(prepared(left,pending,pending,median));reset();}
};
}
