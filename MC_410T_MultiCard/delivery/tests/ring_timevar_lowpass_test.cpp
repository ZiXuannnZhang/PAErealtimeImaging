#include "RingTimevarLowpass.h"
#include "RingTimevarLowpassSettings.h"
#include "RingTimevarPreprocess.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTemporaryDir>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <limits>
#include <algorithm>

void require(bool ok,const char* e){if(!ok)throw std::runtime_error(e);}
std::vector<std::vector<double>> csv(const QString& path) {
    std::ifstream f(path.toStdString());require(f.good(),"missing fixture");
    std::string row;std::vector<std::vector<double>> rows;
    while(std::getline(f,row)) {std::istringstream s(row);std::string value;std::vector<double> v;
        while(std::getline(s,value,','))v.push_back(std::stod(value));rows.push_back(v);}
    return rows;
}
void compare(const std::vector<double>& x,const std::vector<double>& y,double tol,double& worst) {
    require(x.size()==y.size(),"size mismatch");double abs=0,scale=1,l2=0,ref=0;
    for(size_t i=0;i<x.size();++i) {
        require(std::isfinite(x[i])&&std::isfinite(y[i]),"nonfinite comparison");
        abs=std::max(abs,std::abs(x[i]-y[i]));scale=std::max(scale,std::abs(y[i]));
        l2+=(x[i]-y[i])*(x[i]-y[i]);ref+=y[i]*y[i];
    }
    worst=std::max(worst,abs/scale);
    if(abs>tol*scale||std::sqrt(l2)/std::max(1.,std::sqrt(ref))>tol) {
        std::cerr<<"maxAbs="<<abs<<" scale="<<scale<<" L2="<<std::sqrt(l2)/std::max(1.,std::sqrt(ref))<<'\n';
        throw std::runtime_error("MATLAB tolerance exceeded");
    }
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        QFile manifest(QString(TIMEVAR_FIXTURES)+"/manifest.json");require(manifest.open(QIODevice::ReadOnly),"manifest");
        auto meta=QJsonDocument::fromJson(manifest.readAll()).object();
        int cases=0,lines=0;double worst=0,floatWorst=0;
        RingTimevarLowpass filter;
        // Captured from exact baseline's preprocessBlock + OLD enhancer with both switches OFF.
        std::ifstream offFixture(std::string(TIMEVAR_FIXTURES)+"/../baseline_off.csv");
        require(offFixture.good(),"missing exact baseline OFF fixture");
        std::string offRow;std::getline(offFixture,offRow);int offCases=0;
        while(std::getline(offFixture,offRow)) {
            std::replace(offRow.begin(),offRow.end(),',',' ');std::istringstream fields(offRow);
            int n,flags,delay;std::uint64_t referenceHash;fields>>n>>flags>>delay>>referenceHash;
            RingTimevarLowpassConfig off;require(filter.configure(off,250e6,n),"OFF config");
            std::vector<double> x(n);for(int i=0;i<n;++i)x[i]=std::sin(i*.071)+.4*std::cos(i*1.7);
            const auto original=x;
            ringrecon::PreprocessParams pp;pp.systemDelay=delay;pp.dbrRemove=flags&1;
            pp.delayCut=flags&2;pp.signalImpair=flags&4;pp.maskLength=300;pp.dbrmaskExtra=13;pp.imValue=.3;
            std::vector<float> y;require(ringTimevarPreprocess(filter,x,pp,y),"OFF preprocess");
            require(x==original,"OFF input changed");
            std::uint64_t hash=14695981039346656037ULL;
            for(size_t i=0;i<y.size()*sizeof(float);++i){hash^=reinterpret_cast<const unsigned char*>(y.data())[i];hash*=1099511628211ULL;}
            require(hash==referenceHash,"OFF baseline bitwise mismatch");++offCases;
        }
        for(const auto& value:meta["cases"].toArray()) {
            const auto item=value.toObject();if(item.contains("error"))continue;
            require(item["finite"].toBool(),"invalid MATLAB reference");
            RingTimevarLowpassConfig c;QString e;const int n=item["N"].toInt();
            require(ring_timevar::fromJson(item["config"],250e6,n,c,&e),qPrintable(e));
            std::string err;require(filter.configure(c,250e6,n,&err),err.c_str());
            const QString base=QString(TIMEVAR_FIXTURES)+"/"+item["name"].toString();
            const auto x=csv(base+"_x.csv"), y=csv(base+"_y.csv");
            for(size_t col=0;col<x[0].size();++col) {
                std::vector<double> input(n),ref(n);
                for(int i=0;i<n;++i){input[i]=x[i][col];ref[i]=y[i][col];}
                auto actual=input;require(filter.apply(actual,&err),err.c_str());
                compare(actual,ref,1e-8,worst);
                auto repeat=input;require(filter.apply(repeat),"repeat");
                require(std::memcmp(actual.data(),repeat.data(),n*sizeof(double))==0,"state crossed A-lines");
                for(int flags=0;flags<8;++flags)for(int delay:{1,7,17}) {
                    if(delay>n)continue;
                    ringrecon::PreprocessParams pp;pp.dbrRemove=flags&1;pp.signalImpair=flags&2;
                    pp.delayCut=flags&4;pp.systemDelay=delay;pp.dbrmaskExtra=1;
                    pp.maskLength=std::min(12,n-1);pp.imValue=.3;
                    auto full=input;std::vector<float> output;
                    require(ringTimevarPreprocess(filter,full,pp,output,&err),err.c_str());
                    auto expected=ringrecon::preprocessBlock(ref,n,1,pp);
                    compare(std::vector<double>(output.begin(),output.end()),std::vector<double>(expected.begin(),expected.end()),1e-5,floatWorst);
                }
                ++lines;
            }
            // auto and same explicit FFT size must be identical.
            auto explicitConfig=c;explicitConfig.fftLength=ring_timevar::fftLength(c);
            RingTimevarLowpass explicitFilter;require(explicitFilter.configure(explicitConfig,250e6,n),"explicit");
            auto a=std::vector<double>(n,1),b=a;filter.apply(a);explicitFilter.apply(b);require(a==b,"auto vs explicit");
            ++cases;
        }
        RingTimevarLowpassConfig off;
        require(filter.configure(off,250e6,3),"disabled short depth");
        std::vector<double> bypass={-0.0,1.2345678912345,std::numeric_limits<double>::quiet_NaN()};
        auto original=bypass;require(filter.apply(bypass),"bypass");
        require(std::memcmp(bypass.data(),original.data(),bypass.size()*8)==0,"OFF not bitwise");
        // Bad snapshots cannot replace accepted config or half-write output.
        auto c=RingTimevarLowpassConfig{};c.enabled=true;require(filter.configure(c,250e6,2030),"valid");
        auto bad=c;bad.p2=2031;require(!filter.configure(bad,250e6,2030),"depth accepted");
        bad=c;bad.fcMinMhz=1e-30;bad.p2=1000;
        require(!filter.configure(bad,250e6,2030),"nonfinite MATLAB mask configuration accepted");
        require(filter.config().p2==2000,"partial config");
        std::vector<double> invalid(2030,1);invalid[12]=INFINITY;auto unchanged=invalid;
        require(!filter.apply(invalid),"nonfinite accepted");require(invalid==unchanged,"partial line mutation");
        for(const auto& v: {QJsonValue(QJsonValue::Null),QJsonValue(true),QJsonValue(QJsonObject{{"enabled",1}}),
            QJsonValue(QJsonObject{{"p1",1.5}}),QJsonValue(QJsonObject{{"fcMinMhz","1"}}),
            QJsonValue(QJsonObject{{"transition","bogus"}}),QJsonValue(QJsonObject{{"overlapRatio",1.0}})}) {
            auto keep=c;require(!ring_timevar::fromJson(v,250e6,2030,keep),"malformed JSON accepted");
            require(keep.enabled&&keep.p2==2000,"JSON partial write");
        }
        QTemporaryDir dir;require(dir.isValid(),"temp dir");QSettings ini(dir.filePath("settings.ini"),QSettings::IniFormat);
        ini.beginGroup("RingConfigDialog/Defaults");ini.setValue("enhanceBipolar",true);
        RingTimevarLowpassConfig loaded;require(ring_timevar::loadSettings(ini,250e6,5000,loaded),"empty INI");
        require(!loaded.enabled&&loaded.fcMaxMhz==45,"legacy resurrected");
        c.windowLength=129;c.fftLength=192;c.overlapRatio=.625;c.filterOrder=2.5;c.transition="linear";
        ring_timevar::saveSettings(ini,c);ini.sync();require(!ini.contains("enhanceBipolar"),"old key remains");
        require(ring_timevar::loadSettings(ini,250e6,5000,loaded),"settings load");
        require(ring_timevar::toJson(c)==ring_timevar::toJson(loaded),"INI roundtrip");
        ini.setValue("timevarLpP2","broken");loaded=off;
        require(!ring_timevar::loadSettings(ini,250e6,5000,loaded),"bad INI accepted");require(!loaded.enabled,"partial INI");
        std::cout<<"PASS baselineOFF="<<offCases<<" MATLAB cases="<<cases<<" columns="<<lines<<" worstNormalizedAbs="<<worst<<" floatWorst="<<floatWorst<<'\n';
        return 0;
    } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
