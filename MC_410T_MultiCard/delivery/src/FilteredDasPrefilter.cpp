#include "FilteredDasPrefilter.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace {
constexpr double pi = 3.1415926535897932384626433832795;
using Complex = std::complex<double>;
bool fail(std::string* e, const char* message) { if(e) *e=message; return false; }
Complex multiply(Complex a, Complex b) {
    return {a.real()*b.real()-a.imag()*b.imag(), a.real()*b.imag()+a.imag()*b.real()};
}
// Portable radix-2 algorithm, same convention as MATLAB fft/ifft. No external dependency.
struct Plan {
    size_t n=0, left=0;
    std::vector<size_t> reverse;
    std::vector<Complex> roots, h, singleH;
    void prepare(int nt, double fs, const FilteredDasConfig& c) {
        n=filtered_das::fftLength(c,nt); left=(n-nt)/2;
        reverse.resize(n); roots.resize(n/2); h.resize(n); singleH.resize(n);
        for(size_t i=1;i<n;++i) reverse[i]=(reverse[i>>1]>>1)|((i&1)*(n>>1));
        for(size_t i=0;i<n/2;++i) roots[i]=std::polar(1.0,-2*pi*i/n);
        for(size_t k=0;k<n;++k) {
            const double f=(k>n/2?static_cast<double>(k)-n:static_cast<double>(k))*(fs/n);
            const double af=std::abs(f), pass=c.passMhz*1e6, stop=c.stopMhz*1e6;
            const double w=af>=stop?0:af<=pass?1:.5*(1+std::cos(pi*(af-pass)/(stop-pass)));
            h[k]=std::polar(std::pow(af/(c.referenceMhz*1e6),c.order)*w,
                            -(f>0?1:f<0?-1:0)*pi*c.order/2);
            if(k==0||k==n/2) h[k]={};
            // Check representability BEFORE narrowing: an out-of-range float
            // conversion is not a reliable way to detect overflow afterwards.
            const double singleMax=std::numeric_limits<float>::max();
            if(!std::isfinite(h[k].real())||!std::isfinite(h[k].imag())||
               std::abs(h[k].real())>singleMax||std::abs(h[k].imag())>singleMax)
                throw std::domain_error("FilteredDAS response exceeds finite single range");
            // MATLAB casts H to input single BEFORE multiplication by the spectrum.
            singleH[k]={static_cast<float>(h[k].real()),static_cast<float>(h[k].imag())};
            if(!std::isfinite(h[k].real())||!std::isfinite(h[k].imag())||
               !std::isfinite(singleH[k].real())||!std::isfinite(singleH[k].imag()))
                throw std::domain_error("FilteredDAS response overflows double/single");
        }
    }
    void fft(Complex* a, bool inverse) const {
        for(size_t i=0;i<n;++i) if(i<reverse[i]) std::swap(a[i],a[reverse[i]]);
        for(size_t len=2;len<=n;len*=2) {
            const size_t rootStep=n/len;
            for(size_t i=0;i<n;i+=len) for(size_t j=0;j<len/2;++j) {
                const auto r=inverse?std::conj(roots[j*rootStep]):roots[j*rootStep];
                const auto v=multiply(a[i+j+len/2],r), u=a[i+j];
                a[i+j]=u+v; a[i+j+len/2]=u-v;
            }
        }
        if(inverse) for(size_t i=0;i<n;++i) a[i]/=static_cast<double>(n);
    }
    size_t bytes() const { return reverse.size()*sizeof(size_t)+(roots.size()+h.size()+singleH.size())*sizeof(Complex); }
};
}
namespace filtered_das {
size_t fftLength(const FilteredDasConfig& c, int nt) {
    const double target=c.padFactor*nt;
    if(nt<1||!std::isfinite(target)||target>maxFftLength) return 0;
    size_t n=1; while(n<target) n*=2; return n;
}
bool validate(const FilteredDasConfig& c,double fs,int nt,std::string* e,bool depthKnown) {
    if(!std::isfinite(c.order)||c.order<0||c.order>1) return fail(e,"FilteredDAS Order must be in [0,1]");
    if(!std::isfinite(c.referenceMhz)||!std::isfinite(c.passMhz)||!std::isfinite(c.stopMhz)||
       c.referenceMhz<=0||c.referenceMhz>c.passMhz||c.passMhz>=c.stopMhz)
        return fail(e,"FilteredDAS requires 0 < Reference <= Pass < Stop (MHz)");
    if(!std::isfinite(c.padFactor)||c.padFactor<4) return fail(e,"FilteredDAS PadFactor must be finite and >= 4");
    if(c.batchColumns<1) return fail(e,"FilteredDAS BatchColumns must be a positive integer");
    if(!c.enabled) return true; // No Fs/length/resource restriction in bypass.
    if(!std::isfinite(fs)||fs<=0||c.stopMhz>=fs/2e6) return fail(e,"FilteredDAS requires Stop < Fs/2");
    if(depthKnown) {
        const size_t n=fftLength(c,nt);
        if(!n) return fail(e,"FilteredDAS requires nt > 0 and padded FFT <= 262144 samples");
        if(static_cast<size_t>(c.batchColumns)>maxWorkspaceBytes/sizeof(Complex)/n)
            return fail(e,"FilteredDAS batch workspace exceeds 256 MiB");
    }
    return true;
}
bool inputLengths(int depth,bool cut,const int delays[8][2],const int enabled[8],
                  std::vector<int>& lengths,std::string* e) {
    lengths.clear();
    for(int c=0;c<8;++c) if(enabled[c]) for(int w=0;w<2;++w) {
        if(depth<1||(cut&&(delays[c][w]<1||delays[c][w]>depth)))
            return fail(e,"FilteredDAS actual DelayCut must be in [1,sampDepth]");
        const int nt=cut?depth-delays[c][w]+1:depth;
        if(std::find(lengths.begin(),lengths.end(),nt)==lengths.end()) lengths.push_back(nt);
    }
    if(lengths.empty()) return fail(e,"FilteredDAS requires an enabled channel");
    return true;
}
}
struct FilteredDasPrefilter::Impl {
    FilteredDasConfig c;
    std::map<int,Plan> plans;
    std::vector<Complex> work;
    size_t maxBatch=0, batches=0, transforms=0;
    template<class T> bool apply(std::vector<std::vector<T>>& lines,std::string* e) {
        if(!c.enabled) return true; // Strict no scan, cast, allocation or FFT.
        if(lines.empty()) return true;
        if(lines.front().size()>filtered_das::maxFftLength)
            return fail(e,"FilteredDAS actual line length exceeds resource limit");
        const auto it=plans.find(static_cast<int>(lines.front().size()));
        if(it==plans.end()) return fail(e,"FilteredDAS unprepared actual line length");
        const auto& p=it->second;
        // Validate whole group before modifying its lines. Service stages all groups before CUDA/history.
        for(const auto& line:lines) {
            if(line.size()!=lines.front().size()) return fail(e,"FilteredDAS group has unequal actual lengths");
            for(T v:line) if(!std::isfinite(v)) return fail(e,"FilteredDAS nonfinite input");
        }
        const auto& h=std::is_same_v<T,float>?p.singleH:p.h;
        for(size_t start=0;start<lines.size();start+=c.batchColumns) {
            const size_t count=std::min(static_cast<size_t>(c.batchColumns),lines.size()-start);
            maxBatch=std::max(maxBatch,count); ++batches;
            std::fill(work.begin(),work.begin()+p.n*count,Complex{});
            for(size_t j=0;j<count;++j) for(size_t r=0;r<lines[start+j].size();++r)
                work[j*p.n+p.left+r]=lines[start+j][r];
            for(size_t j=0;j<count;++j) {
                auto* a=work.data()+j*p.n; p.fft(a,false);
                for(size_t k=0;k<p.n;++k) a[k]=multiply(a[k],h[k]);
                p.fft(a,true); transforms+=2;
                for(size_t r=0;r<lines[start+j].size();++r) {
                    const double v=a[p.left+r].real();
                    if(!std::isfinite(v)||std::abs(v)>std::numeric_limits<T>::max())
                        return fail(e,"FilteredDAS output exceeds finite input-type range");
                    lines[start+j][r]=static_cast<T>(v);
                }
            }
        }
        return true;
    }
};
FilteredDasPrefilter::FilteredDasPrefilter():m(new Impl) {m->c.enabled=false;}
FilteredDasPrefilter::~FilteredDasPrefilter()=default;
FilteredDasPrefilter::FilteredDasPrefilter(FilteredDasPrefilter&&) noexcept=default;
FilteredDasPrefilter& FilteredDasPrefilter::operator=(FilteredDasPrefilter&&) noexcept=default;
bool FilteredDasPrefilter::configure(const FilteredDasConfig& c,double fs,const std::vector<int>& lengths,std::string* e) {
    if(!filtered_das::validate(c,fs,0,e,false)) return false;
    try {
        auto next=std::make_unique<Impl>(); next->c=c;
        if(c.enabled) {
            if(lengths.empty()) return fail(e,"FilteredDAS actual input lengths required");
            size_t maxN=0, bytes=0;
            for(int nt:lengths) {
                if(!filtered_das::validate(c,fs,nt,e)) return false;
                if(next->plans.count(nt)) continue;
                const size_t n=filtered_das::fftLength(c,nt);
                const size_t planBytes=n*(sizeof(size_t)+2*sizeof(Complex))+n/2*sizeof(Complex);
                if(planBytes>filtered_das::maxPlanBytes-bytes) return fail(e,"FilteredDAS total plans exceed 128 MiB");
                bytes+=planBytes; maxN=std::max(maxN,n);
                next->plans[nt].prepare(nt,fs,c);
            }
            next->work.resize(maxN*static_cast<size_t>(c.batchColumns));
        }
        m=std::move(next); return true;
    } catch(const std::exception& ex) {if(e)*e=ex.what(); return false;}
}
bool FilteredDasPrefilter::apply(std::vector<std::vector<float>>& x,std::string* e) {return m->apply(x,e);}
bool FilteredDasPrefilter::apply(std::vector<std::vector<double>>& x,std::string* e) {return m->apply(x,e);}
size_t FilteredDasPrefilter::workspaceBytes() const {return m->work.size()*sizeof(Complex);}
size_t FilteredDasPrefilter::cachedBytes() const {size_t n=workspaceBytes();for(const auto& p:m->plans)n+=p.second.bytes();return n;}
size_t FilteredDasPrefilter::maxBatchUsed() const {return m->maxBatch;}
size_t FilteredDasPrefilter::batches() const {return m->batches;}
size_t FilteredDasPrefilter::transforms() const {return m->transforms;}
std::vector<Complex> FilteredDasPrefilter::response(int nt,bool single) const {
    const auto it=m->plans.find(nt); if(it==m->plans.end())return {};
    return single?it->second.singleH:it->second.h;
}
