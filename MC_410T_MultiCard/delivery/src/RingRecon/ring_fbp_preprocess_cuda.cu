#include "ring_fbp_preprocess_cuda.h"
#include "ring_fbp_device.h"
#include "ring_fbp.h"
#include <cufft.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr int threads=256, chunk=256, historySlots=RingFbpHistorySlots;
constexpr size_t MiB=1024u*1024u;
thread_local std::string lastError;
void check(cudaError_t code){
    if(code!=cudaSuccess)throw std::runtime_error(cudaGetErrorString(code));
}
void fftCheck(cufftResult code){
    if(code!=CUFFT_SUCCESS)throw std::runtime_error("cuFFT fractional correction failed ("+std::to_string(int(code))+")");
}
template<class T> struct Buffer {
    T* ptr=nullptr;
    ~Buffer(){if(ptr)cudaFree(ptr);}
    void allocate(size_t n){check(cudaMalloc(&ptr,n*sizeof(T)));}
    void upload(const T* input,size_t n,cudaStream_t stream){
        check(cudaMemcpyAsync(ptr,input,n*sizeof(T),cudaMemcpyHostToDevice,stream));
    }
};
struct Context {
    int samples=0, tile=0, fftLength=0, maxLines=0, validLines=0, bank=0;
    cudaStream_t stream=nullptr;
    cufftHandle fft=0;
    Buffer<float> raw, rows, history[2], ready;
    Buffer<double> signal;
    Buffer<cufftDoubleComplex> spectrum;
    Buffer<RingFbpPreprocessLine> meta;
    Buffer<RingFbpPreprocessTriplet> triplets;
    Buffer<int> invalid, retain;
    ~Context(){
        if(stream)cudaStreamSynchronize(stream);
        if(fft)cufftDestroy(fft);
        if(stream)cudaStreamDestroy(stream);
    }
};
unsigned blocks(size_t n){return static_cast<unsigned>((n+threads-1)/threads);}
__device__ size_t index1d(){return size_t(blockIdx.x)*blockDim.x+threadIdx.x;}
__device__ double rawValue(float x,int invert){
    const double v=isfinite(x)?double(x):0.;
    return invert?-v:v;
}
__global__ void packFractional(cufftDoubleComplex* spectrum,const float* raw,
    const RingFbpPreprocessLine* meta,int samples,int nfft,int lines,int invert,size_t n){
    const size_t id=index1d();
    if(id>=n)return;
    const int line=int(id/nfft), i=int(id%nfft);
    const double x=line<lines&&i<samples&&meta[line].shift!=0
        ?rawValue(raw[size_t(line)*samples+i],invert):0.;
    spectrum[id]=make_cuDoubleComplex(x,0);
}
__global__ void rotateFractional(cufftDoubleComplex* spectrum,const RingFbpPreprocessLine* meta,
    int nfft,int lines){
    const size_t id=index1d();
    if(id>=size_t(nfft)*lines)return;
    const int k=int(id%nfft);
    const double shift=meta[id/nfft].shift;
    if(shift==0)return;
    // MATLAB convention: Nyquist belongs to the positive frequencies.
    const double f=double(k<=nfft/2?k:k-nfft)/nfft;
    double s,c;
    sincos(6.28318530717958647692*f*shift,&s,&c);
    const auto v=spectrum[id];
    spectrum[id]=make_cuDoubleComplex(v.x*c-v.y*s,v.x*s+v.y*c);
}
__global__ void materialize(double* out,const float* raw,const cufftDoubleComplex* spectrum,
    const RingFbpPreprocessLine* meta,int samples,int nfft,int invert,size_t n){
    const size_t id=index1d();
    if(id>=n)return;
    const int line=int(id/samples);
    // Keep the original float rounding after nonzero fractional correction.
    out[id]=meta[line].shift==0?rawValue(raw[id],invert):
        double(float(spectrum[size_t(line)*nfft+id%samples].x/nfft));
}
__global__ void cutAndClip(float* rows,const double* signal,const RingFbpPreprocessLine* meta,
    int samples,size_t n,int* invalid){
    const size_t id=index1d();
    if(id>=n)return;
    const int row=int(id/samples), i=int(id%samples);
    const auto p=meta[row];
    if(i>=samples-p.cut){rows[id]=0;return;}
    const int src=i+p.cut;
    double x=src<p.zeroRows?0:signal[size_t(row)*samples+src];
    if(p.clipEnabled){
        if(x>p.clip)x=p.clip;
        else if(x< -p.clip)x=-p.clip;
    }
    const float value=float(x);
    rows[id]=value;
    if(!isfinite(value))atomicExch(invalid,1);
}
__device__ const float* rowAt(const float* rows,const float* history,int slot,int stride){
    return slot<historySlots?history+size_t(slot)*stride:rows+size_t(slot-historySlots)*stride;
}
__global__ void medianDc(float* dst,const float* rows,const float* history,
    const RingFbpPreprocessTriplet* triplets,int stride,int nt,int median){
    const auto refs=triplets[blockIdx.x];
    const auto* center=rowAt(rows,history,refs.center,stride);
    const auto* left=rowAt(rows,history,refs.left,stride);
    const auto* right=rowAt(rows,history,refs.right,stride);
    auto* out=dst+size_t(blockIdx.x)*nt;
    __shared__ double partial[threads];
    double sum=0;
    for(int i=threadIdx.x;i<nt;i+=threads){
        float v=center[i];
        if(median){
            const float a=left[i], b=right[i];
            const float lo=v<a?v:a, hi=a<v?v:a;
            const float z=b<hi?b:hi;
            v=lo<z?z:lo;
        }
        out[i]=v;
        if(i>=99)sum+=v;
    }
    partial[threadIdx.x]=sum;
    __syncthreads();
    for(int offset=threads/2;offset;offset/=2){
        if(threadIdx.x<offset)partial[threadIdx.x]+=partial[threadIdx.x+offset];
        __syncthreads();
    }
    const float dc=float(partial[0]/(nt-99));
    for(int i=threadIdx.x;i<nt;i+=threads)out[i]-=dc;
}
__global__ void retainRows(float* dst,const float* rows,const float* history,const int* slots,int stride){
    const size_t id=index1d();
    if(id>=size_t(historySlots)*stride)return;
    const int slot=slots[id/stride];
    if(slot>=0)dst[id]=rowAt(rows,history,slot,stride)[id%stride];
}
bool validSlot(const Context& c,int slot){return slot>=0&&slot<historySlots+c.validLines;}
}

