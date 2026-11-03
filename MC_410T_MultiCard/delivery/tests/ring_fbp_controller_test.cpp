#include "ImagingController.h"
#include <QCoreApplication>
#include <QTimer>
#include <cmath>
#include <iostream>

// Real controller -> ZMQ -> deployed ImagingSvc -> CUDA -> SHM readiness test.
int main(int argc,char**argv){
    QCoreApplication app(argc,argv);ImagingController controller;
    RingReconCudaConfig c;ring_recon_cuda_set_defaults(&c);
    c.sampDepth=512;c.daqHz=10e6;c.fov=.02;c.gridSize=.001;
    c.alinesPerChannelPerBlock=6;c.alinesPerChannelPerFrame=9;c.alinesPerFrame=72;
    c.enabledChannelCount=4;c.soundSpeedRadiiCount=1;c.soundSpeedRadii[0]=.007;
    c.soundSpeedsCount=2;c.soundSpeeds[0]=1490;c.soundSpeeds[1]=1560;
    c.dbrSigRemove=0;c.singalImpair=0;c.sysDelay[0]=5;c.sysDelay[1]=7;
    for(int ch=0;ch<8;++ch){c.enabledChannels[ch]=ch<4;c.radiusPerChannel[ch]=.004;}
    RingTimevarLowpassConfig tv;tv.fcMaxMhz=4;tv.fcMinMhz=1;
    RingFbpSettings fbp;fbp.enabled=true;fbp.angularSamples=129;
    int ready=0,frames=0,phase=0,result=1;bool prepared=false,failed=false;
    auto fail=[&](const QString& e){std::cerr<<e.toStdString()<<'\n';failed=true;QTimer::singleShot(0,&controller,&ImagingController::stopSvc);QTimer::singleShot(2000,&app,&QCoreApplication::quit);};
    QObject::connect(&controller,&ImagingController::svcError,&app,fail);
    QObject::connect(&controller,&ImagingController::svcStatus,&app,[&](const QString&s,float){if(s==QStringLiteral("FBP 走时表和精确核准备完成"))prepared=true;});
    auto submit=[&]{QVector<float>raw(24*512),angles(24);QVector<quint8> channels(24);
        for(int j=0;j<6;++j)for(int ch=0;ch<4;++ch){int p=j*4+ch;channels[p]=ch;angles[p]=float(c.sectorStartDeg+ch*90+(frames*3+j/2)*10);
            for(int t=0;t<512;++t)raw[p*512+t]=float(sin(t*.27)*exp(-pow((t-80.)/30,2)));}
        if(!controller.submitRingBlock(raw,angles,channels,{1,0},frames+1,nullptr,frames==2))fail("submit rejected after ready");};
    QObject::connect(&controller,&ImagingController::svcReady,&app,[&]{
        ++ready;if(phase!=1||!prepared||ready!=1){fail("premature or duplicate FBP readiness");return;}submit();});
    QObject::connect(&controller,&ImagingController::ringSnapshotReady,&app,
        [&](int,quint64,bool,quint64,quint64,bool complete,int buffer){
            controller.releaseSnapshotBuffer(buffer);++frames;
            if(complete!=(frames==3)){fail("source final contract changed");return;}
            if(frames==3){result=0;phase=2;controller.stopSvc();}else submit();});
    QObject::connect(&controller,&ImagingController::svcStopped,&app,[&]{
        if(failed||phase==2){app.quit();return;}
        phase=1;if(!controller.startSvc())fail("restart failed");});
    FilteredDasConfig filtered;filtered.enabled=false;
    if(!controller.configureRing(c,tv,nullptr,filtered,fbp)||!controller.startSvc())return 2;
    QTimer::singleShot(50,&controller,&ImagingController::stopSvc);
    QTimer::singleShot(30000,&app,[&]{fail("controller integration timed out");});
    app.exec();
    if(!failed&&result==0&&ready==1&&frames==3)std::cout<<"PASS FBP cancelled startup, prepared acknowledgement, restart, 3 real service snapshots and final\n";
    else result=1;
    return result;
}
