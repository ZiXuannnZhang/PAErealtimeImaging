#include "ImagingController.h"
#include <QCoreApplication>
#include <QTimer>
#include <QProcess>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#include <cmath>
#include <iostream>
#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>
#include <windows.h>

// The input mutex is held by another thread before any submission, so the
// controller cannot publish or receive an acknowledgement while filling the
// eight-entry queue. This makes queue-full independent of GPU timing.
static int burstTest(QCoreApplication &app)
{
    ImagingController controller;
    RingReconCudaConfig c; ring_recon_cuda_set_defaults(&c);
    c.sampDepth=512; c.daqHz=10e6; c.fov=.02; c.gridSize=.001;
    c.alinesPerChannelPerBlock=6; c.alinesPerChannelPerFrame=9; c.alinesPerFrame=72;
    c.enabledChannelCount=4; c.soundSpeedRadiiCount=1; c.soundSpeedRadii[0]=.007;
    c.soundSpeedsCount=2; c.soundSpeeds[0]=1490; c.soundSpeeds[1]=1560;
    c.dbrSigRemove=0; c.singalImpair=0; c.sysDelay[0]=5; c.sysDelay[1]=7;
    for (int ch=0; ch<8; ++ch) { c.enabledChannels[ch]=ch<4; c.radiusPerChannel[ch]=.004; }
    RingTimevarLowpassConfig tv; tv.fcMaxMhz=4; tv.fcMinMhz=1;
    RingFbpSettings fbp; fbp.enabled=true; fbp.angularSamples=129;
    bool failed=false, done=false, nextSubmitted=false, resetSubmitted=false;
    bool recoveryComplete=false, resetComplete=false;
    bool terminalEvidence=false, wholeRoundEvidence=false;
    QString evidenceEpoch;
    std::vector<quint64> indices, acknowledgements;
    auto fail=[&](const QString &message) {
        if (failed) return;
        failed=true; std::cerr << message.toStdString() << '\n';
        controller.stopSvc(); QTimer::singleShot(2000, &app, &QCoreApplication::quit);
    };
    auto submit=[&](int seq, paimage::RoundIdentity round, bool complete, quint64 *index) {
        QVector<float> raw(24*512), angles(24); QVector<quint8> channels(24);
        const int roundSeq = round.roundGeneration == 3 ? seq - 10 : seq;
        for (int j=0; j<6; ++j) for (int ch=0; ch<4; ++ch) {
            int p=j*4+ch; channels[p]=ch; angles[p]=float(ch*90+(roundSeq*3+j/2)*10);
            for (int t=0; t<512; ++t)
                raw[p*512+t]=float((seq+1)*sin(t*.27)*exp(-pow((t-80.)/30,2)));
        }
        std::uint64_t id=0;
        bool accepted=controller.submitRingBlock(raw,angles,channels,round,seq,&id,complete);
        if (index) *index=id;
        return accepted;
    };
    auto submitRound=[&](quint64 generation) {
        for (int i=0; i<3; ++i) {
            const int seq = generation == 3 ? 10 + i : i;
            quint64 id=0;
            if (!submit(seq,{10,generation},i==2,&id) || id<=indices.back()) return false;
            indices.push_back(id);
        }
        return true;
    };
    auto maybeReset=[&] {
        if (acknowledgements.size()!=11 || !recoveryComplete || resetSubmitted) return;
        resetSubmitted=true;
        if (!controller.sendRingReset() || !submitRound(4)) fail("reset and reused block sequence failed");
    };
    QObject::connect(&controller,&ImagingController::svcError,&app,fail);
    QObject::connect(&controller,&ImagingController::svcReady,&app,[&] {
        HANDLE held=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        HANDLE release=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        std::atomic<bool> acquired{false};
        std::thread blocker([&] {
            ring_input::Mutex mutex;
            acquired=mutex.lock(5000)==ring_input::Result::Ok;
            SetEvent(held); WaitForSingleObject(release,5000);
            if (acquired) mutex.unlock();
        });
        if (WaitForSingleObject(held,5000)!=WAIT_OBJECT_0 || !acquired) {
            SetEvent(release); blocker.join(); CloseHandle(held); CloseHandle(release);
            fail("could not hold input mutex"); return;
        }
        bool admissionOk=true;
        for (int seq=0; seq<8; ++seq) {
            quint64 id=0;
            if (!submit(seq,{10,1},false,&id) || !id) admissionOk=false;
            indices.push_back(id);
        }
        quint64 rejected=999;
        if (submit(8,{10,1},true,&rejected) || rejected!=0) admissionOk=false;
        // An entire subsequent physical round is rejected while capacity is held.
        for (int seq=0; seq<3; ++seq)
            if (submit(seq,{10,2},seq==2,nullptr)) admissionOk=false;
        SetEvent(release); blocker.join(); CloseHandle(held); CloseHandle(release);
        if (!admissionOk) fail("queue admission/full contract failed");
    });
    QObject::connect(&controller,&ImagingController::svcStatus,&app,[&](const QString &s,float) {
        const QString obsPrefix=QStringLiteral("[RingSHMObs] ");
        if (s.startsWith(obsPrefix)) {
            const auto obj=QJsonDocument::fromJson(s.mid(obsPrefix.size()).toUtf8()).object();
            if (obj.value("kind").toString()=="capacity_rejected") {
                const QString epoch=obj.value("input_epoch").toString();
                if (epoch.isEmpty() || (!evidenceEpoch.isEmpty() && epoch!=evidenceEpoch)
                    || obj.value("measurement_session").toString()!="10"
                    || !obj.value("reconstruction_complete").isBool()
                    || obj.value("reconstruction_complete").toBool()
                    || !obj.value("source_terminal").toBool()) { fail("invalid capacity evidence"); return; }
                evidenceEpoch=epoch;
                const QString round=obj.value("round_generation").toString();
                const int first=obj.value("first_block_index").toInt(-1);
                const int last=obj.value("last_block_index").toInt(-1);
                if (round=="1" && first==8 && last==8 && !terminalEvidence) terminalEvidence=true;
                else if (round=="2" && first==0 && last==2 && !wholeRoundEvidence) wholeRoundEvidence=true;
                else { fail("unexpected capacity evidence"); return; }
            }
        }
        const QString prefix=QStringLiteral("[RingInput] acknowledged id=");
        if (!s.startsWith(prefix) || failed) return;
        bool ok=false; quint64 id=s.mid(prefix.size()).toULongLong(&ok);
        if (!ok) { fail("invalid acknowledgement"); return; }
        acknowledgements.push_back(id);
        if (acknowledgements.size()<=indices.size() &&
            id!=indices[acknowledgements.size()-1]) { fail("acknowledgements out of order"); return; }
        if (acknowledgements.size()==8 && !nextSubmitted) {
            nextSubmitted=true;
            if (!submitRound(3)) { fail("next round was not admitted"); return; }
        }
        maybeReset();
        if (acknowledgements.size()==14 && resetComplete) { done=true; controller.stopSvc(); }
    });
    QObject::connect(&controller,&ImagingController::ringSnapshotReady,&app,
        [&](int,quint64,bool,quint64 session,quint64 round,bool complete,int buffer) {
            controller.releaseSnapshotBuffer(buffer);
            if (session==10 && round==1 && complete) { fail("incomplete round became complete"); return; }
            if (session==10 && round==3 && complete) {
                recoveryComplete=true;
                maybeReset();
            }
            if (session==10 && round==4 && complete) {
                resetComplete=true;
                if (acknowledgements.size()==14) { done=true; controller.stopSvc(); }
            }
        });
    QObject::connect(&controller,&ImagingController::svcStopped,&app,[&] { app.quit(); });
    if (!controller.configureRing(c,tv,nullptr,fbp) || !controller.startSvc()) return 2;
    QTimer::singleShot(50000,&app,[&] { fail("burst integration timed out"); });
    app.exec();
    if (!failed && done && acknowledgements.size()==14 && terminalEvidence && wholeRoundEvidence) {
        std::cout << "PASS real service burst, queue full, ordered acknowledgements, next-round recovery and reset\n";
        return 0;
    }
    return 1;
}