extern "C" RING_RECON_CUDA_API const char* ring_fbp_preprocess_error(){return lastError.c_str();}
extern "C" RING_RECON_CUDA_API void ring_fbp_preprocess_destroy(void* p){delete static_cast<Context*>(p);}

extern "C" RING_RECON_CUDA_API int ring_fbp_preprocess_create(int samples,int tile,int maxLines,void** out){
    if(out)*out=nullptr;
    try{
        if(!out||samples<128||samples>1048576||tile<1||tile>128||maxLines<tile||
           size_t(samples)*(size_t(maxLines)+2*historySlots)>128*MiB/sizeof(float))
            throw std::runtime_error("FBP GPU preprocessing exceeds the bounded row workspace");
        auto c=std::make_unique<Context>();
        c->samples=samples;c->tile=tile;c->maxLines=maxLines;
        c->fftLength=1;while(c->fftLength<samples+64)c->fftLength*=2;
        if(size_t(tile)*c->fftLength>64*MiB/sizeof(cufftDoubleComplex))
            throw std::runtime_error("FBP fractional GPU workspace exceeds 64 MiB");
        check(cudaStreamCreateWithFlags(&c->stream,cudaStreamNonBlocking));
        c->raw.allocate(size_t(samples)*tile);
        c->signal.allocate(size_t(samples)*tile);
        c->meta.allocate(tile);
        c->spectrum.allocate(size_t(c->fftLength)*tile);
        c->rows.allocate(size_t(samples)*maxLines);
        for(auto& h:c->history){
            h.allocate(size_t(samples)*historySlots);
            check(cudaMemsetAsync(h.ptr,0,size_t(samples)*historySlots*sizeof(float),c->stream));
        }
        c->ready.allocate(size_t(std::min(samples,8192))*std::min(chunk,maxLines+1));
        c->triplets.allocate(chunk);
        c->retain.allocate(historySlots);
        c->invalid.allocate(1);
        fftCheck(cufftPlan1d(&c->fft,c->fftLength,CUFFT_Z2Z,tile));
        // Exercise the FFT and conversion before opening acquisition.
        std::vector<float> warm(size_t(samples)*tile,0);
        std::vector<RingFbpPreprocessLine> meta(tile,{.25,std::max(0,samples-8192),0,0,0});
        if(ring_fbp_preprocess_block(c.get(),warm.data(),meta.data(),tile,0,nullptr))
            throw std::runtime_error(lastError);
        c->validLines=0;
        *out=c.release();return 0;
    }catch(const std::exception& e){lastError=e.what();return -1;}
}

