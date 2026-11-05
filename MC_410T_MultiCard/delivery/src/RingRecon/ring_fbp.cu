#include "ring_fbp.h"
#include "ring_fbp_device.h"
#include "ring_fbp_math.h"
#include <cuda_runtime.h>
#include <cublas_v2.h>
#include <algorithm>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
thread_local std::string lastError;
void check(cudaError_t e){if(e!=cudaSuccess)throw std::runtime_error(cudaGetErrorString(e));}
void blas(cublasStatus_t e){if(e!=CUBLAS_STATUS_SUCCESS)throw std::runtime_error("FBP matrix multiplication failed");}
template<class T> struct Buffer {
    T* p=nullptr;size_t n=0;
    ~Buffer(){if(p)cudaFree(p);}
    void resize(size_t count){if(count<=n)return;T* next=nullptr;check(cudaMalloc(&next,count*sizeof(T)));if(p)cudaFree(p);p=next;n=count;}
};
struct Table {Buffer<float2> data;int nr=0,na=0;float dr=0;};
struct Core {
    RingReconCudaConfig cfg{};RingFbpConfig fbp{};ring_fbp_math::Medium medium{};
    int nx=0;bool failed=false; cublasHandle_t blasHandle=nullptr;
    Buffer<float> acc,P,Q;Buffer<float2> angles;std::map<int,std::unique_ptr<Buffer<float>>> kernels;
    std::map<double,std::unique_ptr<Table>> tables;
    ~Core(){if(blasHandle)cublasDestroy(blasHandle);}
};
__global__ void makeKernel(float* K,int n,double dt){
    int q=blockIdx.x*blockDim.x+threadIdx.x,t=blockIdx.y*blockDim.y+threadIdx.y;
    if(q<n&&t<n)K[q+size_t(t)*n]=float(ring_fbp_math::kernel(q,t,n,dt));
}
__global__ void makeTable(float2* dst,int nr,int na,double dr,double R,ring_fbp_math::Medium m){
    int i=blockIdx.x*blockDim.x+threadIdx.x;if(i>=nr*na)return;
    int ir=i/na,ia=i%na;
    auto v=ring_fbp_math::travel(m,R,fmax(ir*dr,1e-10),ia*ring_fbp_math::pi/(na-1));
    dst[i]=v.valid?make_float2(float(v.time),float(v.weight)):make_float2(-1,0);
}
__device__ float angleWrap(float x){x=fmodf(x,360.f);return x<0?x+360:x;}
__device__ float sectorWeight(float phi,float start,float width,float blend){
    if(width>=360)return 1;
    const float u=angleWrap(phi-start);
    if(blend<=0)return u<width?1:0;
    // Same centred cosine feather used by the accepted Ring sector path.
    float d=fminf(u,width-u);
    if(u>width)d=-fminf(u-width,360-u);
    if(d>=blend)return 1;if(d<=-blend)return 0;
    return .5f+.5f*sinf(float(ring_fbp_math::pi)*d/(2*blend));
}
__global__ void backproject(float* acc,const float* Q,int nt,int nd,const float2* angles,
    int nx,float fov,float fs,float R,float ds,const float2* table,int nr,int na,float dr,
    int splice,float secStart,float secWidth,float blend,float fovStart,float fovWidth,
    ring_fbp_math::Medium medium,int homogeneous){
    int id=blockIdx.x*blockDim.x+threadIdx.x;if(id>=nx*nx)return;
    int ix=id/nx,iy=id%nx;float x=-fov/2+ix*fov/(nx-1),y=-fov/2+iy*fov/(nx-1);
    float r=hypotf(x,y),phi=atan2f(y,x),phiDeg=phi*float(180/ring_fbp_math::pi);
    if(fovWidth<360&&angleWrap(phiDeg-fovStart)>fovWidth)return;
    float sw=splice?sectorWeight(phiDeg,secStart,secWidth,blend):1;
    if(sw==0)return;
    float sum=0;
    for(int j=0;j<nd;++j){
        float proj=x*angles[j].x+y*angles[j].y;
        float time,w;
        if(homogeneous){
            float dist=sqrtf(fmaxf(0,R*R+r*r-2*R*proj));
            if(dist<1e-9)continue;time=dist/float(medium.c[0]);
            w=-(proj-R)/(dist*float(medium.c[0]*ring_fbp_math::pi));
        }else{
            float delta=acosf(fminf(1,fmaxf(-1,proj/fmaxf(r,1e-10f))));
            float ri=r/dr,ai=delta*(na-1)/float(ring_fbp_math::pi);
            int ir=min(int(ri),nr-2),ia=min(int(ai),na-2);float u=ri-ir,v=ai-ia;
            if(ir<0||u>1)continue;
            float2 a=table[ir*na+ia],b=table[ir*na+ia+1],c=table[(ir+1)*na+ia],d=table[(ir+1)*na+ia+1];
            if(a.x<0||b.x<0||c.x<0||d.x<0)continue;
            time=(1-u)*((1-v)*a.x+v*b.x)+u*((1-v)*c.x+v*d.x);
            w=(1-u)*((1-v)*a.y+v*b.y)+u*((1-v)*c.y+v*d.y);
        }
        float f=time*fs;int i=int(f);if(i<0||i>=nt-2)continue;
        float q=Q[size_t(j)*nt+i]*(1-(f-i))+Q[size_t(j)*nt+i+1]*(f-i);
        sum+=w*q;
    }
    acc[id]+=sum*ds*sw;
}
bool homogeneous(const Core& h){for(int i=1;i<=h.medium.nb;++i)if(h.medium.c[i]!=h.medium.c[0]||h.medium.rho[i]!=h.medium.rho[0])return false;return true;}
Table* getTable(Core& h,double R){
    if(homogeneous(h))return nullptr;
    auto it=h.tables.find(R);if(it!=h.tables.end())return it->second.get();
    auto t=std::make_unique<Table>();t->dr=float(h.cfg.gridSize/2);t->nr=int(ceil(h.cfg.fov/sqrt(2.)/t->dr))+2;t->na=h.fbp.tableAngularSamples;
    size_t count=size_t(t->nr)*t->na;
    if(count>64*1024*1024)throw std::runtime_error("FBP geometry table exceeds 512 MiB per radius; increase grid size");
    t->data.resize(count);makeTable<<<(count+255)/256,256>>>(t->data.p,t->nr,t->na,t->dr,R,h.medium);
    check(cudaGetLastError());check(cudaDeviceSynchronize());auto* p=t.get();h.tables.emplace(R,std::move(t));return p;
}
Buffer<float>& getKernel(Core& h,int nt){
    auto it=h.kernels.find(nt);if(it!=h.kernels.end())return *it->second;
    auto k=std::make_unique<Buffer<float>>();k->resize(size_t(nt)*nt);
    makeKernel<<<dim3((nt+15)/16,(nt+15)/16),dim3(16,16)>>>(k->p,nt,1/h.cfg.daqHz);
    check(cudaGetLastError());check(cudaDeviceSynchronize());
    auto* result=k.get();h.kernels.emplace(nt,std::move(k));return *result;
}
}
extern "C" RING_RECON_CUDA_API const char* ring_fbp_error(){return lastError.c_str();}
extern "C" RING_RECON_CUDA_API void ring_fbp_destroy(void* p){delete static_cast<Core*>(p);}
extern "C" RING_RECON_CUDA_API int ring_fbp_prepare(void* p,int nt){
    try{auto*h=static_cast<Core*>(p);if(!h||nt<128||nt>h->fbp.maxNt)throw std::runtime_error("Invalid FBP prepare length");
        getKernel(*h,nt);h->P.resize(size_t(nt)*256);h->Q.resize(size_t(nt)*256);h->angles.resize(256);return 0;
    }catch(const std::exception&e){lastError=e.what();return -1;}
}
extern "C" RING_RECON_CUDA_API int ring_fbp_create(const RingReconCudaConfig* cfg,const RingFbpConfig* fb,void** out){
    if(out)*out=nullptr;
    try{
        if(!cfg||!fb||!out||fb->size!=sizeof(RingFbpConfig)||fb->version!=1)throw std::runtime_error("FBP ABI version mismatch");
        if(!(cfg->daqHz>0&&cfg->gridSize>0&&cfg->fov>cfg->gridSize)||fb->maxNt<128||fb->maxNt>8192||!(fb->angularStepRad>0&&fb->angularStepRad<=ring_fbp_math::pi)||fb->tableAngularSamples<65||fb->tableAngularSamples>8193)throw std::runtime_error("Invalid FBP grid/time/trajectory limits (Nt 128..8192)");
        int nb=cfg->soundSpeedRadiiCount;if(nb<0||nb>7||cfg->soundSpeedsCount<nb+1)throw std::runtime_error("Invalid concentric medium");
        auto h=std::make_unique<Core>();h->cfg=*cfg;h->fbp=*fb;h->nx=int(ceil(cfg->fov/cfg->gridSize));
        if(h->nx<2||h->nx>8192)throw std::runtime_error("FBP grid dimension must be 2..8192");
        h->medium.nb=nb;
        for(int i=0;i<=nb;++i){
            if(!std::isfinite(cfg->soundSpeeds[i])||cfg->soundSpeeds[i]<=0||!std::isfinite(fb->densities[i])||fb->densities[i]<=0)throw std::runtime_error("Invalid sound speed/density");
            h->medium.c[i]=cfg->soundSpeeds[i];h->medium.rho[i]=fb->densities[i];
            if(i<nb){if(!std::isfinite(cfg->soundSpeedRadii[i])||cfg->soundSpeedRadii[i]<=0||(i&&cfg->soundSpeedRadii[i]<=cfg->soundSpeedRadii[i-1]))throw std::runtime_error("Invalid layer radius");h->medium.radii[i]=cfg->soundSpeedRadii[i];}
        }
        h->acc.resize(size_t(h->nx)*h->nx);check(cudaMemset(h->acc.p,0,h->acc.n*4));blas(cublasCreate(&h->blasHandle));
        blas(cublasSetMathMode(h->blasHandle,CUBLAS_PEDANTIC_MATH));
        for(int c=0;c<8;++c)if(cfg->enabledChannels[c])getTable(*h,cfg->radiusPerChannel[c]);
        *out=h.release();return 0;
    }catch(const std::exception&e){lastError=e.what();return -1;}
}
static int append(void* p,const float* data,int nt,int nd,const float* angles,double R,double sec,double width,
                  bool device,cudaStream_t stream){
    auto* h=static_cast<Core*>(p);
    float2 trig[256];
    try{
        if(!h||h->failed||!data||!angles||nt<128||nt>h->fbp.maxNt||nd<1||!std::isfinite(R)||R<=0||!std::isfinite(sec)||!std::isfinite(width))throw std::runtime_error("Invalid FBP append or failed context");
        for(int j=0;j<nd;++j)if(!std::isfinite(angles[j]))throw std::runtime_error("Nonfinite FBP angle");
        auto& K=getKernel(*h,nt);
        Table*t=getTable(*h,R);
        blas(cublasSetStream(h->blasHandle,stream));
        // Bound matrix workspace independent of acquisition block size.
        for(int start=0;start<nd;start+=256){int n=std::min(256,nd-start);
            h->Q.resize(size_t(nt)*n);h->angles.resize(n);
            const float* P=data+size_t(start)*nt;
            if(!device){
                h->P.resize(size_t(nt)*n);
                check(cudaMemcpy(h->P.p,P,size_t(nt)*n*sizeof(float),cudaMemcpyHostToDevice));
                P=h->P.p;
            }
            for(int j=0;j<n;++j){double a=angles[start+j]*ring_fbp_math::pi/180;trig[j]=make_float2(float(cos(a)),float(sin(a)));}
            if(device)check(cudaMemcpyAsync(h->angles.p,trig,n*sizeof(float2),cudaMemcpyHostToDevice,stream));
            else check(cudaMemcpy(h->angles.p,trig,n*sizeof(float2),cudaMemcpyHostToDevice));
            float one=1,zero=0;blas(cublasSgemm(h->blasHandle,CUBLAS_OP_N,CUBLAS_OP_N,nt,n,nt,&one,K.p,nt,P,nt,&zero,h->Q.p,nt));
            backproject<<<(h->nx*h->nx+255)/256,256,0,stream>>>(h->acc.p,h->Q.p,nt,n,h->angles.p,h->nx,float(h->cfg.fov),float(h->cfg.daqHz),float(R),float(R*h->fbp.angularStepRad),t?t->data.p:nullptr,t?t->nr:0,t?t->na:0,t?t->dr:0,h->cfg.spliceMode,float(sec),float(width),float(h->cfg.spliceBlendDeg),float(h->cfg.fovTheta0Deg),float(h->cfg.fovDeg),h->medium,homogeneous(*h));
            check(cudaGetLastError());
            if(device)check(cudaStreamSynchronize(stream)); // trig has automatic storage
        }if(!device)check(cudaDeviceSynchronize());return 0;
    }catch(const std::exception&e){if(device)cudaStreamSynchronize(stream);lastError=e.what();if(h)h->failed=true;return -1;}
}
extern "C" RING_RECON_CUDA_API int ring_fbp_append(void* p,const float* data,int nt,int nd,const float* angles,double R,double sec,double width){
    return append(p,data,nt,nd,angles,R,sec,width,false,nullptr);
}
int ring_fbp_append_device(void* p,const float* data,int nt,int nd,const float* angles,double R,double sec,double width,cudaStream_t stream){
    return append(p,data,nt,nd,angles,R,sec,width,true,stream);
}
extern "C" RING_RECON_CUDA_API int ring_fbp_snapshot(void*p,float*out){
    try{auto*h=static_cast<Core*>(p);if(!h||h->failed||!out)throw std::runtime_error("Invalid FBP snapshot");check(cudaMemcpy(out,h->acc.p,h->acc.n*4,cudaMemcpyDeviceToHost));return 0;}catch(const std::exception&e){lastError=e.what();return -1;}
}
extern "C" RING_RECON_CUDA_API int ring_fbp_reset(void*p){
    try{auto*h=static_cast<Core*>(p);if(!h)throw std::runtime_error("Invalid FBP reset");check(cudaMemset(h->acc.p,0,h->acc.n*4));h->failed=false;return 0;}catch(const std::exception&e){lastError=e.what();return -1;}
}