// Exercise the production controller and a real service process. The test seam
// suppresses only one ring_block_ready send, after the shared slot is published.
static int faultsTest(QCoreApplication &app)
{
    ImagingController controller;
    RingReconCudaConfig c; ring_recon_cuda_set_defaults(&c);
    c.sampDepth=512; c.daqHz=10e6; c.fov=.02; c.gridSize=.001;
    c.alinesPerChannelPerBlock=6; c.alinesPerChannelPerFrame=9; c.alinesPerFrame=72;
    c.enabledChannelCount=4; c.soundSpeedRadiiCount=1; c.soundSpeedRadii[0]=.007;
    c.soundSpeedsCount=2; c.soundSpeeds[0]=1490; c.soundSpeeds[1]=1560;
    c.dbrSigRemove=0; c.singalImpair=0; c.sysDelay[0]=5; c.sysDelay[1]=7;
    for (int ch=0; ch<8; ++ch) { c.enabledChannels[ch]=ch<4; c.radiusPerChannel[ch]=.004; }
    RingTimevarLowpassConfig tv; tv.fcMaxMhz=4; tv.fcMinMhz=1;
    RingFbpSettings fbp; fbp.enabled=true; fbp.angularSamples=129;
    int phase=0; bool failed=false, done=false, inspected=false, retried=false, completed=false;
    quint64 first=0, second=0, oldEpoch=0; std::vector<quint64> expected, acks;
    auto fail=[&](const QString &message) {
        if (failed) return;
        failed=true; std::cerr << message.toStdString() << '\n';
        controller.stopSvc(); QTimer::singleShot(2000,&app,&QCoreApplication::quit);
    };
    auto submit=[&](int seq, quint64 generation, bool complete, quint64 &id) {
        QVector<float> raw(24*512,float(seq+1)), angles(24); QVector<quint8> channels(24);
        for (int p=0; p<24; ++p) { channels[p]=p%4; angles[p]=float((p%4)*90+(seq*3+p/8)*10); }
        std::uint64_t index=0;
        bool ok=controller.submitRingBlock(raw,angles,channels,{20,generation},seq,&index,complete);
        id=index; return ok && id;
    };
    QObject::connect(&controller,&ImagingController::svcError,&app,[&](const QString &message) {
        if (phase!=1) fail(message); // The test deliberately kills its own child in phase 1.
    });
    QObject::connect(&controller,&ImagingController::svcReady,&app,[&] {
        if (phase==0) {
            controller.testFailNextRingNotifications(1);
            if (!submit(0,1,false,first)) { fail("first block rejected"); return; }
            expected.push_back(first);
        } else if (phase==2) {
            ring_input::NativeMapping peer; ring_input::Mutex mutex; ring_input::View view;
            if (peer.attach()!=ring_input::Result::Ok || mutex.lock(1000)!=ring_input::Result::Ok) {
                fail("could not inspect restarted mapping"); return;
            }
            bool valid=ring_input::layout(peer.data(),peer.capacity(),true,view) && view.meta->epoch!=oldEpoch;
            mutex.unlock();
            if (!valid) { fail("restart did not create a new epoch"); return; }
            for (int seq=0; seq<3; ++seq) {
                quint64 id=0; if (!submit(seq,2,seq==2,id)) { fail("restart block rejected"); return; }
                expected.push_back(id);
            }
        } else fail("unexpected readiness");
    });
    QObject::connect(&controller,&ImagingController::svcStatus,&app,[&](const QString &s,float) {
        const QString failure=QStringLiteral("[RingInput] notification_failed id=");
        const QString sent=QStringLiteral("[RingInput] notification_sent id=");
        const QString ack=QStringLiteral("[RingInput] acknowledged id=");
        if (s.startsWith(failure) && phase==0) {
            bool ok=false; const quint64 id=s.mid(failure.size()).toULongLong(&ok);
            ring_input::NativeMapping peer; ring_input::Mutex mutex; ring_input::View view;
            if (!ok || id!=first || peer.attach()!=ring_input::Result::Ok ||
                mutex.lock(1000)!=ring_input::Result::Ok) { fail("failed notification inspection setup"); return; }
            bool valid=ring_input::layout(peer.data(),peer.capacity(),true,view);
            if (valid) {
                oldEpoch=view.meta->epoch;
                const auto ready=uint32_t(RingInputSlotState::Ready);
                valid=view.meta->state==ready && view.meta->publication_id==first &&
                      view.meta->acknowledged_id!=first && view.meta->block_seq==0 &&
                      view.meta->measurement_session==20 && view.meta->round_generation==1;
                const auto bytes=size_t(view.header->block_size)*sizeof(float)+
                    size_t(view.header->alines)*sizeof(float)+size_t(view.header->alines);
                std::vector<unsigned char> before(view.data,view.data+bytes);
                const auto *raw=reinterpret_cast<const float *>(view.data);
                const auto *angles=reinterpret_cast<const float *>(view.data+ringAnglesOffset(int(view.header->block_size)));
                const auto *channels=view.data+ringChannelsOffset(int(view.header->block_size),int(view.header->alines));
                for (int i=0; i<24*512; ++i) valid=valid && raw[i]==1.0f;
                for (int p=0; p<24; ++p)
                    valid=valid && angles[p]==float((p%4)*90+(p/8)*10) && channels[p]==p%4;
                valid=valid && submit(1,1,false,second) && second>first;
                valid=valid && view.meta->state==ready && view.meta->publication_id==first &&
                    view.meta->acknowledged_id!=first && std::equal(before.begin(),before.end(),view.data);
            }
            mutex.unlock();
            if (!valid) { fail("failed notification changed or acknowledged the published slot"); return; }
            expected.push_back(second); inspected=true;
        } else if (s.startsWith(sent) && phase==0) {
            bool ok=false; const quint64 id=s.mid(sent.size()).toULongLong(&ok);
            if (ok && id==first && inspected) retried=true;
        } else if (s.startsWith(ack)) {
            bool ok=false; const quint64 id=s.mid(ack.size()).toULongLong(&ok);
            if (!ok || acks.size()>=expected.size() || id!=expected[acks.size()]) {
                fail("unexpected acknowledgement"); return;
            }
            acks.push_back(id);
            if (phase==0 && acks.size()==2) {
                if (!inspected || !retried) { fail("retry did not retain publication identity"); return; }
                QProcess *child=controller.findChild<QProcess *>();
                if (!child || child->state()!=QProcess::Running) { fail("service child missing"); return; }
                phase=1; child->kill();
            }
            if (phase==2 && acks.size()==5 && completed) { done=true; phase=3; controller.stopSvc(); }
        }
    });
    QObject::connect(&controller,&ImagingController::ringSnapshotReady,&app,
        [&](int,quint64,bool,quint64 session,quint64 round,bool complete,int buffer) {
            controller.releaseSnapshotBuffer(buffer);
            if (phase==2 && (session!=20 || round!=2)) { fail("old epoch snapshot after restart"); return; }
            if (phase==2 && complete) {
                completed=true;
                if (acks.size()==5) { done=true; phase=3; controller.stopSvc(); }
            }
        });
    QObject::connect(&controller,&ImagingController::svcStopped,&app,[&] {
        if (failed || phase==3) { app.quit(); return; }
        if (phase!=1) { fail("unexpected service stop"); return; }
        QTimer::singleShot(0,&app,[&] {
            phase=2;
            if (!controller.startSvc()) fail("restart after crash failed");
        });
    });
    if (!controller.configureRing(c,tv,nullptr,fbp) || !controller.startSvc()) return 2;
    QTimer::singleShot(50000,&app,[&] { fail("fault integration timed out"); });
    app.exec();
    if (!failed && done && acks.size()==5) {
        std::cout << "PASS failed notification preserves slot, retry, child crash and restart\n";
        return 0;
    }
    return 1;
}

