#include "ring_timevar_cuda.h"
#include <cuda_runtime.h>
#include <cufft.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
thread_local std::string lastError;
void check(cudaError_t code) {
    if(code!=cudaSuccess)throw std::runtime_error(cudaGetErrorString(code));
}
void fftCheck(cufftResult code) {
    if(code!=CUFFT_SUCCESS)throw std::runtime_error("cuFFT STFT failure ("+std::to_string(int(code))+")");
}
template<class T> struct Buffer {
    T* ptr=nullptr;
    ~Buffer(){if(ptr)cudaFree(ptr);}
    void allocate(size_t count){check(cudaMalloc(reinterpret_cast<void**>(&ptr),count*sizeof(T)));}
    void upload(const T* src,size_t count,cudaStream_t stream){
        allocate(count);check(cudaMemcpyAsync(ptr,src,count*sizeof(T),cudaMemcpyHostToDevice,stream));
    }
};
struct Context {
    int samples=0,windowLength=0,nfft=0,hop=0,frames=0,length=0,capacity=0;
    size_t spectra=0;
    cudaStream_t stream=nullptr;
    cufftHandle fft=0;
    bool failed=false;
    Buffer<double> input,window,mask,norm,normalized,output,weight;
    Buffer<int> index,invalid;
    Buffer<cufftDoubleComplex> spectrum;
    std::vector<double> hostOutput;
    ~Context(){
        if(stream)cudaStreamSynchronize(stream);
        if(fft)cufftDestroy(fft);
        if(stream)cudaStreamDestroy(stream);
    }
};
unsigned blocks(size_t n){return static_cast<unsigned>((n+255)/256);}

__global__ void validateInput(const double* input,size_t count,int* invalid){
    const size_t id=size_t(blockIdx.x)*blockDim.x+threadIdx.x;
    if(id<count&&!isfinite(input[id]))atomicExch(invalid,1);
}
__global__ void packWindows(cufftDoubleComplex* out,const double* input,const double* window,
                            int samples,int win,int nfft,int hop,int frames,int lines,size_t count){
    const size_t id=size_t(blockIdx.x)*blockDim.x+threadIdx.x;
    if(id>=count)return;
    const int k=int(id%nfft),f=int((id/nfft)%frames),line=int(id/(size_t(nfft)*frames));
    double value=0;
    if(line<lines&&k<win){
        const double x=input[size_t(line)*samples+f*hop+k];
        // Invalid inputs are rejected before publication; keep NaNs out of cuFFT.
        value=isfinite(x)?__dmul_rn(x,window[k]):0;
    }
    out[id]=make_cuDoubleComplex(value,0);
}
__global__ void maskPositive(cufftDoubleComplex* spectrum,const double* mask,int nfft,int frames,
                            int bins,size_t count){
    const size_t id=size_t(blockIdx.x)*blockDim.x+threadIdx.x;
    if(id>=count)return;
    const int k=int(id%bins),frame=int((id/bins)%frames);
    const size_t pos=(id/bins)*nfft+k;
    const double gain=mask[size_t(frame)*bins+k];
    const auto x=spectrum[pos];
    spectrum[pos]=make_cuDoubleComplex(__dmul_rn(x.x,gain),__dmul_rn(x.y,gain));
}
__global__ void restoreNegative(cufftDoubleComplex* spectrum,int nfft,int bins,size_t count){
    const size_t id=size_t(blockIdx.x)*blockDim.x+threadIdx.x;
    if(id>=count)return;
    const int negatives=nfft-bins,k=bins+int(id%negatives);
    const size_t start=(id/negatives)*nfft;
    const auto value=spectrum[start+nfft-k];
    spectrum[start+k]=make_cuDoubleComplex(value.x,-value.y);
}
__global__ void overlapAdd(const cufftDoubleComplex* spectrum,const double* window,
                          const double* norm,double* normalized,int win,int nfft,int hop,
                          int frames,int length,size_t count,int* invalid){
    const size_t id=size_t(blockIdx.x)*blockDim.x+threadIdx.x;
    if(id>=count)return;
    const int i=int(id%length),line=int(id/length);
    const int first=i>=win?(i-win)/hop+1:0;
    const int last=min(i/hop,frames-1);
    double sum=0;
    // CPU reference visits frames in increasing order. This gather also avoids
    // atomics and their nondeterministic accumulation order at overlapping bins.
    for(int frame=first;frame<=last;++frame){
        const int k=i-frame*hop;
        const double inverse=__ddiv_rn(spectrum[(size_t(line)*frames+frame)*nfft+k].x,double(nfft));
        sum=__dadd_rn(sum,__dmul_rn(inverse,window[k]));
    }
    const double value=__ddiv_rn(sum,norm[i]);
    normalized[id]=value;
    if(!isfinite(value))atomicExch(invalid,1);
}
__global__ void interpolate(const double* normalized,const int* index,const double* weight,
                            double* output,int samples,int length,size_t count,int* invalid){
    const size_t id=size_t(blockIdx.x)*blockDim.x+threadIdx.x;
    if(id>=count)return;
    const int i=int(id%samples),line=int(id/samples),lo=index[i];
    double value=0;
    if(length==samples)value=normalized[id]; // Reference's exact-length copy branch.
    else if(lo>=0){
        const size_t offset=size_t(line)*length+lo;
        const double a=normalized[offset];
        value=lo==length-1?a:__dadd_rn(a,__dmul_rn(weight[i],__dsub_rn(normalized[offset+1],a)));
    }
    output[id]=value;
    if(!isfinite(value))atomicExch(invalid,1);
}

