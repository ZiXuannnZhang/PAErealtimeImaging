#include <cstdio>
#include <cmath>
static const double PI=3.14159265358979323846;
int main(){
    const double R=6.57e-3, arc=10.0*PI/180.0;
    struct P{double x,y;const char*n;};
    P ps[3]={{7.794e-3,4.5e-3,"环外同侧 A(ρ=9)"},{-11.258e-3,6.5e-3,"环外对侧 B(ρ=13)"},{0,-17e-3,"环外远端 C(ρ=17)"}};
    std::printf("%-22s %-12s %-12s %-10s\n","像素","探测器方位","cosα","w 符号");
    for(auto&p:ps){
      double rho=std::sqrt(p.x*p.x+p.y*p.y), ang=std::atan2(p.y,p.x)*180/PI;
      for(int k=0;k<4;k++){
        double th=(ang + (k==0?0:(k==1?180:(k==2?90:-90))))*PI/180;
        double sx=R*std::cos(th), sy=R*std::sin(th);
        double dx=p.x-sx, dy=p.y-sy, d=std::sqrt(dx*dx+dy*dy);
        double cosA=(R-(p.x*std::cos(th)+p.y*std::sin(th)))/d;
        double w=arc*cosA/d;
        const char* label=(k==0?"同侧(最近)":k==1?"对侧(最远)":k==2?"+90°":"-90°");
        std::printf("%-22s %-12s %-12.4f %-10s\n",p.n,label,cosA,w>0?"正":(w<0?"负":"0"));
      }
    }
    return 0;
}
