#include "ImagingSvc.h"
#include "ImagingSharedMemory.h"
#include "RingFbpArtifact.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QTemporaryDir>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
static void need(bool b,const char*s){if(!b)throw std::runtime_error(s);}
static void close(const std::vector<float>&a,const std::vector<float>&b){double d=0,s=0;for(size_t i=0;i<a.size();++i){need(std::isfinite(a[i]),"nonfinite image");d+=pow(double(a[i])-b[i],2);s+=double(b[i])*b[i];}need(s>0&&sqrt(d/s)<1e-5,"service vs all-at-once FBP mismatch");}
struct RingTimevarServiceTest {
static void checkDasSectors(){
    constexpr int depth=512,nx=40,frame=nx*nx,perCh=4;
    constexpr double pi=3.14159265358979323846, first=180.;
    for(const auto& physical: {std::vector<int>{0,2,5},std::vector<int>{0,1,2,3,5,6,7}})
    for(int positive: {0,1}){
        const int M=int(physical.size()),lines=M*perCh;
        const int sign=positive?1:-1;
        const double width=360./M;
        QSharedMemory shm("MC_410T_RingShm");
        need(shm.create(int(ringImagingShmTotalSizeV2(depth*lines,frame,lines,frame))),"isolated DAS SHM");
        shm.lock();
        std::memset(shm.data(),0,shm.size());
        auto* h=static_cast<RingImagingShmHeader*>(shm.data());
        h->magic=0x52494e47;h->version=2;h->block_size=depth*lines;h->alines=lines;
        h->frame_size=frame;h->nx=nx;h->ny=nx;h->display_nx=nx;h->display_ny=nx;
        h->display_frame_size=frame;h->snapshot_step=1;
        h->block_seq=1;h->block_ready=1;
        auto* raw=reinterpret_cast<float*>(h+1);
        auto* angles=reinterpret_cast<float*>(reinterpret_cast<char*>(h+1)+ringAnglesOffset(depth*lines));
        auto* channels=reinterpret_cast<uint8_t*>(h+1)+ringChannelsOffset(depth*lines,lines);
        for(int j=0;j<perCh;++j)for(int rank=0;rank<M;++rank){
            const int pos=j*M+rank,c=physical[rank],w=j%2;
            angles[pos]=float(first+sign*(rank+(j/2+w)*.5)*width);channels[pos]=c;
            std::fill_n(raw+pos*depth,depth,float(c+1+10*w));
        }
        shm.unlock();
        QJsonArray enabled,radii;
        for(int c=0;c<8;++c){
            enabled.append(std::find(physical.begin(),physical.end(),c)!=physical.end()?1:0);
            radii.append(.02);
        }
        QJsonObject ring{{"sampDepth",depth},{"daqHz",10e6},{"fov",.02},{"gridSize",.0005},{"fovDeg",360.},
            {"alinesPerChannelPerBlock",perCh},{"alinesPerChannelPerFrame",2},{"alinesPerFrame",lines},
            {"enabledChannels",enabled},{"radiusPerChannel",radii},{"multiRadius",1},{"spliceMode",1},{"spliceBlendDeg",0.},
            {"sectorStartDeg",first},{"sectorCcw",positive},{"delayCut",0},{"dbrSigRemove",0},{"singalImpair",0},
            {"timevarLowpass",QJsonObject{{"enabled",false},{"fcMaxMhz",4.}}},
            {"soundSpeedRadiiCount",0},{"soundSpeeds",QJsonArray{1490.}}};
        ImagingSvc svc;
        svc.processRingConfigure(ring);
        need(svc.m_ringCuda[0]&&svc.m_ringCuda[1],"DAS configure failed");
        svc.m_running=true;
        svc.processRingPulse(1,1,0,true,{1,0},true);
        need(h->frame_seq==1,"DAS did not publish a frame");
        std::vector<int> seen(M);
        for(int x=0;x<nx;++x)for(int y=0;y<nx;++y){
            const double angle=std::atan2(y-(nx-1)*.5,x-(nx-1)*.5)*180/pi;
            const double directed=std::fmod(sign*(angle-first)+720.,360.);
            if(std::abs(std::remainder(directed,width))<1.)continue; // exclude the pixel boundary
            const int rank=int(directed/width);
            for(int w=0;w<2;++w){
                const auto& image=w?svc.m_ringDisplay1:svc.m_ringDisplay0;
                need(std::abs(image[x*nx+y]-(physical[rank]+1+10*w))<1e-4,"DAS pixel has wrong channel owner");
            }
            ++seen[rank];
        }
        need(std::all_of(seen.begin(),seen.end(),[](int n){return n>20;}),"DAS did not sample every sector");
        std::cout<<"DAS sector ownership PASS M="<<M<<" sectorCcw="<<positive<<'\n';
    }
}
static void run(bool twoOnly=false){
    if(!twoOnly)checkDasSectors();
    QTemporaryDir calibrationDir;need(calibrationDir.isValid(),"temporary calibration directory");
    QJsonArray offsets,delaysFbp;
    for(int c=0;c<8;++c){offsets.append(.15*c);delaysFbp.append(QJsonArray{.375+c*.1,-.625-c*.1});}
    QJsonObject calibration{{"formatVersion",1},{"geometry","concentric-ring"},{"daqHz",10e6},
        {"angleOffsetDeg",offsets},{"delayOffsetSamples",delaysFbp}};
    const auto calibrationPath=calibrationDir.filePath("ring.json");
    auto writeCalibration=[&]{QFile f(calibrationPath);need(f.open(QIODevice::WriteOnly),"write calibration");
        auto bytes=QJsonDocument(calibration).toJson();need(f.write(bytes)==bytes.size(),"calibration write size");};
    writeCalibration();
    const std::vector<int> testedChannels=twoOnly?std::vector<int>{2}:std::vector<int>{3,4,8};
    for(int M:testedChannels)for(bool staged:(M==3?std::vector<bool>{false,true}:std::vector<bool>{false}))
    for(int ccw:{0,1})for(bool splice:{false,true})for(bool layered:{false,true})for(bool timevar:{false,true}){
        const int depth=256,perCh=6,nx=20,frame=nx*nx,lines=M*perCh;
        const double width=360./M, first=180.;
        const int sign=ccw?1:-1;
        std::vector<int> physical;
        if(M==3)physical={0,2,5};
        else for(int c=0;c<M;++c)physical.push_back(c);
        const bool median=!twoOnly||splice, gap=twoOnly&&splice, invert=twoOnly&&ccw;
        const bool mask=twoOnly&&layered, clip=twoOnly&&ccw, cutEnabled=!twoOnly||splice;
        QSharedMemory shm("MC_410T_RingShm");need(shm.create(int(ringImagingShmTotalSizeV2(depth*lines,frame,lines,frame))),"isolated SHM");
        shm.lock();memset(shm.data(),0,shm.size());auto*h=static_cast<RingImagingShmHeader*>(shm.data());
        h->magic=0x52494e47;h->version=2;h->block_size=depth*lines;h->alines=lines;h->frame_size=frame;h->nx=nx;h->ny=nx;h->display_nx=nx;h->display_ny=nx;h->display_frame_size=frame;h->snapshot_step=1;shm.unlock();
        QJsonArray enabled,delays,radii;for(int c=0;c<8;++c){
            enabled.append(std::find(physical.begin(),physical.end(),c)!=physical.end()?1:0);
            delays.append(QJsonArray{5+c,7+c});radii.append(.004+c*.00001);
        }
        QJsonObject ring{{"sampDepth",depth},{"daqHz",10e6},{"alinesPerChannelPerBlock",perCh},{"alinesPerChannelPerFrame",9},
            {"alinesPerFrame",M*18},{"enabledChannels",enabled},{"sysDelayPerChannel",delays},{"fov",.02},{"gridSize",.001},
            {"radius",.004},{"radiusPerChannel",radii},{"multiRadius",1},{"spliceMode",int(splice)},{"spliceBlendDeg",1.5},
            {"sectorCcw",ccw},{"sectorStartDeg",first},{"delayCut",int(cutEnabled)},{"shiftWL2",1},
            {"dbrSigRemove",int(mask)},{"maskLength",56},{"singalImpair",int(clip)},{"imValue",QJsonArray{.25,.125}},
            {"soundSpeedRadiiCount",layered?1:0},{"soundSpeedRadii",QJsonArray{.007}},{"soundSpeeds",QJsonArray{1490.,1560.}},
            {"timevarLowpass",QJsonObject{{"enabled",timevar},{"fcMaxMhz",4.},{"fcMinMhz",1.},
                {"p1",10},{"p2",200},{"windowLength",32},{"fftLength",48}}},
            {"timevarBackend","cuda"},
            {"fbpPreprocessBackend",staged?"staged":(twoOnly?"cuda":"auto")},
            {"exactFbp",QJsonObject{{"enabled",true},{"invert",invert},{"median",median},{"angularSamples",129}}}};
        if(layered){RingFbpSettings settings;settings.enabled=true;settings.invert=invert;settings.median=median;settings.angularSamples=129;
            settings.calibrationFile=calibrationPath;QString error;
            need(ring_fbp_settings::loadCalibration(settings,10e6,&error),"calibration load failed");
            need(settings.delayOffset[3][1]==delaysFbp[3].toArray()[1].toDouble(),"fractional offset lost on import");
            ring["exactFbp"]=ring_fbp_settings::toJson(settings);}
        ImagingSvc svc;svc.processRingConfigure(ring);need(svc.m_fbp[0]&&svc.m_fbp[1],"FBP configure failed");svc.m_running=true;
        need(svc.m_ringTimevarGpu.active()==timevar,"GPU STFT selection mismatch");
        need(bool(svc.m_fbpDevice)!=staged,"Unexpected FBP preprocessing path");
        RingTimevarLowpass referenceFilter;std::string filterError;
        need(referenceFilter.configure(svc.m_ringTimevarConfig,10e6,depth,&filterError),"reference filter configure");
        std::vector<ring_fbp_stream::Line> full[8][2];
        for(int block=0;block<3;++block){
            shm.lock();h->block_seq=block+1;h->block_ready=1;auto* raw=reinterpret_cast<float*>(h+1);
            auto* ang=reinterpret_cast<float*>(reinterpret_cast<char*>(h+1)+ringAnglesOffset(depth*lines));
            auto* ch=reinterpret_cast<uint8_t*>(h+1)+ringChannelsOffset(depth*lines,lines);
            for(int j=0;j<perCh;++j)for(int rank=0;rank<M;++rank){int c=physical[rank],pos=j*M+rank,w=j%2,k=block*3+j/2;
                ang[pos]=float(first+sign*(rank+(k+w+(gap&&k>=3))/9.)*width);ch[pos]=c;
                ring_fbp_stream::Line l;l.angle=float(ang[pos]+svc.m_fbpSettings.angleOffset[c]);int cut=cutEnabled?(w?7:5)+c-1:0;
                std::vector<double> uncut;
                for(int t=0;t<depth;++t){float v=float(sin(t*.27+c*.13+w*.5+k*.09)*exp(-pow((t-80.)/33,2)));
                    if(k==4&&t==50)v+=3;raw[pos*depth+t]=v;uncut.push_back(invert?-v:v);}
                ring_fbp_stream::fractionalAdvance(uncut,svc.m_fbpSettings.delayOffset[c][w]);
                need(referenceFilter.apply(uncut,&filterError),"reference STFT");
                for(int t=cut;t<depth;++t){
                    double v=mask&&t<56+2*w?0:uncut[t];
                    if(clip){const double limit=w?.125:.25;v=std::clamp(v,-limit,limit);}
                    l.data.push_back(float(v));
                }
                full[c][w].push_back(l);
            }
            std::vector<float> saved(raw,raw+depth*lines);shm.unlock();
            svc.processRingPulse(block+1,block+1,0,true,{1,0},block==2);
            need(!memcmp(saved.data(),h+1,saved.size()*4),"raw input changed");
            need(svc.m_running,"FBP service stopped");
        }
        need(svc.m_ringBlockIndex==0,"source final did not reset");
        for(int w=0;w<2;++w){void*ref=nullptr;auto f=ring_fbp_settings::native(svc.m_fbpSettings,svc.m_ringConfig);
            need(!ring_fbp_create(&svc.m_ringConfig,&f,&ref),ring_fbp_error());
            for(int rank=0;rank<M;++rank){int c=physical[rank];std::vector<float> flat,ang;auto& seq=full[c][w];
                for(int j=0;j<9;++j){
                    const int left=gap&&j==3?j:std::max(0,j-1),right=gap&&j==2?j:std::min(8,j+1);
                    auto l=ring_fbp_stream::Stream::prepared(seq[left],seq[j],seq[right],median);
                    flat.insert(flat.end(),l.data.begin(),l.data.end());ang.push_back(l.angle);
                }
                const double start=first+sign*rank*width, end=first+sign*(rank+1)*width;
                need(!ring_fbp_append(ref,flat.data(),int(seq[0].data.size()),9,ang.data(),
                                      radii[c].toDouble(),std::min(start,end),width),ring_fbp_error());
            }std::vector<float> image(frame);ring_fbp_snapshot(ref,image.data());close(w?svc.m_ringDisplay1:svc.m_ringDisplay0,image);ring_fbp_destroy(ref);
        }
        auto* before=svc.m_fbp[0];auto invalid=ring;invalid["exactFbp"]=QJsonObject{{"enabled",true},{"angularSamples",1}};
        svc.processRingConfigure(invalid);need(svc.m_fbp[0]==before,"invalid config destroyed accepted context");
        for(auto&c:svc.m_fbpStreams)for(auto&s:c)need(s.pending.data.empty(),"cross-round median state");
        for(auto&c:svc.m_fbpHistoryAngle)for(auto&a:c)need(!a,"cross-round GPU median state");
        if(twoOnly&&ccw&&splice&&layered&&timevar){
            QVector<float> raw(depth*2*M,.1f),angles(2*M);
            QVector<quint8> channels(2*M);
            for(int j=0;j<2;++j)for(int c=0;c<M;++c){angles[j*M+c]=float(c*180+j*20);channels[j*M+c]=c;}
            need(svc.processFbpBlock(raw,angles,channels,false),"GPU duplicate test initial block");
            for(auto& a:angles)a+=20;
            need(svc.processFbpBlock(raw,angles,channels,false),"GPU duplicate test second block");
            std::vector<float> image(frame);
            need(!ring_fbp_snapshot(svc.m_fbp[0],image.data()),"GPU duplicate test snapshot");
            need(std::any_of(image.begin(),image.end(),[](float v){return v!=0;}),"GPU duplicate test had no accumulation");
            need(!svc.processFbpBlock(raw,angles,channels,false)&&!svc.m_running,"GPU duplicate accepted");
            for(auto* fbp:svc.m_fbp){
                need(!ring_fbp_snapshot(fbp,image.data()),"GPU duplicate reset snapshot");
                need(std::all_of(image.begin(),image.end(),[](float v){return v==0;}),"GPU duplicate left a partial image");
            }
            for(auto& c:svc.m_fbpHistoryAngle)for(auto& a:c)need(!a,"GPU error left median history");
        }
        std::cout<<"service PASS M="<<M<<" ccw="<<ccw<<" splice="<<splice<<" layered="<<layered
                 <<" timevar="<<timevar<<" staged="<<staged<<'\n';
    }
    std::vector<double> tmpl(401);std::vector<float>x(401);for(int i=0;i<401;++i){tmpl[i]=exp(-pow((i-180.)/7,2))*cos((i-180)*.6);x[i]=float(exp(-pow((i-182.)/7,2))*cos((i-182)*.6));}
    need(fabs(ring_fbp_artifact::delay(x.data(),tmpl)-2)<.01,"card timing sign");
    // Hold a prefix spanning three blocks; correct relative card delay before
    // preprocessing, flush each physical line once, then clear all round state.
    ImagingSvc timed;ring_recon_cuda_set_defaults(&timed.m_ringConfig);
    auto& cfg=timed.m_ringConfig;cfg.sampDepth=512;cfg.daqHz=10e6;cfg.fov=.02;cfg.gridSize=.001;
    const std::vector<int> timingChannels=twoOnly?std::vector<int>{0,2}:std::vector<int>{0,1,2,3};
    const int M=int(timingChannels.size());
    cfg.alinesPerChannelPerFrame=9;cfg.enabledChannelCount=M;cfg.alinesPerChannelPerBlock=6;
    cfg.sectorCcw=1;
    cfg.soundSpeedRadiiCount=0;cfg.soundSpeedsCount=1;cfg.soundSpeeds[0]=1490;cfg.delayCut=0;cfg.dbrSigRemove=0;cfg.singalImpair=0;
    timed.m_ringChannelCount=M;timed.m_fbpSettings.enabled=true;timed.m_fbpSettings.invert=false;
    QJsonArray templates;for(int c=0;c<8;++c){
        const bool selected=std::find(timingChannels.begin(),timingChannels.end(),c)!=timingChannels.end();
        cfg.enabledChannels[c]=selected;timed.m_ringChannels[c]=selected;cfg.radiusPerChannel[c]=.004;
        QJsonArray t;for(double v:tmpl)t.append(v);templates.append(QJsonArray{t,t});}
    calibration["artifact"]=QJsonObject{{"startSample",1},{"prefixPerWavelength",8},{"templates",templates}};
    // This timing-only file has no static shift, isolating the estimated card correction.
    QJsonArray zeroAngles,zeroDelays;for(int c=0;c<8;++c){zeroAngles.append(0.);zeroDelays.append(QJsonArray{0.,0.});}
    calibration["angleOffsetDeg"]=zeroAngles;calibration["delayOffsetSamples"]=zeroDelays;writeCalibration();
    timed.m_fbpSettings.calibrationFile=calibrationPath;QString calibrationError;
    need(ring_fbp_settings::loadCalibration(timed.m_fbpSettings,10e6,&calibrationError),"timing template import failed");
    need(ring_fbp_settings::validate(timed.m_fbpSettings,cfg,timed.m_ringSysDelayCh,&calibrationError),"imported template validation");
    auto native=ring_fbp_settings::native(timed.m_fbpSettings,cfg);
    for(int w=0;w<2;++w)need(!ring_fbp_create(&cfg,&native,&timed.m_fbp[w]),ring_fbp_error());
    if(twoOnly){
        // Three buffered blocks exceed the 6-line tile; exercise multiple cuFFT batches.
        void* device=nullptr;
        need(!ring_fbp_preprocess_create(512,6,M*18,&device),ring_fbp_preprocess_error());
        timed.m_fbpDevice.reset(device);
        timed.m_ringTimevarConfig.enabled=true;timed.m_ringTimevarConfig.fcMaxMhz=4.;
        timed.m_ringTimevarConfig.fcMinMhz=1.;timed.m_ringTimevarConfig.p1=10;timed.m_ringTimevarConfig.p2=200;
        timed.m_ringTimevarConfig.windowLength=32;timed.m_ringTimevarConfig.fftLength=48;
        std::string error;
        need(timed.m_ringTimevar.configure(timed.m_ringTimevarConfig,10e6,512,&error),"timing STFT reference configure");
        need(timed.m_ringTimevarGpu.configure(timed.m_ringTimevar,10e6,6,&error),"timing GPU STFT configure");
    }
    QVector<float> raw(512*6*M),angles(6*M);QVector<quint8>channels(6*M);
    for(int block=0;block<3;++block){for(int j=0;j<6;++j)for(int i=0;i<M;++i){int c=timingChannels[i],pos=j*M+i;channels[pos]=c;angles[pos]=float(i*360./M+(block*3+j/2)*360./M/9);
        for(int t=0;t<512;++t){double z=t-180-(c<2?2:0);raw[pos*512+t]=float(exp(-pow(z/7,2))*cos(z*.6));}}
        need(timed.processFbpBlock(raw,angles,channels,block==2),"timing prefix pipeline failed");
        if(block<2)need(!timed.m_fbpTimingReady&&!timed.m_fbpPrefixRaw.empty(),"prefix released early");}
    need(timed.m_fbpTimingReady&&fabs(timed.m_fbpCardShift[0]-2)<.01&&timed.m_fbpCardShift[1]==0,"relative card correction");
    for(auto& c:timed.m_fbpStreams)for(auto&s:c)need(s.pending.data.empty(),"timing final flush");
    for(auto& c:timed.m_fbpHistoryAngle)for(auto&a:c)need(!a,"timing GPU final flush");
    std::vector<float> image(400);need(!ring_fbp_snapshot(timed.m_fbp[0],image.data()),"timing snapshot");double energy=0;for(float v:image)energy+=v*v;need(energy>0,"timing prefix lost input");
    timed.resetRingRecon();need(!timed.m_fbpTimingReady&&timed.m_fbpPrefixRaw.empty()&&timed.m_fbpCardShift[0]==0,"timing round reset");
    RingFbpSettings settings;QString error;
    need(!ring_fbp_settings::fromJson({{"enabled","true"}},settings,&error),"malformed bool accepted");
    need(!ring_fbp_settings::fromJson({{"angularSamples",129.5}},settings,&error),"fractional table size accepted");
    std::cout<<"timing prefix / card shift / round reset / invalid schema PASS\n";
}
};
int main(int argc,char**argv){QCoreApplication app(argc,argv);try{RingTimevarServiceTest::run(app.arguments().contains("--two-channels"));return 0;}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
