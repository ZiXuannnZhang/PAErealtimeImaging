#include "NetworkController.h"
#include "DiagnosticRecorder.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QThreadPool>
#include <QTemporaryDir>
#include <QTextStream>
#include <QDir>
#include <QFile>
#include <ws2tcpip.h>

namespace {
struct Cards {
    QVector<SOCKET> sockets;
    QVector<int> configs;
    ~Cards() { for (SOCKET s : sockets) closesocket(s); }
    bool open(int n) {
        configs.fill(0, n);
        for (int i=0;i<n;++i) {
            SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if(s==INVALID_SOCKET) return false;
            sockets.append(s);
            sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_port=htons(8080);
            inet_pton(AF_INET, qPrintable(QString("127.0.0.%1").arg(i+1)), &addr.sin_addr);
            if(bind(s,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))!=0) return false;
            u_long nonblocking=1; ioctlsocket(s,FIONBIO,&nonblocking);
        }
        return true;
    }
    static void feedback(SOCKET s, int length) {
        sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_port=htons(8000);
        inet_pton(AF_INET,"127.0.0.1",&addr.sin_addr);
        QByteArray bytes(length,'\0');
        sendto(s,bytes.constData(),bytes.size(),0,reinterpret_cast<sockaddr*>(&addr),sizeof(addr));
    }
    void pump(bool fifthFails) {
        for(int i=0;i<sockets.size();++i) {
            char bytes[128]; sockaddr_in source{}; int len=sizeof(source);
            int n=recvfrom(sockets[i],bytes,sizeof(bytes),0,reinterpret_cast<sockaddr*>(&source),&len);
            if(n==58 && static_cast<unsigned char>(bytes[4])==2) {
                ++configs[i];
                if(fifthFails && i==4) continue;
                if(i==0 && configs[i]==1) feedback(sockets[i],18);
                feedback(sockets[i],60);
                feedback(sockets[i],60); // duplicate evidence
            }
        }
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
};
bool require(bool yes, const QString &text) {
    if(!yes) QTextStream(stderr) << "FAIL " << text << Qt::endl;
    return yes;
}
bool receptionDuringExport(bool withExport, const QString &output)
{
    NetworkController controller;
    AcqConfig config;
    config.nCards=4; config.acqTimeNs=16000; config.localBindIP="127.0.0.1";
    config.targetIPs={"127.0.0.1","127.0.0.2","127.0.0.3","127.0.0.4"};
    controller.setDiagnosticContext(withExport ? "replay-export" : "replay-baseline", "test_loopback");
    if(!require(controller.start(config),"reception start")) return false;
    controller.setMeasureEnabled(true);
    SOCKET sender=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    std::future<DiagnosticRecorder::ExportResult> job;
    QElapsedTimer timer; timer.start();
    constexpr int triggers=80;
    const int bytesPerTrigger=config.samplesPerTrig()*config.bytesPerSamplePair();
    for(int t=0;t<triggers;++t) {
        while(timer.elapsed()<t*25) { QCoreApplication::processEvents(); QThread::msleep(1); }
        if(withExport && t==40) {
            auto *r=DiagnosticRecorder::instance();
            for(int i=0;i<2000;++i) r->recordEvent("export-load",QString::number(i));
            job=r->exportRunAsync(output);
        }
        for(int card=0;card<4;++card) {
            sockaddr_in to{}; to.sin_family=AF_INET; to.sin_port=htons(8001+card);
            inet_pton(AF_INET,"127.0.0.1",&to.sin_addr);
            for(int p=0;p<config.packetsPerTrig();++p) {
                const int length=qMin(UDP_PAYLOAD_BYTES,bytesPerTrigger-p*UDP_PAYLOAD_BYTES);
                QByteArray packet(4+length,'\0');
                quint16 seq=static_cast<quint16>(t*config.packetsPerTrig()+p);
                packet[0]=seq&255; packet[1]=(seq>>8)&255; packet[2]=t&255; packet[3]=(t>>8)&255;
                if(sendto(sender,packet.constData(),packet.size(),0,reinterpret_cast<sockaddr*>(&to),sizeof(to))!=packet.size()) {
                    closesocket(sender); return require(false,"replay send failed");
                }
            }
        }
    }
    closesocket(sender);
    QElapsedTimer settle; settle.start();
    while(settle.elapsed()<400) { QCoreApplication::processEvents(); QThread::msleep(2); }
    const auto stats=controller.getAllCardStats();
    bool ok=stats.size()==4;
    for(const auto &card:stats) {
        ok=require(card.triggersComplete==triggers && card.packetsDropped==0 && card.triggersPartial==0,
            QString("replay card=%1 triggers=%2 dropped=%3 partial=%4")
                .arg(card.cardId).arg(card.triggersComplete).arg(card.packetsDropped).arg(card.triggersPartial)) && ok;
    }
    if(withExport) ok=require(job.get().success,"export during reception") && ok;
    if (ok) QTextStream(stdout) << "PASS replay " << (withExport?"with export":"baseline") << ": 4 cards x 80 triggers, 40Hz" << Qt::endl;
    return ok;
}
}

