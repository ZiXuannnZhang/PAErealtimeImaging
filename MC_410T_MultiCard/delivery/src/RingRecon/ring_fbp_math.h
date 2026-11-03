#pragma once
#include <cmath>
#ifdef __CUDACC__
#define FBP_HD __host__ __device__
#else
#define FBP_HD
#endif
namespace ring_fbp_math {
constexpr double pi = 3.14159265358979323846;
FBP_HD inline double clamp(double x,double a,double b){return x<a?a:(x>b?b:x);}
FBP_HD inline double root(double x){return sqrt(x>0?x:0);}
// Integral of one linear hat against 1/sqrt(t^2-T^2), exactly as fc.exact_kernel.
FBP_HD inline double hat(int t,int q,int n,double dt) {
    q=q<0?0:(q>=n?n-1:q);
    const double tau=t*dt,T=q==0?.25*dt:q*dt;
    double lo=fmax(tau-dt,T),hi=fmax(tau,T);
    double v=(root(hi*hi-T*T)-root(lo*lo-T*T)
        -(tau-dt)*(acosh(fmax(hi/T,1.))-acosh(fmax(lo/T,1.))))/dt;
    lo=fmax(tau,T);hi=fmax(tau+dt,T);
    return v+((tau+dt)*(acosh(fmax(hi/T,1.))-acosh(fmax(lo/T,1.)))
        -(root(hi*hi-T*T)-root(lo*lo-T*T)))/dt;
}
FBP_HD inline double kernel(int q,int t,int n,double dt){
    if(q==0)return (hat(t,1,n,dt)-hat(t,0,n,dt))/dt;
    if(q==n-1)return (hat(t,n-1,n,dt)-hat(t,n-2,n,dt))/dt;
    return (hat(t,q+1,n,dt)-hat(t,q-1,n,dt))/(2*dt);
}
struct Medium {int nb=0;double radii[8]={};double c[8]={1490};double rho[8]={1000};};
struct Leg {double phi=0,time=0,deriv=0;bool valid=true;};
struct Ray {double time=0,weight=0;bool valid=false;};
FBP_HD inline int layer(const Medium& m,double r){int k=0;while(k<m.nb&&r>m.radii[k])++k;return k;}
// Radial monotone segment. b = r sin(alpha)/c is conserved by Snell's law.
FBP_HD inline Leg leg(const Medium& m,double a,double z,double b,int first=0,bool turning=false){
    Leg v; double r=a;
    for(int k=first;k<=m.nb;++k){
        double end=k<m.nb?fmin(z,m.radii[k]):z;
        if(end<=r)continue;
        const double bc=b*m.c[k];
        if(bc>r*(1+1e-10)){v.valid=false;return v;}
        const double u=root(r*r-bc*bc),w=root(end*end-bc*bc);
        v.phi+=acos(clamp(bc/end,0,1))-(turning&&r==a?0:acos(clamp(bc/r,0,1)));
        v.time+=(w-u)/m.c[k];
        // At a turning point the moving lower limit is handled separately.
        if(!(turning&&r==a)&&u>1e-15)v.deriv+=m.c[k]*(1/u-1/fmax(w,1e-15));
        else v.deriv-=m.c[k]/fmax(w,1e-15);
        r=end;if(r>=z)break;
    }return v;
}
FBP_HD inline double transmission(const Medium& m,int a,int z,double b){
    double tp=1;const int dir=z>=a?1:-1;
    for(int i=a;i!=z;i+=dir){int j=i+dir;double r=m.radii[dir>0?i:j];
        double co=root(1-pow(b*m.c[i]/r,2)),cn=root(1-pow(b*m.c[j]/r,2));
        double den=m.rho[j]*m.c[j]*co+m.rho[i]*m.c[i]*cn;
        if(den<=0)return 0;tp*=2*m.rho[j]*m.c[j]*co/den;
    }return tp;
}
FBP_HD inline Ray finish(const Medium&m,double R,double r,double phi,double b,
    double time,double derivative,int ls,int lt,int turn,bool targetOut){
    Ray o;if(!(time>0))return o;
    double cs=root(1-pow(b*m.c[ls]/R,2)),ct=root(1-pow(b*m.c[lt]/r,2));
    double J=fabs(r*ct*derivative*R*cs/m.c[ls]);
    if(J<1e-12)return o;
    double tp=turn<0?transmission(m,ls,lt,b):transmission(m,ls,turn,b)*transmission(m,turn,lt,b);
    // Derivative along the sensor's radial normal, expressed at the pixel.
    double projection=(targetOut?ct:-ct)*cos(phi)-(b*m.c[lt]/r)*sin(phi);
    o.time=time;o.weight=-projection/m.c[lt]*tp*sqrt(m.c[ls]*time/J)/pi;o.valid=true;return o;
}
FBP_HD inline Ray travel(const Medium&m,double R,double r,double phi){
    Ray best;phi=fabs(phi);
    if(R<=0||r<=1e-12)return best;
    int ls=layer(m,R),lt=layer(m,r);
    // Analytic homogeneous case avoids any approximation or table iteration.
    bool homogeneous=true;for(int k=1;k<=m.nb;++k)if(m.c[k]!=m.c[0]||m.rho[k]!=m.rho[0])homogeneous=false;
    if(homogeneous){double d=root(R*R+r*r-2*R*r*cos(phi));if(d<1e-12)return best;
        best.time=d/m.c[0];best.weight=-(r*cos(phi)-R)/(d*m.c[0]*pi);best.valid=true;return best;}
    if(phi>pi-1e-9){
        Leg x=leg(m,0,R,0,0,true),y=leg(m,0,r,0,0,true);
        return finish(m,R,r,pi,0,x.time+y.time,x.deriv+y.deriv,ls,lt,0,true);
    }
    double a=fmin(R,r),z=fmax(R,r),bmax=a/m.c[layer(m,a)];
    for(int k=layer(m,a);k<m.nb&&m.radii[k]<z;++k)bmax=fmin(bmax,m.radii[k]/m.c[k+1]);
    // Monotone radial branch, angle increases with b.
    Leg vmax=leg(m,a,z,bmax*(1-1e-12));
    if(vmax.valid&&phi<=vmax.phi){double lo=0,hi=bmax*(1-1e-12);
        for(int it=0;it<48;++it){double b=(lo+hi)/2;Leg v=leg(m,a,z,b);if(v.phi<phi)lo=b;else hi=b;}
        double b=(lo+hi)/2;Leg v=leg(m,a,z,b);
        best=finish(m,R,r,phi,b,v.time,v.deriv,ls,lt,-1,r>=R);
    }
    // Direct rays with one smooth radial turning point. No reflected/multiple-bounce rays.
    for(int k=0;k<= (ls<lt?ls:lt);++k){
        double low=(k?m.radii[k-1]:0)/m.c[k];
        double high=fmin(a,k<m.nb?m.radii[k]:a)/m.c[k];
        for(int j=k+1;j<=ls;++j)high=fmin(high,m.radii[j-1]/m.c[j]);
        for(int j=k+1;j<=lt;++j)high=fmin(high,m.radii[j-1]/m.c[j]);
        low+=1e-10*(high-low);high-=1e-10*(high-low);if(high<=low)continue;
        // Bracket all stationary direct branches; layered media need not be monotone.
        double prev=low;
        Leg va=leg(m,prev*m.c[k],R,prev,k,true),vb=leg(m,prev*m.c[k],r,prev,k,true);
        double fp=va.phi+vb.phi-phi;
        for(int s=1;s<=16;++s){double next=low+(high-low)*s/16;
            va=leg(m,next*m.c[k],R,next,k,true);vb=leg(m,next*m.c[k],r,next,k,true);
            double fn=va.phi+vb.phi-phi;
            if(va.valid&&vb.valid&&fp*fn<=0){double lo=prev,hi=next,fl=fp;
                for(int it=0;it<48;++it){double b=(lo+hi)/2;Leg x=leg(m,b*m.c[k],R,b,k,true),y=leg(m,b*m.c[k],r,b,k,true);
                    double f=x.phi+y.phi-phi;if(fl*f<=0)hi=b;else{lo=b;fl=f;}}
                double b=(lo+hi)/2;Leg x=leg(m,b*m.c[k],R,b,k,true),y=leg(m,b*m.c[k],r,b,k,true);
                Ray o=finish(m,R,r,phi,b,x.time+y.time,x.deriv+y.deriv,ls,lt,k,true);
                if(o.valid&&(!best.valid||o.time<best.time))best=o;
            }prev=next;fp=fn;
        }
    }return best;
}
} // namespace
#undef FBP_HD