void validatePlan(const RingTimevarCudaPlan& p){
    if(p.size!=sizeof(RingTimevarCudaPlan)||p.version!=1)
        throw std::invalid_argument("GPU STFT ABI version mismatch");
    if(p.samples<2||p.samples>1048576||p.windowLength<2||p.windowLength>16384||
       p.windowLength>p.samples||p.fftLength<p.windowLength||p.fftLength>16384||
       p.hop<1||p.hop>p.windowLength||p.capacity<1||p.capacity>128||
       p.frames!=1+(p.samples-p.windowLength)/p.hop||
       p.outputLength!=p.windowLength+(p.frames-1)*p.hop)
        throw std::invalid_argument("Invalid GPU STFT dimensions");
    const size_t transforms=size_t(p.frames)*p.capacity;
    const size_t spectralCount=transforms*p.fftLength;
    const size_t masks=size_t(p.frames)*(p.fftLength/2+1);
    if(transforms>size_t(std::numeric_limits<int>::max())||
       spectralCount>(64u*1024*1024)/sizeof(cufftDoubleComplex)||masks>8u*1024*1024)
        throw std::invalid_argument("GPU STFT workspace limit exceeded");
    if(!p.window||!p.mask||!p.norm||!p.interpolationIndex||!p.interpolationWeight)
        throw std::invalid_argument("Null GPU STFT coefficients");
    for(int i=0;i<p.windowLength;++i)if(!std::isfinite(p.window[i]))
        throw std::invalid_argument("Nonfinite GPU STFT window");
    for(size_t i=0;i<masks;++i)if(!std::isfinite(p.mask[i]))
        throw std::invalid_argument("Nonfinite GPU STFT frequency mask");
    for(int i=0;i<p.outputLength;++i)if(!std::isfinite(p.norm[i])||p.norm[i]<=0)
        throw std::invalid_argument("Invalid GPU STFT normalization");
    for(int i=0;i<p.samples;++i){
        const int index=p.interpolationIndex[i];
        if(index< -1||index>=p.outputLength||!std::isfinite(p.interpolationWeight[i])||
           (index==p.outputLength-1&&p.interpolationWeight[i]!=0)||
           (p.outputLength==p.samples&&(index!=i||p.interpolationWeight[i]!=0)))
            throw std::invalid_argument("Invalid GPU STFT interpolation");
    }
}
}

