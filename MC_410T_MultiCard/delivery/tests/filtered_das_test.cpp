#include "FilteredDasPrefilter.h"
#include "FilteredDasSettings.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

static void need(bool b,const char* m) {if(!b)throw std::runtime_error(m);}
template<class T> static std::vector<T> read(const std::string& path) {
    QFile f(QString::fromStdString(path+".qz"));need(f.open(QIODevice::ReadOnly),path.c_str());
    const auto data=qUncompress(f.readAll());need(!data.isEmpty()&&data.size()%sizeof(T)==0,"bad fixture bytes");
    std::vector<T> a(static_cast<size_t>(data.size())/sizeof(T));std::memcpy(a.data(),data.constData(),data.size());return a;
}
static double worstAbs=0,worstL2=0,worstDouble=0,worstFloat=0,worstDoubleL2=0,worstFloatL2=0;
template<class T> static void run(const QJsonObject& entry) {
    const auto prefix=std::string(FILTERED_FIXTURES)+"/"+entry["name"].toString().toStdString();
    const int nt=entry["nt"].toInt(), cols=entry["columns"].toInt();
    const auto opts=entry["opts"].toObject();
    FilteredDasConfig c;c.order=opts["Order"].toDouble();c.referenceMhz=opts["ReferenceHz"].toDouble()/1e6;
    c.passMhz=opts["PassHz"].toDouble()/1e6;c.stopMhz=opts["StopHz"].toDouble()/1e6;
    c.padFactor=opts["PadFactor"].toDouble();
    const auto x=read<T>(prefix+"_x.bin"),r=read<T>(prefix+"_y.bin");
    const auto href=read<double>(prefix+"_H.bin"),fref=read<double>(prefix+"_F.bin");
    need(x.size()==size_t(nt)*cols&&x.size()==r.size(),"fixture shape");
    std::vector<std::vector<T>> base;
    for(int batch:{1,7,64,100}) {
        c.batchColumns=batch;FilteredDasPrefilter filter;std::string error;
        need(filter.configure(c,250e6,{nt},&error),error.c_str());
        const auto n=filtered_das::fftLength(c,nt);need(n==size_t(entry["nfft"].toInt()),"nfft");
        need((n-nt)/2==size_t(entry["left"].toInt()),"symmetric crop origin");
        auto h=filter.response(nt);need(h.size()==fref.size()&&href.size()==2*n,"response shape");
        for(size_t k=0;k<n;++k) {
            const double f=(k>n/2?double(k)-n:double(k))*(250e6/n);
            need(f==fref[k],"unshifted frequency axis");
            need(std::abs(h[k]-std::complex<double>(href[k],href[n+k]))<1e-12,"response formula/phase/window");
            if(k)need(std::abs(h[k]-std::conj(h[n-k]))<1e-12,"conjugate symmetry");
        }
        need(h[0]==std::complex<double>{}&&h[n/2]==std::complex<double>{},"DC/Nyquist not zero");
        if(c.order==.5)need(h[1].imag()<0&&h[n-1].imag()>0,"phase sign");
        auto y=std::vector<std::vector<T>>(cols,std::vector<T>(nt));
        for(int j=0;j<cols;++j)std::copy_n(x.data()+j*nt,nt,y[j].data());
        need(filter.apply(y,&error),error.c_str());
        double abs=0,scale=0,l2=0,norm=0;
        for(int j=0;j<cols;++j)for(int i=0;i<nt;++i) {
            double a=y[j][i],b=r[j*nt+i];need(std::isfinite(a),"nonfinite output");
            abs=std::max(abs,std::abs(a-b));scale=std::max(scale,std::abs(b));l2+=(a-b)*(a-b);norm+=b*b;
        }
        const double ae=abs/std::max(1.0,scale),le=std::sqrt(l2)/std::max(1.0,std::sqrt(norm));
        worstAbs=std::max(worstAbs,ae);worstL2=std::max(worstL2,le);
        if(sizeof(T)==4) {worstFloat=std::max(worstFloat,ae);worstFloatL2=std::max(worstFloatL2,le);}
        else {worstDouble=std::max(worstDouble,ae);worstDoubleL2=std::max(worstDoubleL2,le);}
        const double tolerance=sizeof(T)==4?1e-5:1e-8;
        if(ae>tolerance||le>tolerance)std::cerr<<prefix<<" abs="<<ae<<" l2="<<le<<'\n';
        need(ae<=tolerance&&le<=tolerance,"MATLAB all-sample oracle mismatch");
        if(base.empty())base=y;else need(y==base,"batch size changes output bytes/order");
        need(filter.maxBatchUsed()==size_t(std::min(batch,cols)),"BatchColumns not used");
        need(filter.batches()==size_t((cols+batch-1)/batch)&&filter.transforms()==size_t(2*cols),"batch/FFT counts");
        need(filter.workspaceBytes()==n*batch*sizeof(std::complex<double>),"batch workspace not reconfigured");
        auto repeated=std::vector<std::vector<T>>(1,std::vector<T>(x.begin()+2*nt,x.begin()+3*nt));
        need(filter.apply(repeated,&error)&&repeated[0]==y[2],"cross-line/repeated state");
        auto invalid=c;invalid.padFactor=1e300;
        need(!filter.configure(invalid,250e6,{nt},&error),"overflow accepted");
        need(filter.response(nt)==h,"invalid configure replaced accepted plan");
        need(!filter.configure(c,250e6,{70000},&error),"FFT resource bound accepted");
    }
}
int main(int argc,char** argv) {QCoreApplication app(argc,argv);try {
    QFile manifest(QString(FILTERED_FIXTURES)+"/manifest.json");need(manifest.open(QIODevice::ReadOnly),"manifest");
    const auto cases=QJsonDocument::fromJson(manifest.readAll()).object()["cases"].toArray();need(cases.size()==72,"oracle case count");
    for(const auto& e:cases) {
        const auto o=e.toObject();if(o["kind"]=="single")run<float>(o);else run<double>(o);
    }
    std::string error;FilteredDasConfig off;off.enabled=false;
    off.batchColumns=std::numeric_limits<int>::max();off.stopMhz=500;
    FilteredDasPrefilter bypass;need(bypass.configure(off,0,{},&error),"disabled runtime validation");
    std::vector<std::vector<float>> x{{-0.0f,1,std::numeric_limits<float>::quiet_NaN()}};auto original=x;
    need(bypass.apply(x,&error)&&!std::memcmp(x[0].data(),original[0].data(),12),"OFF not byte preserving");
    need(!bypass.cachedBytes()&&!bypass.transforms()&&!bypass.batches(),"OFF allocated/transformed");
    FilteredDasConfig c;QString qerror;
    need(filtered_das::fromJson(QJsonValue(QJsonValue::Undefined),250e6,5000,c,&qerror)
         &&c.enabled&&c.order==.5&&c.referenceMhz==5&&c.passMhz==40&&c.stopMhz==60&&c.padFactor==4&&c.batchColumns==64,
         "entry-script factory defaults for missing object");
    for(auto bad:{QJsonValue(QJsonValue::Null),QJsonValue(1),QJsonValue(QJsonObject{{"enabled",1}}),
        QJsonValue(QJsonObject{{"order","0.5"}}),QJsonValue(QJsonObject{{"order",1.1}}),
        QJsonValue(QJsonObject{{"batchColumns",1.5}}),QJsonValue(QJsonObject{{"batchColumns",0}}),
        QJsonValue(QJsonObject{{"padFactor",3.9}}),QJsonValue(QJsonObject{{"stopMhz",125}}),
        QJsonValue(QJsonObject{{"referenceMhz",41}}),QJsonValue(QJsonObject{{"passMhz",60}})})
        need(!filtered_das::fromJson(bad,250e6,5000,c,&qerror),"malformed JSON accepted");
    c=FilteredDasConfig{};c.batchColumns=std::numeric_limits<int>::max();
    need(!bypass.configure(c,250e6,{5000},&error),"workspace overflow accepted");
    c=FilteredDasConfig{};c.order=0;need(bypass.configure(c,250e6,{17},&error),"Order=0 config");
    std::vector<std::vector<float>> dc(1,std::vector<float>(17,1));need(bypass.apply(dc,&error),"Order=0 apply");
    need(dc[0]!=std::vector<float>(17,1)&&bypass.transforms()==2,"Order=0 treated as OFF");
    dc[0].resize(18);need(!bypass.apply(dc,&error),"unprepared length silently reused cache");
    FilteredDasPrefilter changing,fresh;c=FilteredDasConfig{};
    need(changing.configure(c,250e6,{17},&error),"initial cache");
    c.order=1;c.padFactor=8;c.batchColumns=7;
    need(changing.configure(c,200e6,{18},&error)&&changing.response(17).empty(),"old length cache survived configure");
    need(fresh.configure(c,200e6,{18},&error),"fresh cache");
    std::vector<std::vector<float>> a(2,std::vector<float>(18));a[0][0]=1;a[1][17]=1;auto b=a;
    need(changing.apply(a,&error)&&fresh.apply(b,&error)&&a==b,"Fs/order/nt/padding/batch cache invalidation");
    FilteredDasPrefilter limited; c=FilteredDasConfig{};c.batchColumns=1;
    std::vector<int> manyLengths;for(int i=0;i<16;++i)manyLengths.push_back(65000-i);
    need(!limited.configure(c,250e6,manyLengths,&error)&&error.find("128 MiB")!=std::string::npos,
         "total plan memory cap not enforced");
    need(!limited.cachedBytes(),"rejected multi-plan config partly replaced bypass");
    c.referenceMhz=1e-300;c.order=1;
    need(!limited.configure(c,250e6,{5000},&error),"nonfinite single response accepted");
    c=FilteredDasConfig{};c.order=1;c.batchColumns=1;
    need(limited.configure(c,250e6,{5000},&error),"output overflow test configure");
    std::vector<std::vector<float>> huge(1,std::vector<float>(5000));
    for(size_t i=0;i<5000;++i)huge[0][i]=static_cast<float>(std::sin(2*3.14159265358979323846*20e6*i/250e6)
        *(.5*std::numeric_limits<float>::max()));
    need(!limited.apply(huge,&error),"output narrowing overflow was not rejected");
    for(float v:huge[0])need(std::isfinite(v),"overflow stored a nonfinite float before rejection");
    std::cout<<"PASS MATLAB cases="<<cases.size()<<" batchVariants=4 all edges included worstNormalizedAbs="<<worstAbs<<" worstNormalizedL2="<<worstL2<<" doubleWorst="<<worstDouble<<" singleWorst="<<worstFloat
             <<" doubleL2="<<worstDoubleL2<<" singleL2="<<worstFloatL2<<'\n';
    return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
