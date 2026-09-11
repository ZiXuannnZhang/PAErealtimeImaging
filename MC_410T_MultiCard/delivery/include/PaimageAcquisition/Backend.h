#pragma once
#include "ControlSocket.h"
#include "ControlState.h"
#include "HostOutput.h"
#include "LoopLog.h"
#include "SocketReceiver.h"
namespace paimage {
// One immutable listening configuration. The owner marshals feedback to its
// controller thread and calls poll there. No legacy receiver or StartFence.
class Backend {
public:
    struct Settings {Config acquisition;int blockSize=50;std::string localIp;
        std::vector<std::string> targets;std::uint16_t dataPort=8001,feedbackPort=8000,controlPort=8080;};
    Backend(Settings,std::vector<DataProcessor*>,std::vector<FileSaver*>,TraceWriter*,TimingWriter* = nullptr,LoopLog* = nullptr);
    ~Backend();
    bool listen(std::string&);
    void requestStop();
    void stop();
    bool configure(int durationNs,int delayA,int delayB);
    bool startMeasurement(std::uint64_t);
    bool stopMeasurement();
    void feedback(int card,int type){control_.feedback(card,type);}
    void poll(){control_.poll(SocketReceiver::now());}
    bool configured()const{return control_.configured();}
    bool configuring()const{return control_.configuring();}
    const std::vector<bool>& ready()const{return control_.ready();}
    const std::vector<bool>& acknowledgements()const{return control_.acknowledgements();}
    HostOutput& output(){return output_;}
    const SocketReceiver& receiver()const{return receiver_;}
    SocketReceiver& receiver(){return receiver_;}
    std::function<void(int,int,std::int64_t)> feedbackSink;
private:
    Settings settings_;TraceWriter* trace_;TimingWriter* timing_;ControlSocket socket_;
    HostOutput output_;SocketReceiver receiver_;ControlState control_;
    bool listening_=false;std::uint64_t session_=0;
};
}
