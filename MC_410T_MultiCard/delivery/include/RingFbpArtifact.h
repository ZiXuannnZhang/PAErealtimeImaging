#pragma once
#include <vector>
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace ring_fbp_artifact {
inline double median(std::vector<double> a){if(a.empty())throw std::runtime_error("No timing samples");std::sort(a.begin(),a.end());size_t n=a.size();return n%2?a[n/2]:(a[n/2-1]+a[n/2])/2;}
// Direct form of the prototype's zero-padded FFT correlation (1024 samples),
// restricted to the same +/-12 lag search. Positive = measured later than template.
inline double delay(const float* line,const std::vector<double>& t){
    int n=int(t.size());std::vector<double>x(line,line+n);double base=median(x);for(auto&v:x)v-=base;
    double a[25]={};for(int lag=-12;lag<=12;++lag)for(int i=0;i<n;++i){int j=i-lag;if(j>=0&&j<n)a[lag+12]+=x[i]*t[j];}
    int k=int(std::max_element(a,a+25)-a);if(k==0||k==24)throw std::runtime_error("Card timing outside +/-12 sample calibration range");
    double den=a[k-1]-2*a[k]+a[k+1];if(!std::isfinite(den)||fabs(den)<1e-20)throw std::runtime_error("Unusable card timing template/correlation");
    return k-12+.5*(a[k-1]-a[k+1])/den;
}
}
