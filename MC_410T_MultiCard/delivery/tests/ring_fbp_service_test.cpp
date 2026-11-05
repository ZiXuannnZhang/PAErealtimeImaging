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
static void run(bool twoOnly=false){
    QTemporaryDir calibrationDir;need(calibrationDir.isValid(),"temporary calibration directory");
    QJsonArray offsets,delaysFbp;
    for(int c=0;c<8;++c){offsets.append(.15*c);delaysFbp.append(QJsonArray{.375+c*.1,-.625-c*.1});}
    QJsonObject calibration{{"formatVersion",1},{"geometry","concentric-ring"},{"daqHz",10e6},
        {"angleOffsetDeg",offsets},{"delayOffsetSamples",delaysFbp}};
    const auto calibrationPath=calibrationDir.filePath("ring.json");
    auto writeCalibration=[&]{QFile f(calibrationPath);need(f.open(QIODevice::WriteOnly),"write calibration");
        auto bytes=QJsonDocument(calibration).toJson();need(f.write(bytes)==bytes.size(),"calibration write size");};
    writeCalibration();
    const std::vector<int> testedChannels=twoOnly?std::vector<int>{2}:std::vector<int>{4,8};
    for(int M:testedChannels)for(int ccw:{0,1})for(bool splice:{false,true})for(bool layered:{false,true})for(bool timevar:{false,true}){
        const int depth=256,perCh=6,nx=20,frame=nx*nx,lines=M*perCh;
        QSharedMemory shm("MC_410T_RingShm");need(shm.create(int(ringImagingShmTotalSizeV2(depth*lines,frame,lines,frame))),"isolated SHM");
        shm.lock();memset(shm.data(),0,shm.size());auto*h=static_cast<RingImagingShmHeader*>(shm.data());
        h->magic=0x52494e47;h->version=2;h->block_size=depth*lines;h->alines=lines;h->frame_size=frame;h->nx=nx;h->ny=nx;h->display_nx=nx;h->display_ny=nx;h->display_frame_size=frame;h->snapshot_step=1;shm.unlock();
        QJsonArray enabled,delays,radii;for(int c=0;c<8;++c){enabled.append(c<M?1:0);delays.append(QJsonArray{5+c,7+c});radii.append(.004+c*.00001);}
        QJsonObject ring{{"sampDepth",depth},{"daqHz",10e6},{"alinesPerChannelPerBlock",perCh},{"alinesPerChannelPerFrame",9},
            {"alinesPerFrame",M*18},{"enabledChannels",enabled},{"sysDelayPerChannel",delays},{"fov",.02},{"gridSize",.001},
            {"radius",.004},{"radiusPerChannel",radii},{"multiRadius",1},{"spliceMode",int(splice)},{"spliceBlendDeg",1.5},
            {"sectorCcw",ccw},{"sectorStartDeg",0},{"delayCut",1},{"shiftWL2",1},{"dbrSigRemove",0},{"singalImpair",0},
            {"soundSpeedRadiiCount",layered?1:0},{"soundSpeedRadii",QJsonArray{.007}},{"soundSpeeds",QJsonArray{1490.,1560.}},
            {"timevarLowpass",QJsonObject{{"enabled",timevar},{"fcMaxMhz",4.},{"fcMinMhz",1.},
                {"p1",10},{"p2",200},{"windowLength",32},{"fftLength",48}}},
            {"timevarBackend","cuda"},
            {"exactFbp",QJsonObject{{"enabled",true},{"invert",false},{"angularSamples",129}}}};
        if(layered){RingFbpSettings settings;settings.enabled=true;settings.invert=false;settings.angularSamples=129;
            settings.calibrationFile=calibrationPath;QString error;
            need(ring_fbp_settings::loadCalibration(settings,10e6,&error),"calibration load failed");
            need(settings.delayOffset[3][1]==delaysFbp[3].toArray()[1].toDouble(),"fractional offset lost on import");
            ring["exactFbp"]=ring_fbp_settings::toJson(settings);}
        ImagingSvc svc;svc.processRingConfigure(ring);need(svc.m_fbp[0]&&svc.m_fbp[1],"FBP configure failed");svc.m_running=true;
        need(svc.m_ringTimevarGpu.active()==timevar,"GPU STFT selection mismatch");
        RingTimevarLowpass referenceFilter;std::string filterError;
        need(referenceFilter.configure(svc.m_ringTimevarConfig,10e6,depth,&filterError),"reference filter configure");
        std::vector<ring_fbp_stream::Line> full[8][2];
        for(int block=0;block<3;++block){
            shm.lock();h->block_seq=block+1;h->block_ready=1;auto* raw=reinterpret_cast<float*>(h+1);
            auto* ang=reinterpret_cast<float*>(reinterpret_cast<char*>(h+1)+ringAnglesOffset(depth*lines));
            auto* ch=reinterpret_cast<uint8_t*>(h+1)+ringChannelsOffset(depth*lines,lines);
            for(int j=0;j<perCh;++j)for(int c=0;c<M;++c){int pos=j*M+c,w=j%2,k=block*3+j/2;
                ang[pos]=float(c*360./M+(ccw?1:-1)*(k+w)*360./M/9);ch[pos]=c;
                ring_fbp_stream::Line l;l.angle=float(ang[pos]+svc.m_fbpSettings.angleOffset[c]);int cut=(w?7:5)+c-1;
                std::vector<double> uncut;
                for(int t=0;t<depth;++t){float v=float(sin(t*.27+c*.13+w*.5+k*.09)*exp(-pow((t-80.)/33,2)));
                    if(k==4&&t==50)v+=3;raw[pos*depth+t]=v;uncut.push_back(v);}
                ring_fbp_stream::fractionalAdvance(uncut,svc.m_fbpSettings.delayOffset[c][w]);
                need(referenceFilter.apply(uncut,&filterError),"reference STFT");
                for(int t=cut;t<depth;++t)l.data.push_back(float(uncut[t]));
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
            for(int c=0;c<M;++c){std::vector<float> flat,ang;auto& seq=full[c][w];
                for(int j=0;j<9;++j){auto l=ring_fbp_stream::Stream::prepared(seq[std::max(0,j-1)],seq[j],seq[std::min(8,j+1)],true);flat.insert(flat.end(),l.data.begin(),l.data.end());ang.push_back(l.angle);}
                need(!ring_fbp_append(ref,flat.data(),int(seq[0].data.size()),9,ang.data(),radii[c].toDouble(),(ccw?c:c-1)*360./M,360./M),ring_fbp_error());
            }std::vector<float> image(frame);ring_fbp_snapshot(ref,image.data());close(w?svc.m_ringDisplay1:svc.m_ringDisplay0,image);ring_fbp_destroy(ref);
        }
        auto* before=svc.m_fbp[0];auto invalid=ring;invalid["exactFbp"]=QJsonObject{{"enabled",true},{"angularSamples",1}};
        svc.processRingConfigure(invalid);need(svc.m_fbp[0]==before,"invalid config destroyed accepted context");
        for(auto&c:svc.m_fbpStreams)for(auto&s:c)need(s.pending.data.empty(),"cross-round median state");
        std::cout<<"service PASS M="<<M<<" ccw="<<ccw<<" splice="<<splice<<" layered="<<layered<<" timevar="<<timevar<<'\n';
    }
    std::vector<double> tmpl(401);std::vector<float>x(401);for(int i=0;i<401;++i){tmpl[i]=exp(-pow((i-180.)/7,2))*cos((i-180)*.6);x[i]=float(exp(-pow((i-182.)/7,2))*cos((i-182)*.6));}
    need(fabs(ring_fbp_artifact::delay(x.data(),tmpl)-2)<.01,"card timing sign");
    // Hold a prefix spanning three blocks; correct relative card delay before
    // preprocessing, flush each physical line once, then clear all round state.
    ImagingSvc timed;ring_recon_cuda_set_defaults(&timed.m_ringConfig);
    auto& cfg=timed.m_ringConfig;cfg.sampDepth=512;cfg.daqHz=10e6;cfg.fov=.02;cfg.gridSize=.001;
    cfg.alinesPerChannelPerFrame=9;cfg.enabledChannelCount=4;cfg.alinesPerChannelPerBlock=6;
    cfg.soundSpeedRadiiCount=0;cfg.soundSpeedsCount=1;cfg.soundSpeeds[0]=1490;cfg.delayCut=0;cfg.dbrSigRemove=0;cfg.singalImpair=0;
    timed.m_ringChannelCount=4;timed.m_fbpSettings.enabled=true;timed.m_fbpSettings.invert=false;
    QJsonArray templates;for(int c=0;c<8;++c){cfg.enabledChannels[c]=c<4;timed.m_ringChannels[c]=c<4;cfg.radiusPerChannel[c]=.004;
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
    QVector<float> raw(512*24),angles(24);QVector<quint8>channels(24);
    for(int block=0;block<3;++block){for(int j=0;j<6;++j)for(int c=0;c<4;++c){int pos=j*4+c;channels[pos]=c;angles[pos]=float(c*90+(block*3+j/2)*10);
        for(int t=0;t<512;++t){double z=t-180-(c<2?2:0);raw[pos*512+t]=float(exp(-pow(z/7,2))*cos(z*.6));}}
        need(timed.processFbpBlock(raw,angles,channels,block==2),"timing prefix pipeline failed");
        if(block<2)need(!timed.m_fbpTimingReady&&!timed.m_fbpPrefixRaw.empty(),"prefix released early");}
    need(timed.m_fbpTimingReady&&fabs(timed.m_fbpCardShift[0]-2)<.01&&timed.m_fbpCardShift[1]==0,"relative card correction");
    for(auto& c:timed.m_fbpStreams)for(auto&s:c)need(s.pending.data.empty(),"timing final flush");
    std::vector<float> image(400);need(!ring_fbp_snapshot(timed.m_fbp[0],image.data()),"timing snapshot");double energy=0;for(float v:image)energy+=v*v;need(energy>0,"timing prefix lost input");
    timed.resetRingRecon();need(!timed.m_fbpTimingReady&&timed.m_fbpPrefixRaw.empty()&&timed.m_fbpCardShift[0]==0,"timing round reset");
    RingFbpSettings settings;QString error;
    need(!ring_fbp_settings::fromJson({{"enabled","true"}},settings,&error),"malformed bool accepted");
    need(!ring_fbp_settings::fromJson({{"angularSamples",129.5}},settings,&error),"fractional table size accepted");
    std::cout<<"timing prefix / card shift / round reset / invalid schema PASS\n";
}
};
int main(int argc,char**argv){QCoreApplication app(argc,argv);try{RingTimevarServiceTest::run(app.arguments().contains("--two-channels"));return 0;}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