class NetworkDiagnosticTestAccess {
public:
    static bool exercise(int n) {
        Cards cards;
        if(!require(cards.open(n),"loopback ports 8080 unavailable")) return false;
        NetworkController c;
        c.setDiagnosticContext(QString("test-%1-cards").arg(n),"test_explicit_loopback");
        c.m_config.nCards=n;
        c.m_config.localBindIP="127.0.0.1";
        for(int i=0;i<n;++i) c.m_targetIPs.append(QString("127.0.0.%1").arg(i+1));
        c.m_cardsReady.assign(n,false);
        c.m_cardDiagnostics.resize(n);
        if(!require(c.initControlSocket() && c.initFeedbackListener(),"control/feedback startup")) return false;
        for(int i=0;i<qMin(n,4);++i) Cards::feedback(cards.sockets[i],18);
        if(n==5) c.markCardReady(4,false); // exercise the existing ARP-ready branch without sending ARP
        QElapsedTimer timer; timer.start();
        while(!c.isAllCardsReady() && timer.elapsed()<2000) cards.pump(n==5);
        if(!require(c.isAllCardsReady(),"ready packets not processed")) return false;
        QByteArray expected=QByteArray::fromHex("fafafafa02000000fa000fa000007d");
        expected.append(43,'\0');
        if(!require(c.buildConfigPacket(16000,500,1000)==expected,"58 byte wire protocol changed")) return false;
        if(!require(c.sendConfigCommand(16000,500,1000,"test"),"configuration rejected")) return false;
        timer.restart();
        while(c.m_configPhase==NetworkController::ConfigPhase::WaitingAck && timer.elapsed()<6000) cards.pump(n==5);
        const auto wanted=n==5 ? NetworkController::ConfigPhase::Failed : NetworkController::ConfigPhase::Confirmed;
        if(!require(c.m_configPhase==wanted,"configuration outcome incorrect")) return false;
        for(int i=0;i<4;++i)
            if(!require(c.m_configAck[i] && c.cardDiagnosticState(i)=="confirmed","confirmed card marked failed")) return false;
        if(n==5 && !require(cards.configs[4]==4 && c.cardDiagnosticState(4)=="failed","fifth-card retry/diagnostic incorrect")) return false;
        Cards::feedback(cards.sockets[0],60); // late feedback after completion
        SOCKET unknown=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
        sockaddr_in source{}; source.sin_family=AF_INET;
        inet_pton(AF_INET,"127.0.0.99",&source.sin_addr);
        bind(unknown,reinterpret_cast<sockaddr*>(&source),sizeof(source));
        Cards::feedback(unknown,60); closesocket(unknown);
        timer.restart(); while(timer.elapsed()<60) cards.pump(n==5);
        QTextStream(stdout) << "PASS " << n << " targets: " << (n==5?"fifth timeout; first four confirmed":"all confirmed") << Qt::endl;
        return true;
    }
};

int main(int argc, char **argv) {
    QCoreApplication app(argc,argv);
    WSADATA winsock{};
    if(WSAStartup(MAKEWORD(2,2),&winsock)!=0) return 2;
    QTemporaryDir temporary;
    DiagnosticRecorder::Options options;
    options.rootDirectory=temporary.path(); options.noiseBurst=0;
    auto *recorder=DiagnosticRecorder::initialize(options);
    const bool ok=NetworkDiagnosticTestAccess::exercise(4) && NetworkDiagnosticTestAccess::exercise(5)
        && receptionDuringExport(false,QString())
        && receptionDuringExport(true,QDir(temporary.path()).filePath("during-reception.zip"));
    QThreadPool::globalInstance()->waitForDone();
    const auto result=recorder->exportRun(QDir(temporary.path()).filePath("control-test.zip"));
    QFile evidence(QDir(recorder->runDirectory()).filePath("events.jsonl"));
    const bool readable=evidence.open(QIODevice::ReadOnly);
    const QByteArray events=evidence.readAll();
    const bool evidenceOk=require(readable && events.contains("config_failed")
        && events.contains("config_confirmed") && events.contains("test-5-cards")
        && events.contains("127.0.0.5") && events.contains("retry_reset"),
        "configuration evidence was not persisted");
    evidence.close();
    DiagnosticRecorder::shutdown();
    WSACleanup();
    return ok && result.success && evidenceOk ? 0 : 1;
}
