#include "RingTimevarLowpass.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <stdexcept>

namespace {
constexpr double pi=3.1415926535897932384626433832795;
using Complex=std::complex<double>;
inline Complex multiply(Complex a,Complex b) {
    return {a.real()*b.real()-a.imag()*b.imag(),a.real()*b.imag()+a.imag()*b.real()};
}
// Original portable radix-2/Bluestein FFT; no third-party runtime dependency.
struct Radix2 {
    size_t n=0;
    std::vector<size_t> reverse;
    std::vector<Complex> roots;
    void prepare(size_t size) {
        n=size; reverse.resize(n); roots.resize(n/2);
        for(size_t i=1;i<n;++i) reverse[i]=(reverse[i>>1]>>1)|((i&1)*(n>>1));
        for(size_t i=0;i<n/2;++i) roots[i]=std::polar(1.0,-2*pi*i/n);
    }
    void run(std::vector<Complex>& a,bool inverse=false) const {
        for(size_t i=0;i<n;++i) if(i<reverse[i]) std::swap(a[i],a[reverse[i]]);
        for(size_t len=2;len<=n;len*=2) for(size_t i=0;i<n;i+=len)
            for(size_t j=0;j<len/2;++j) {
                const auto root=inverse?std::conj(roots[j*n/len]):roots[j*n/len];
                const auto v=multiply(a[i+j+len/2],root), u=a[i+j];
                a[i+j]=u+v; a[i+j+len/2]=u-v;
            }
        if(inverse) for(auto& x:a) x/=static_cast<double>(n);
    }
};
struct FFT {
    int n=0; bool radix=false;
    Radix2 plan;
    std::vector<Complex> chirp,kernel,work;
    void prepare(int size) {
        n=size; radix=(n&(n-1))==0;
        if(radix) {plan.prepare(n); return;}
        size_t size2=1;while(size2<static_cast<size_t>(2*n-1)) size2*=2;
        plan.prepare(size2); chirp.resize(n);kernel.assign(size2,{});work.resize(size2);
        for(int i=0;i<n;++i) {
            const double angle=pi*(static_cast<long long>(i)*i%(2*n))/n;
            chirp[i]=std::polar(1.0,-angle);
            kernel[i]=std::conj(chirp[i]);
            if(i)kernel[size2-i]=kernel[i];
        }
        plan.run(kernel);
    }
    void forward(std::vector<Complex>& a) {
        if(radix) {plan.run(a);return;}
        std::fill(work.begin(),work.end(),Complex{});
        for(int i=0;i<n;++i) work[i]=multiply(a[i],chirp[i]);
        plan.run(work);
        for(size_t i=0;i<work.size();++i)work[i]=multiply(work[i],kernel[i]);
        plan.run(work,true);
        for(int i=0;i<n;++i)a[i]=multiply(work[i],chirp[i]);
    }
    void inverse(std::vector<Complex>& a) {
        if(radix) {plan.run(a,true);return;}
        for(auto& v:a)v=std::conj(v);
        forward(a);
        for(auto& v:a)v=std::conj(v)/static_cast<double>(n);
    }
};
bool fail(std::string* e,const char* message){if(e)*e=message;return false;}
}
namespace ring_timevar {
int fftLength(const RingTimevarLowpassConfig& c) {
    if(c.fftLength)return c.fftLength;
    int n=1;while(n<c.windowLength)n*=2;return n;
}
int hop(const RingTimevarLowpassConfig& c) {
    return c.windowLength-std::min(c.windowLength-1,static_cast<int>(std::round(c.windowLength*c.overlapRatio)));
}
bool validate(const RingTimevarLowpassConfig& c,double fs,int samples,std::string* e,bool depthKnown) {
    if(!std::isfinite(fs)||fs<=0)return fail(e,"Fs must be finite and positive");
    if(!std::isfinite(c.fcMinMhz)||!std::isfinite(c.fcMaxMhz)||c.fcMinMhz<=0||
       c.fcMinMhz>c.fcMaxMhz||c.fcMaxMhz>fs/2e6)return fail(e,"require 0 < fcMin <= fcMax <= Fs/2 (MHz)");
    if(c.p1<1||c.p2<=c.p1)return fail(e,"require 1 <= p1 < p2 (uncut 1-based samples)");
    if(c.windowLength<2||c.windowLength>maxFftLength)return fail(e,"WindowLength must be in [2,16384]");
    if(c.fftLength!=0&&(c.fftLength<c.windowLength||c.fftLength>maxFftLength))
        return fail(e,"FFTLength must be auto or in [WindowLength,16384]");
    if(!std::isfinite(c.overlapRatio)||c.overlapRatio<0||c.overlapRatio>=1)
        return fail(e,"OverlapRatio must be in [0,1)");
    if(!std::isfinite(c.filterOrder)||c.filterOrder<=0)return fail(e,"FilterOrder must be finite and positive");
    if(c.transition!="smoothstep"&&c.transition!="linear")return fail(e,"unknown Transition");
    // Lazy UI load defers only depth-dependent checks until acquisition is known.
    // Apply/controller/service always validate the actual full depth.
    if(c.enabled && depthKnown) {
        if(samples<2||samples>maxSamples||c.p2>samples||c.windowLength>samples)
            return fail(e,"enabled filter requires p2/WindowLength <= full sampDepth <= 1048576");
        const size_t frames=1+(samples-c.windowLength)/hop(c);
        if(frames*(fftLength(c)/2+1)>maxMaskElements)return fail(e,"cached mask exceeds 64 MiB resource limit");
        const double maxHz=c.fcMaxMhz*1e6,minHz=c.fcMinMhz*1e6;
        for(size_t f=0;f<frames;++f) {
            const double t=(f*hop(c)+c.windowLength/2.0)/fs;
            const double u=std::clamp((t*fs+1-c.p1)/(c.p2-c.p1),0.0,1.0);
            const double q=c.transition=="linear"?u:3*u*u-2*u*u*u;
            const double fc=maxHz-(maxHz-minHz)*q;
            if(!std::isfinite(fc)||fc<=0)
                return fail(e,"MATLAB cutoff formula produces nonpositive/nonfinite fc (floating-point cancellation)");
        }
    }
    return true;
}
}
struct RingTimevarLowpass::Impl {
    RingTimevarLowpassConfig c;
    double fs=0;
    int samples=0,nfft=0,step=0,frames=0,length=0,bins=0;
    FFT fft;
    std::vector<double> window,mask,norm,temp,result;
    std::vector<Complex> spectrum;
    void prepare() {
        if(!c.enabled)return;
        nfft=ring_timevar::fftLength(c);step=ring_timevar::hop(c);
        frames=1+(samples-c.windowLength)/step;
        length=c.windowLength+(frames-1)*step;bins=nfft/2+1;
        fft.prepare(nfft);spectrum.resize(nfft);window.resize(c.windowLength);
        mask.resize(static_cast<size_t>(frames)*bins);norm.assign(length,0);
        temp.resize(length);result.resize(samples);
        for(int i=0;i<c.windowLength;++i)window[i]=.5-.5*std::cos(2*pi*i/c.windowLength);
        const double maxHz=c.fcMaxMhz*1e6,minHz=c.fcMinMhz*1e6;
        for(int f=0;f<frames;++f) {
            const double t=(f*step+c.windowLength/2.0)/fs;
            double u=std::clamp((t*fs+1-c.p1)/(c.p2-c.p1),0.0,1.0);
            double q=c.transition=="linear"?u:3*u*u-2*u*u*u;
            const double fc=maxHz-(maxHz-minHz)*q;
            for(int k=0;k<bins;++k) {
                const double gain=1/std::sqrt(1+std::pow((k*(fs/nfft))/fc,2*c.filterOrder));
                if(!std::isfinite(gain))throw std::domain_error("nonfinite MATLAB-compatible STFT mask");
                mask[static_cast<size_t>(f)*bins+k]=gain;
            }
            for(int i=0;i<c.windowLength;++i)norm[f*step+i]+=window[i]*window[i];
        }
        // MATLAB R2023a WOLA normalization: small weights are replaced with 1.
        for(auto& v:norm)if(v<frames*std::numeric_limits<double>::epsilon())v=1;
    }
};
RingTimevarLowpass::RingTimevarLowpass():m(new Impl){}
RingTimevarLowpass::~RingTimevarLowpass()=default;
RingTimevarLowpass::RingTimevarLowpass(RingTimevarLowpass&&) noexcept=default;
RingTimevarLowpass& RingTimevarLowpass::operator=(RingTimevarLowpass&&) noexcept=default;
bool RingTimevarLowpass::configure(const RingTimevarLowpassConfig& c,double fs,int samples,std::string* e) {
    if(c.enabled && samples<2)return fail(e,"enabled filter requires actual acquisition depth");
    if(!ring_timevar::validate(c,fs,samples,e))return false;
    try {
        auto next=std::make_unique<Impl>();next->c=c;next->fs=fs;next->samples=samples;next->prepare();
        m=std::move(next);return true;
    } catch(const std::exception& ex){if(e)*e=ex.what();return false;}
}
bool RingTimevarLowpass::apply(std::vector<double>& line,std::string* e) {
    if(!m->c.enabled)return true; // Strict bypass, including no input scan or conversion.
    if(line.size()!=static_cast<size_t>(m->samples))return fail(e,"A-line depth differs from accepted configuration");
    for(double v:line)if(!std::isfinite(v))return fail(e,"nonfinite A-line input");
    std::fill(m->temp.begin(),m->temp.end(),0);
    for(int f=0;f<m->frames;++f) {
        std::fill(m->spectrum.begin(),m->spectrum.end(),Complex{});
        for(int i=0;i<m->c.windowLength;++i)m->spectrum[i]=line[f*m->step+i]*m->window[i];
        m->fft.forward(m->spectrum);
        for(int k=0;k<m->bins;++k)m->spectrum[k]*=m->mask[static_cast<size_t>(f)*m->bins+k];
        for(int k=m->bins;k<m->nfft;++k)m->spectrum[k]=std::conj(m->spectrum[m->nfft-k]);
        m->fft.inverse(m->spectrum);
        for(int i=0;i<m->c.windowLength;++i)m->temp[f*m->step+i]+=m->spectrum[i].real()*m->window[i];
    }
    for(int i=0;i<m->length;++i) {
        m->temp[i]/=m->norm[i];
        if(!std::isfinite(m->temp[i]))return fail(e,"nonfinite iSTFT output");
    }
    if(m->length==m->samples)std::copy(m->temp.begin(),m->temp.end(),m->result.begin());
    else {
        // Preserve the reference length branch and time-based linear interpolation.
        for(int i=0;i<m->samples;++i) {
            const double t=i/m->fs;
            if(t>(m->length-1)/m->fs) {m->result[i]=0;continue;}
            int lo=std::min(m->length-2,static_cast<int>(t*m->fs));
            const double a=lo/m->fs,b=(lo+1)/m->fs;
            const double w=(t-a)/(b-a);
            m->result[i]=m->temp[lo]+w*(m->temp[lo+1]-m->temp[lo]);
        }
    }
    for(double v:m->result)if(!std::isfinite(v))return fail(e,"nonfinite interpolated output");
    std::copy(m->result.begin(),m->result.end(),line.begin());return true;
}
const RingTimevarLowpassConfig& RingTimevarLowpass::config() const{return m->c;}
size_t RingTimevarLowpass::cachedBytes() const {
    return (m->window.size()+m->mask.size()+m->norm.size()+m->temp.size()+m->result.size())*sizeof(double)
        +(m->spectrum.size()+m->fft.kernel.size()+m->fft.chirp.size()+m->fft.work.size()+m->fft.plan.roots.size())*sizeof(Complex)
        +m->fft.plan.reverse.size()*sizeof(size_t);
}