extern "C" RING_RECON_CUDA_API const char* ring_timevar_cuda_error(){return lastError.c_str();}
extern "C" RING_RECON_CUDA_API void ring_timevar_cuda_destroy(void* p){delete static_cast<Context*>(p);}
extern "C" RING_RECON_CUDA_API int ring_timevar_cuda_create(const RingTimevarCudaPlan* p,void** out){
    if(out)*out=nullptr;
    try{
        if(!p||!out)throw std::invalid_argument("Null GPU STFT configuration/output");
        validatePlan(*p);
        auto c=std::make_unique<Context>();
        c->samples=p->samples;c->windowLength=p->windowLength;c->nfft=p->fftLength;
        c->hop=p->hop;c->frames=p->frames;c->length=p->outputLength;c->capacity=p->capacity;
        c->spectra=size_t(c->capacity)*c->frames*c->nfft;
        const size_t signalCount=size_t(c->capacity)*c->samples;
        c->hostOutput.resize(signalCount);
        check(cudaStreamCreateWithFlags(&c->stream,cudaStreamNonBlocking));
        c->window.upload(p->window,c->windowLength,c->stream);
        c->mask.upload(p->mask,size_t(c->frames)*(c->nfft/2+1),c->stream);
        c->norm.upload(p->norm,c->length,c->stream);
        c->index.upload(p->interpolationIndex,c->samples,c->stream);
        c->weight.upload(p->interpolationWeight,c->samples,c->stream);
        c->input.allocate(signalCount);c->output.allocate(signalCount);
        c->normalized.allocate(size_t(c->capacity)*c->length);
        c->spectrum.allocate(c->spectra);c->invalid.allocate(1);
        fftCheck(cufftPlan1d(&c->fft,c->nfft,CUFFT_Z2Z,c->capacity*c->frames));
        fftCheck(cufftSetStream(c->fft,c->stream));
        check(cudaStreamSynchronize(c->stream));
        // Exercise the complete zero batch during preparation, before the host
        // opens acquisition submission; include lazy CUDA/cuFFT initialization.
        if(ring_timevar_cuda_apply(c.get(),c->hostOutput.data(),c->capacity,c->hostOutput.data()))
            throw std::runtime_error(lastError);
        *out=c.release();lastError.clear();return 0;
    }catch(const std::exception& e){lastError=e.what();return -1;}
    catch(...){lastError="Unknown GPU STFT preparation failure";return -1;}
}
extern "C" RING_RECON_CUDA_API int ring_timevar_cuda_apply(void* opaque,const double* input,int lines,double* output){
    auto* c=static_cast<Context*>(opaque);
    if(!c||c->failed||!input||!output||lines<1||lines>c->capacity){
        lastError="Invalid GPU STFT apply or failed context";return -1;
    }
    try{
        const size_t signalCount=size_t(lines)*c->samples;
        check(cudaMemsetAsync(c->invalid.ptr,0,sizeof(int),c->stream));
        check(cudaMemcpyAsync(c->input.ptr,input,signalCount*sizeof(double),cudaMemcpyHostToDevice,c->stream));
        validateInput<<<blocks(signalCount),256,0,c->stream>>>(c->input.ptr,signalCount,c->invalid.ptr);
        check(cudaGetLastError());
        packWindows<<<blocks(c->spectra),256,0,c->stream>>>(c->spectrum.ptr,c->input.ptr,c->window.ptr,
            c->samples,c->windowLength,c->nfft,c->hop,c->frames,lines,c->spectra);
        check(cudaGetLastError());
        fftCheck(cufftExecZ2Z(c->fft,c->spectrum.ptr,c->spectrum.ptr,CUFFT_FORWARD));
        const int bins=c->nfft/2+1;
        const size_t positiveCount=size_t(c->capacity)*c->frames*bins;
        maskPositive<<<blocks(positiveCount),256,0,c->stream>>>(c->spectrum.ptr,c->mask.ptr,c->nfft,c->frames,bins,positiveCount);
        check(cudaGetLastError());
        const size_t negativeCount=size_t(c->capacity)*c->frames*(c->nfft-bins);
        if(negativeCount){
            restoreNegative<<<blocks(negativeCount),256,0,c->stream>>>(c->spectrum.ptr,c->nfft,bins,negativeCount);
            check(cudaGetLastError());
        }
        fftCheck(cufftExecZ2Z(c->fft,c->spectrum.ptr,c->spectrum.ptr,CUFFT_INVERSE));
        const size_t normalizedCount=size_t(lines)*c->length;
        overlapAdd<<<blocks(normalizedCount),256,0,c->stream>>>(c->spectrum.ptr,c->window.ptr,c->norm.ptr,
            c->normalized.ptr,c->windowLength,c->nfft,c->hop,c->frames,c->length,normalizedCount,c->invalid.ptr);
        check(cudaGetLastError());
        interpolate<<<blocks(signalCount),256,0,c->stream>>>(c->normalized.ptr,c->index.ptr,c->weight.ptr,
            c->output.ptr,c->samples,c->length,signalCount,c->invalid.ptr);
        check(cudaGetLastError());
        int invalid=0;
        check(cudaMemcpyAsync(&invalid,c->invalid.ptr,sizeof(invalid),cudaMemcpyDeviceToHost,c->stream));
        check(cudaStreamSynchronize(c->stream));
        if(invalid){lastError="Nonfinite GPU STFT input or output";return -1;}
        // Stage the complete result before committing caller memory, including
        // aliasing input/output. A failed CUDA copy never publishes a partial line.
        check(cudaMemcpyAsync(c->hostOutput.data(),c->output.ptr,signalCount*sizeof(double),cudaMemcpyDeviceToHost,c->stream));
        check(cudaStreamSynchronize(c->stream));
        std::memcpy(output,c->hostOutput.data(),signalCount*sizeof(double));
        lastError.clear();return 0;
    }catch(const std::exception& e){c->failed=true;lastError=e.what();return -1;}
    catch(...){c->failed=true;lastError="Unknown GPU STFT execution failure";return -1;}
}
