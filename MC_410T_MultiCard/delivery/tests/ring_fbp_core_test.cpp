#include "ring_fbp.h"
#include "ring_fbp_math.h"
#include "RingFbpStream.h"
#include <iostream>
#include <fstream>
#include <vector>
#include <stdexcept>
#include <cmath>
static void need(bool x,const char*m){if(!x)throw std::runtime_error(m);}
static std::vector<float> read(const std::string&p){std::ifstream f(p,std::ios::binary|std::ios::ate);need(bool(f),"missing MATLAB fixture");auto n=f.tellg();std::vector<float>x(size_t(n)/4);f.seekg(0);f.read(reinterpret_cast<char*>(x.data()),n);return x;}
static void compare(const std::vector<float>&a,const std::vector<float>&b,double tol){need(a.size()==b.size(),"length");double e=0,s=0,peak=0;
    for(size_t i=0;i<a.size();++i){need(std::isfinite(a[i]),"nonfinite");e+=pow(double(a[i])-b[i],2);s+=double(b[i])*b[i];peak=std::max(peak,std::abs(double(a[i])-b[i]));}
    std::cout<<"relative L2="<<sqrt(e/std::max(s,1e-30))<<" max abs="<<peak<<'\n';need(sqrt(e/std::max(s,1e-30))<tol,"numerical tolerance");}