extern "C" RING_RECON_CUDA_API int ring_fbp_preprocess_block(void* p,const float* raw,
    const RingFbpPreprocessLine* meta,int lines,int invert,void* timevar){
    auto* c=static_cast<Context*>(p);
    cudaStream_t stream=nullptr;
    try{
        if(!c||lines<0||lines>c->maxLines||(lines&&(!raw||!meta)))
            throw std::runtime_error("Invalid FBP GPU preprocessing block");
        c->validLines=0;
        if(!lines)return 0;
        int tile=c->tile;
        if(timevar)tile=std::min(tile,ring_timevar_capacity(timevar,c->samples));
        if(!tile)throw std::runtime_error("Incompatible GPU STFT context");
        for(int i=0;i<lines;++i){
            const auto& m=meta[i];
            if(!std::isfinite(m.shift)||m.cut<0||m.cut>c->samples-128||c->samples-m.cut>8192||
               m.zeroRows<0||m.zeroRows>c->samples||(m.clipEnabled&&!std::isfinite(m.clip)))
                throw std::runtime_error("Invalid GPU FBP line parameters");
        }
        stream=timevar?ring_timevar_stream(timevar):c->stream;
        check(cudaMemsetAsync(c->invalid.ptr,0,sizeof(int),stream));
        for(int first=0;first<lines;first+=tile){
            const int n=std::min(tile,lines-first);
            const size_t count=size_t(n)*c->samples;
            c->raw.upload(raw+size_t(first)*c->samples,count,stream);
            c->meta.upload(meta+first,n,stream);
            if(std::any_of(meta+first,meta+first+n,[](const auto& m){return m.shift!=0;})){
                fftCheck(cufftSetStream(c->fft,stream));
                const size_t spectralCount=size_t(c->fftLength)*c->tile;
                packFractional<<<blocks(spectralCount),threads,0,stream>>>(c->spectrum.ptr,c->raw.ptr,
                    c->meta.ptr,c->samples,c->fftLength,n,invert,spectralCount);
                check(cudaGetLastError());
                fftCheck(cufftExecZ2Z(c->fft,c->spectrum.ptr,c->spectrum.ptr,CUFFT_FORWARD));
                rotateFractional<<<blocks(size_t(n)*c->fftLength),threads,0,stream>>>(c->spectrum.ptr,
                    c->meta.ptr,c->fftLength,n);
                check(cudaGetLastError());
                fftCheck(cufftExecZ2Z(c->fft,c->spectrum.ptr,c->spectrum.ptr,CUFFT_INVERSE));
            }
            materialize<<<blocks(count),threads,0,stream>>>(c->signal.ptr,c->raw.ptr,c->spectrum.ptr,
                c->meta.ptr,c->samples,c->fftLength,invert,count);
            check(cudaGetLastError());
            const double* signal=timevar?ring_timevar_device(timevar,c->signal.ptr,n,c->invalid.ptr):c->signal.ptr;
            cutAndClip<<<blocks(count),threads,0,stream>>>(c->rows.ptr+size_t(first)*c->samples,
                signal,c->meta.ptr,c->samples,count,c->invalid.ptr);
            check(cudaGetLastError());
        }
        int invalid=0;
        check(cudaMemcpyAsync(&invalid,c->invalid.ptr,sizeof(int),cudaMemcpyDeviceToHost,stream));
        check(cudaStreamSynchronize(stream));
        if(invalid)throw std::runtime_error("Nonfinite GPU FBP preprocessed signal");
        c->validLines=lines;return 0;
    }catch(const std::exception& e){if(stream)cudaStreamSynchronize(stream);lastError=e.what();return -1;}
}

extern "C" RING_RECON_CUDA_API int ring_fbp_preprocess_append(void* p,void* fbp,
    const RingFbpPreprocessTriplet* triplets,int nt,int lines,int median,const float* angles,
    double radius,double sector,double width){
    auto* c=static_cast<Context*>(p);
    try{
        if(!c||!fbp||!triplets||!angles||nt<128||nt>std::min(c->samples,8192)||
           lines<1||lines>c->validLines+1)
            throw std::runtime_error("Invalid GPU FBP prepared rows");
        for(int i=0;i<lines;++i){
            const auto& t=triplets[i];
            if(!validSlot(*c,t.left)||!validSlot(*c,t.center)||!validSlot(*c,t.right))
                throw std::runtime_error("Invalid GPU FBP median reference");
        }
        for(int first=0;first<lines;first+=chunk){
            const int n=std::min(chunk,lines-first);
            c->triplets.upload(triplets+first,n,c->stream);
            medianDc<<<n,threads,0,c->stream>>>(c->ready.ptr,c->rows.ptr,c->history[c->bank].ptr,
                c->triplets.ptr,c->samples,nt,median);
            check(cudaGetLastError());
            if(ring_fbp_append_device(fbp,c->ready.ptr,nt,n,angles+first,radius,sector,width,c->stream))
                throw std::runtime_error(ring_fbp_error());
        }
        return 0;
    }catch(const std::exception& e){if(c)cudaStreamSynchronize(c->stream);lastError=e.what();return -1;}
}

extern "C" RING_RECON_CUDA_API int ring_fbp_preprocess_retain(void* p,const int* slots){
    auto* c=static_cast<Context*>(p);
    try{
        if(!c||!slots)throw std::runtime_error("Invalid GPU FBP history");
        for(int i=0;i<historySlots;++i)
            if(slots[i]!=-1&&!validSlot(*c,slots[i]))throw std::runtime_error("Invalid GPU FBP history reference");
        c->retain.upload(slots,historySlots,c->stream);
        retainRows<<<blocks(size_t(historySlots)*c->samples),threads,0,c->stream>>>(c->history[1-c->bank].ptr,
            c->rows.ptr,c->history[c->bank].ptr,c->retain.ptr,c->samples);
        check(cudaGetLastError());
        check(cudaStreamSynchronize(c->stream));
        c->bank=1-c->bank;return 0;
    }catch(const std::exception& e){if(c)cudaStreamSynchronize(c->stream);lastError=e.what();return -1;}
}