// Real controller -> ZMQ -> deployed ImagingSvc -> CUDA -> SHM readiness test.
struct RingLayoutRegression {
    static int run()
    {
        ImagingController ctrl;
        RingReconCudaConfig cfg{};
        ring_recon_cuda_set_defaults(&cfg);
        cfg.sampDepth = 512;
        cfg.daqHz = 10e6;
        cfg.gridSize = .001;
        cfg.enabledChannelCount = 1;
        cfg.alinesPerChannelPerBlock = 4;
        cfg.sectorStartDeg = 180;
        cfg.sectorCcw = 0;
        for (int ch = 0; ch < 8; ++ch) cfg.enabledChannels[ch] = ch == 0;
        RingTimevarLowpassConfig tv;
        tv.fcMaxMhz = 4; tv.fcMinMhz = 1;
        int emitted = 0, restart = 0, errors = 0, directions = 0;
        bool pixelsCorrect = true;
        int lastSeq = 0;
        QObject::connect(&ctrl, &ImagingController::ringConfigChangedWhileRunning, [&]{ ++restart; });
        QObject::connect(&ctrl, &ImagingController::ringDirectionChanged, [&](int){ ++directions; });
        QObject::connect(&ctrl, &ImagingController::svcError, [&](const QString &){ ++errors; });
        QObject::connect(&ctrl, &ImagingController::ringSnapshotReady,
            [&](int seq, quint64, bool, quint64, quint64, bool, int buffer){
                ++emitted;
                lastSeq = seq;
                const auto *data = ctrl.snapshotBuffer(buffer);
                const auto n = ctrl.m_ringLayout.bytes.frameBytes / sizeof(float);
                pixelsCorrect = pixelsCorrect && data && std::all_of(data, data + n,
                    [seq](float value){ return value == float(seq); });
                ctrl.releaseSnapshotBuffer(buffer);
            });
        auto configure = [&](int nx, int alines){
            cfg.fov = (nx - .25) * cfg.gridSize;
            cfg.alinesPerChannelPerBlock = alines;
            return ctrl.configureRing(cfg, tv);
        };
        const auto mapping = (QStringLiteral("Local\ring-layout-") + QUuid::createUuid().toString()).toStdWString();
        auto start = [&]{
            // Use the real native mapping/worker and bypass process startup and GPU.
            if (!ctrl.setupRingSharedMemory(ctrl.m_pendingRingLayout, mapping.c_str())) return false;
            ctrl.m_svcResourcesActive = true;
            ctrl.m_suppressSvcCrashError = false;
            ctrl.m_running = true;
            ctrl.m_ringWorkerStop = false;
            ctrl.m_ringAdmission = true;
            return true;
        };
        auto queue = [&](int seq){
            auto *h = static_cast<RingImagingShmHeader *>(ctrl.m_ringMapping.data());
            auto *pixels = reinterpret_cast<float *>(reinterpret_cast<unsigned char *>(h + 1) +
                ringFramesOffset(h->block_size, h->alines));
            std::fill_n(pixels, size_t(h->frame_size) * 2, float(seq));
            h->frame_ready = 1; h->frame_seq = seq;
            ctrl.startRingFrameWorker(seq, seq, true, {1, 0}, false);
        };
        auto fail = [](const char *why){ std::cerr << "R4: " << why << '\n'; return 1; };
        if (!configure(5, 4) || !start()) return fail("initial mapping");
        ctrl.applyMotorRingDirection(1);
        if (restart || directions != 1 || !ctrl.m_ringAdmission) return fail("direction-only change rebuilt mapping");
        for (int i = 0; i < 2; ++i) {
            std::mutex mutex;
            std::condition_variable cv;
            bool entered = false, proceed = false;
            ctrl.m_testBeforeRingCopy = [&]{
                std::unique_lock<std::mutex> lock(mutex);
                entered = true;
                cv.notify_one();
                cv.wait(lock, [&]{ return proceed; });
            };
            const int oldNx = ctrl.ringDisplayNx();
            const int oldSeq = 31 + i;
            queue(oldSeq);
            bool parked;
            {
                std::unique_lock<std::mutex> lock(mutex);
                parked = cv.wait_for(lock, std::chrono::seconds(3), [&]{ return entered; });
            }
            const int newNx = i == 0 ? 23 : 3;
            const bool pending = parked && configure(newNx, i == 0 ? 18 : 2) &&
                restart == i + 1 && !ctrl.m_ringAdmission && ctrl.ringDisplayNx() == oldNx;
            {
                std::lock_guard<std::mutex> lock(mutex);
                proceed = true;
            }
            cv.notify_one();
            if (i == 0) ctrl.stopSvc();
            else ctrl.onSvcErrorOccurred(QProcess::FailedToStart);
            ctrl.m_testBeforeRingCopy = {};
            const auto &old = ctrl.m_dispBuf[0];
            if (!pending || old.size() != size_t(2 * oldNx * oldNx) ||
                !std::all_of(old.begin(), old.end(), [oldSeq](float v){ return v == float(oldSeq); }))
                return fail("in-flight request did not read precisely the old mapping");
            if (i == 1 && !configure(newNx, 2)) return fail("reconfigure after service error");
            if (ctrl.m_ringWorker.joinable() || ctrl.m_ringMapping.data() || !start())
                return fail("old worker or mapping survived restart");
            const int before = emitted;
            const int newSeq = 81 + i;
            queue(newSeq);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (emitted == before && std::chrono::steady_clock::now() < deadline) {
                QCoreApplication::sendPostedEvents(&ctrl, QEvent::MetaCall);
                std::this_thread::yield();
            }
            if (emitted != before + 1 || lastSeq != newSeq || !pixelsCorrect || ctrl.ringDisplayNx() != newNx)
                return fail("stale notification or incorrect new pixels/grid");
            // Join the completed worker before installing the next copy barrier.
            ctrl.stopRingWorker();
            ctrl.m_ringWorkerStop = false;
            ctrl.m_dispActive = -1;
        }
        ctrl.stopSvc();
        if (errors != 1) return fail("expected one service error");
        std::cout << "PASS R4 mapping growth/shrink, in-flight snapshots and recovery after service error\n";
        return 0;
    }
};

int main(int argc,char**argv){
    QCoreApplication app(argc,argv);
    if (app.arguments().contains(QStringLiteral("--layout"))) return RingLayoutRegression::run();
    if (app.arguments().contains(QStringLiteral("--burst"))) return burstTest(app);
    if (app.arguments().contains(QStringLiteral("--faults"))) return faultsTest(app);
    ImagingController controller;
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
        if(!controller.submitRingBlock(raw,angles,channels,{1,0},frames,nullptr,frames==2))fail("submit rejected after ready");};
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
    if(!controller.configureRing(c,tv,nullptr,fbp)||!controller.startSvc())return 2;
    QTimer::singleShot(50,&controller,&ImagingController::stopSvc);
    QTimer::singleShot(30000,&app,[&]{fail("controller integration timed out");});
    app.exec();
    if(!failed&&result==0&&ready==1&&frames==3)std::cout<<"PASS FBP cancelled startup, prepared acknowledgement, restart, 3 real service snapshots and final\n";
    else result=1;
    return result;
}
