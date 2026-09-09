#pragma once
#include "SourceCore.h"
#include <string>
namespace paimage {
// 137360 / 1505b0: local-interface-bound, ephemeral source port, blocking
// datagram send with 500ms SO_SNDTIMEO, target port from source field 0x50.
class ControlSocket {
public:
    struct SendResult {int card=0,bytes=-1,error=0;std::uint32_t targetIPv4=0;};
    using Observer=std::function<void(const Command&,const SendResult&)>;
    ControlSocket()=default;~ControlSocket();
    bool open(const std::string& localIp,const std::vector<std::string>& targets,std::uint16_t targetPort,std::string& error);
    void close();
    bool send(const Command&,const std::vector<int>& cards,Observer={});
    std::uint16_t localPort()const{return localPort_;}
    int timeoutOptionError()const{return timeoutOptionError_;}
private:
    std::uintptr_t socket_=~std::uintptr_t(0);
    std::vector<std::uint32_t> targets_;std::uint16_t targetPort_=0,localPort_=0;
    int timeoutOptionError_=-1;bool wsa_=false;
};
}
