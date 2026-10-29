#include "ImagingSvc.h"
#include "ImagingSharedMemory.h"
#include "FilteredDasSettings.h"
#include "RingTimevarLowpassSettings.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QFile>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

static void need(bool b,const char* m){if(!b)throw std::runtime_error(m);}
static std::vector<float> read(const std::string& name) {
    QFile f(QString(FILTERED_FIXTURES)+"/"+QString::fromStdString(name)+".qz");
    need(f.open(QIODevice::ReadOnly),name.c_str());const auto data=qUncompress(f.readAll());
    need(!data.isEmpty()&&data.size()%4==0,"bad fixture bytes");std::vector<float> a(size_t(data.size())/4);
    std::memcpy(a.data(),data.constData(),data.size());return a;
}
static double worst=0;
static void compare(const std::vector<float>& a,const std::vector<float>& b) {
    need(a.size()==b.size(),"actual channel/wavelength nt changed");
    double err=0,scale=0,l2=0,norm=0;
    for(size_t i=0;i<a.size();++i){need(std::isfinite(a[i]),"nonfinite service output");
        const double d=double(a[i])-b[i];err=std::max(err,std::abs(d));scale=std::max(scale,std::abs(double(b[i])));l2+=d*d;norm+=double(b[i])*b[i];}
    worst=std::max(worst,err/std::max(1.0,scale));
    need(err<=1e-5*std::max(1.0,scale)&&std::sqrt(l2)<=1e-5*std::max(1.0,std::sqrt(norm)),"service MATLAB oracle mismatch");
}
static std::vector<std::vector<float>> captured[8][2];
static void capture(int c,int w,const std::vector<std::vector<float>>& lines){captured[c][w]=lines;}
struct RingTimevarServiceTest {
    static void run() {
        for(int channels:{4,8}) {
            const int n=5000, perCh=6, lines=channels*perCh, frame=400;
            QSharedMemory shm("MC_410T_RingShm");need(shm.create(int(ringImagingShmTotalSizeV2(n*lines,frame,lines,frame))),"isolated SHM");
            shm.lock();std::memset(shm.data(),0,shm.size());auto* h=static_cast<RingImagingShmHeader*>(shm.data());
            h->magic=0x52494E47;h->version=2;h->block_size=n*lines;h->alines=lines;h->frame_size=frame;
            h->nx=20;h->ny=20;h->display_nx=20;h->display_ny=20;h->display_frame_size=frame;h->snapshot_step=1;shm.unlock();
            ImagingSvc svc;svc.m_testFilteredLines=capture;
            QJsonArray enabled,delays;
            for(int c=0;c<8;++c){enabled.append(c<channels?1:0);delays.append(QJsonArray{358+3*c,371+3*c});}
            QJsonObject ring{{"sampDepth",n},{"daqHz",250e6},{"alinesPerChannelPerBlock",perCh},
                {"alinesPerFrame",channels*perCh*4},{"enabledChannels",enabled},{"sysDelayPerChannel",delays},
                {"fov",.002},{"gridSize",.0001},{"dbrSigRemove",1},{"maskLength",300},{"delayCut",1},
                {"singalImpair",1},{"imValue",QJsonArray{.3,.4}},{"shiftWL2",1}};
            for(int ccw:{1,0})for(bool splice:{false,true})for(int tv:{0,1})for(int fd:{0,1}) {
                const double start = ccw ? 180.0 : 0.0;
                const double sign = ccw ? 1.0 : -1.0;
                ring["sectorCcw"]=ccw; ring["sectorStartDeg"]=start;
                ring["multiRadius"]=int(splice); ring["spliceMode"]=int(splice);
                ring["timevarLowpass"]=QJsonObject{{"enabled",bool(tv)}};
                ring["filteredDas"]=QJsonObject{{"enabled",bool(fd)},{"batchColumns",2}};
                svc.processRingConfigure(ring);need(svc.m_ringCuda[0]&&svc.m_ringCuda[1],"configure CUDA");svc.m_running=true;
                const auto raw=read("service_"+std::to_string(tv)+"_raw.bin");
                auto fill=[&](int seq) {
                    shm.lock();h->block_seq=seq;h->block_ready=1;
                    auto* a=reinterpret_cast<float*>(h+1);
                    auto* angles=reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(h+1)+ringAnglesOffset(n*lines));
                    auto* ch=reinterpret_cast<uint8_t*>(h+1)+ringChannelsOffset(n*lines,lines);
                    for(int j=0;j<perCh;++j)for(int c=0;c<channels;++c) {
                        const int pos=j*channels+c;std::memcpy(a+pos*n,raw.data()+(j*8+c)*n,n*4);
                        angles[pos]=float(start+c*360.0/channels+sign*(j/2)*2);ch[pos]=c;
                    }
                    shm.unlock();
                };
                fill(1);const std::vector<float> input(reinterpret_cast<float*>(h+1),reinterpret_cast<float*>(h+1)+n*lines);
                svc.processRingPulse(1,1,0,true,{1,0},false);need(svc.m_ringBlockIndex==1,"first block failed");
                need(!std::memcmp(h+1,input.data(),input.size()*4),"shared/raw input modified");
                std::vector<float> flat[2],angles[2],radii[2],sectors[2];
                for(int c=0;c<channels;++c)for(int w=0;w<2;++w) {
                    const auto expected=read("service_"+std::to_string(tv)+"_c"+std::to_string(c)+"_w"+std::to_string(w)+(fd?"_after.bin":"_before.bin"));
                    const int nt=n-(358+13*w+3*c)+1;need(captured[c][w].size()==3,"column order/count");
                    for(int j=0;j<3;++j) {
                        compare(captured[c][w][j],std::vector<float>(expected.begin()+j*nt,expected.begin()+(j+1)*nt));
                        flat[w].insert(flat[w].end(),expected.begin()+j*nt,expected.begin()+(j+1)*nt);
                        angles[w].push_back(float(start+c*360.0/channels+sign*j*2));
                        sectors[w].push_back(float(start+(ccw?c:c-1)*360.0/channels));
                        radii[w].push_back(float(svc.m_ringConfig.radiusPerChannel[c]));
                    }
                    if(w==1)compare(svc.m_ringPrevWL2[c],captured[c][w].back());
                }
                // Preserve the existing append contract exactly, including its flatten/nt calculation.
                for(int w=0;w<2;++w) {
                    void* ref=nullptr;need(ring_recon_cuda_create(&svc.m_ringConfig,&ref)==0,"reference CUDA create");
                    const int nd=int(angles[w].size()),nt=int(flat[w].size()/nd);
                    const int rc=splice
                        ? ring_recon_cuda_append_angles_radii_sector(ref,flat[w].data(),nt,nd,
                            angles[w].data(),radii[w].data(),sectors[w].data(),float(360.0/channels))
                        : ring_recon_cuda_append_angles_radii(ref,flat[w].data(),nt,nd,angles[w].data(),radii[w].data());
                    need(rc==0,"reference DAS append");
                    std::vector<float> image(frame);need(ring_recon_cuda_snapshot(ref,20,1,image.data())==0,"reference snapshot");
                    compare(w?svc.m_ringDisplay1:svc.m_ringDisplay0,image);ring_recon_cuda_destroy(ref);
                }
                const auto transforms=svc.m_filteredDas.transforms();
                fill(2);svc.processRingPulse(2,2,0,true,{1,0},false);need(svc.m_ringBlockIndex==2,"second block failed");
                if(fd)need(svc.m_filteredDas.transforms()-transforms==size_t(channels*perCh*2),"WL2 history filtered twice");
                else need(!svc.m_filteredDas.transforms()&&!svc.m_filteredDas.cachedBytes(),"OFF created FFT work");
                for(int c=0;c<channels;++c)compare(svc.m_ringPrevWL2[c],captured[c][1][1]);
                // Last wavelength/channel fails after all earlier groups: no partial CUDA or history.
                if(fd) {
                    auto oldHistory=std::vector<std::vector<float>>(svc.m_ringPrevWL2,svc.m_ringPrevWL2+8);
                    std::vector<int> lengths;
                    for(int c=0;c<channels;++c)for(int w=0;w<2;++w)if(c!=channels-1||w!=1)lengths.push_back(n-(358+13*w+3*c)+1);
                    std::string e;need(svc.m_filteredDas.configure(svc.m_filteredDasConfig,250e6,lengths,&e),"inject late unprepared length");
                    const auto image0=svc.m_ringDisplay0;const auto image1=svc.m_ringDisplay1;const auto frameSeq=h->frame_seq;
                    std::vector<float> accBefore[2],weightBefore[2];int blocksBefore[2]={};
                    for(int w=0;w<2;++w) {
                        accBefore[w].resize(frame);weightBefore[w].resize(frame);
                        need(ring_recon_cuda_get_state(svc.m_ringCuda[w],accBefore[w].data(),weightBefore[w].data(),&blocksBefore[w])==0,
                             "read CUDA state before late failure");
                    }
                    fill(3);svc.processRingPulse(3,3,0,true,{1,0},false);
                    need(svc.m_ringBlockIndex==2&&h->frame_seq==frameSeq,"failure committed partial snapshot/block");
                    need(svc.m_ringDisplay0==image0&&svc.m_ringDisplay1==image1,"failure changed display");
                    for(int c=0;c<8;++c)need(svc.m_ringPrevWL2[c]==oldHistory[c],"failure committed partial history");
                    for(int w=0;w<2;++w) {
                        std::vector<float> acc(frame),weight(frame);int blocks=0;
                        need(ring_recon_cuda_get_state(svc.m_ringCuda[w],acc.data(),weight.data(),&blocks)==0,
                             "read CUDA state after late failure");
                        need(blocks==blocksBefore[w]&&acc==accBefore[w]&&weight==weightBefore[w],
                             "late failure submitted partial CUDA accumulation");
                    }
                }
                const auto oldTv=ring_timevar::toJson(svc.m_ringTimevarConfig);
                const auto oldFd=filtered_das::toJson(svc.m_filteredDasConfig);
                auto bad=ring;bad["timevarLowpass"]=QJsonObject{{"enabled",!bool(tv)}};
                for(const auto& value:{QJsonValue(QJsonValue::Null),QJsonValue(QJsonObject{{"order",2}}),
                    QJsonValue(QJsonObject{{"batchColumns",1.5}}),QJsonValue(QJsonObject{{"padFactor",1e300}}),
                    QJsonValue(QJsonObject{{"referenceMhz",1e-300},{"order",1}})}) {
                    bad["filteredDas"]=value;svc.processRingConfigure(bad);
                    need(oldTv==ring_timevar::toJson(svc.m_ringTimevarConfig)&&oldFd==filtered_das::toJson(svc.m_filteredDasConfig),"rejection partially accepted enhancement");
                }
                svc.resetRingRecon();need(svc.m_ringBlockIndex==0&&svc.m_ringPrevWL2[0].empty(),"reset retained history");
                std::cout<<"PASS channels="<<channels<<" ccw="<<ccw<<" splice="<<splice<<" timevar="<<tv<<" filtered="<<fd<<"\n";
            }
        }
        std::cout<<"PASS production four combinations, MATLAB pre-DAS/DAS, actual nt, WL2 once, late failure, atomic configuration worstNormalizedAbs="<<worst<<'\n';
    }
};
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{RingTimevarServiceTest::run();return 0;}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