int main(int argc,char**argv){try{
    need(argc==2,"fixture directory required");std::string dir=argv[1];auto P=read(dir+"/P.f32"),expected=read(dir+"/image.f32"),K=read(dir+"/K.f32");
    std::vector<float> actualK(256*256);for(int t=0;t<256;++t)for(int q=0;q<256;++q)actualK[q+t*256]=float(ring_fbp_math::kernel(q,t,256,1e-7));compare(actualK,K,2e-6);
    auto sx=read(dir+"/shift_input.f32"),se=read(dir+"/shift_expected.f32");std::vector<double>d(sx.begin(),sx.end());ring_fbp_stream::fractionalAdvance(d,1.375);compare(std::vector<float>(d.begin(),d.end()),se,1e-6);
    ring_fbp_math::Medium m;m.nb=1;m.radii[0]=.007;m.c[0]=1490;m.c[1]=1560;m.rho[0]=1000;m.rho[1]=1100;
    auto radial=ring_fbp_math::travel(m,.004,.012,0);need(radial.valid&&fabs(radial.time-(.003/1490+.005/1560))<1e-12,"radial Fermat time");
    const double tp=2*1100.*1560/(1100.*1560+1000.*1490);
    // Paraxial ray transfer through a circular boundary: curvature contributes
    // l1*l2/rb*(1-c2/c1), unlike the accepted planar reference geometry.
    const double J=.003+.005*1560/1490.+.003*.005/.007*(1-1560/1490.);
    need(fabs(radial.weight+tp*sqrt(1490*radial.time/J)/(1560*ring_fbp_math::pi))<1e-12,"normal-incidence transmission/spreading weight");
    // Independent one-interface Fermat minimization in Cartesian coordinates.
    for(double phi:{.2,.5,.9,1.3,2.,2.8,ring_fbp_math::pi}){
        auto f=[&](double a){return sqrt(.004*.004+.007*.007-2*.004*.007*cos(a))/1490+sqrt(.012*.012+.007*.007-2*.012*.007*cos(phi-a))/1560;};
        double lo=0,hi=phi;for(int it=0;it<120;++it){double a=(2*lo+hi)/3,b=(lo+2*hi)/3;if(f(a)<f(b))hi=b;else lo=a;}
        auto v=ring_fbp_math::travel(m,.004,.012,phi);need(v.valid&&fabs(v.time-f((lo+hi)/2))<1e-10,"independent circular Fermat oracle");
    }
    m.c[1]=1490;m.rho[1]=1000;auto v=ring_fbp_math::travel(m,.004,.012,1.1);need(fabs(v.time-sqrt(.004*.004+.012*.012-2*.004*.012*cos(1.1))/1490)<1e-12,"homogeneous reduction");
    RingReconCudaConfig c;ring_recon_cuda_set_defaults(&c);c.fov=.02;c.gridSize=.001;c.radius=.004;c.daqHz=10e6;c.sampDepth=256;c.soundSpeedRadiiCount=0;c.soundSpeedsCount=1;c.soundSpeeds[0]=1490;
    for(int k=0;k<8;++k){c.enabledChannels[k]=k==0;c.radiusPerChannel[k]=.004;}c.enabledChannelCount=1;
    RingFbpConfig b{};b.size=sizeof(b);b.version=1;b.maxNt=256;b.angularStepRad=22.5*ring_fbp_math::pi/180;b.tableAngularSamples=257;for(auto&rho:b.densities)rho=1000;
    void*h=nullptr;need(!ring_fbp_create(&c,&b,&h),ring_fbp_error());std::vector<float>ang(16);for(int j=0;j<16;++j)ang[j]=j*22.5f;
    need(!ring_fbp_append(h,P.data(),256,16,ang.data(),.004,0,360),ring_fbp_error());std::vector<float>one(400),split(400);need(!ring_fbp_snapshot(h,one.data()),ring_fbp_error());compare(one,expected,2e-5);
    need(!ring_fbp_reset(h),"reset");for(int j=0;j<16;j+=2)need(!ring_fbp_append(h,P.data()+256*j,256,2,ang.data()+j,.004,0,360),ring_fbp_error());ring_fbp_snapshot(h,split.data());compare(split,one,2e-6);
    ring_fbp_reset(h);ring_fbp_snapshot(h,split.data());for(float f:split)need(f==0,"reset left residual");ring_fbp_destroy(h);
    // Circular table interpolation versus direct ray tracing (no table in the oracle).
    m.c[1]=1560;m.rho[1]=1100;c.soundSpeedRadiiCount=1;c.soundSpeedsCount=2;
    c.soundSpeedRadii[0]=.007;c.soundSpeeds[1]=1560;b.densities[1]=1100;b.tableAngularSamples=2049;
    c.gridSize=.00025;const int nx=80;
    need(!ring_fbp_create(&c,&b,&h),ring_fbp_error());need(!ring_fbp_prepare(h,256),ring_fbp_error());
    need(!ring_fbp_append(h,P.data(),256,16,ang.data(),.004,0,360),ring_fbp_error());
    std::vector<float> table(nx*nx),direct(nx*nx),q(P.size());ring_fbp_snapshot(h,table.data());ring_fbp_destroy(h);
    for(int j=0;j<16;++j)for(int i=0;i<256;++i){double s=0;for(int t=0;t<256;++t)s+=double(K[i+t*256])*P[t+j*256];q[i+j*256]=float(s);}
    for(int ix=0;ix<nx;++ix)for(int iy=0;iy<nx;++iy){double x=-.01+ix*.02/(nx-1),y=-.01+iy*.02/(nx-1),r=hypot(x,y),sum=0;
        for(int j=0;j<16;++j){double phi=fabs(remainder(atan2(y,x)-ang[j]*ring_fbp_math::pi/180,2*ring_fbp_math::pi));auto ray=ring_fbp_math::travel(m,.004,r,phi);
            if(!ray.valid)continue;double f=ray.time*1e7;int i=int(f);if(i<0||i>=254)continue;
            sum+=ray.weight*(q[j*256+i]*(1-(f-i))+q[j*256+i+1]*(f-i))*.004*b.angularStepRad;}
        direct[ix*nx+iy]=float(sum);}
    std::cout<<"layered lookup vs direct ray: ";compare(table,direct,.03);
    // Median stream output must be invariant to block partition and preserve first/last edge.
    ring_fbp_stream::Stream stream;std::vector<ring_fbp_stream::Line>out;
    for(int j=0;j<5;++j){ring_fbp_stream::Line l;l.angle=float(j);l.data.resize(128);for(int i=0;i<128;++i)l.data[i]=float(i+j);if(j==2)l.data[20]+=100;stream.push(l,1,true,out);}
    stream.finish(true,out);need(out.size()==5,"median dropped/duplicated positions");need(std::abs(out[2].data[20]+92)<1e-6,"median neighbor semantics");
    std::cout<<"FBP core tests PASS\n";return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
