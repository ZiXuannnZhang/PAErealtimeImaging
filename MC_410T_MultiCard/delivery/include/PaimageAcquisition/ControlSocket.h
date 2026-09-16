#pragma once
#include "SourceCore.h"
#include <functional>
#include <string>
#include <utility>
namespace paimage {
// 137360 / 1505b0: local-interface-bound, ephemeral source port, blocking
// datagram send with 500ms SO_SNDTIMEO, target port from source field 0x50.
class ControlSocket {
public:
    struct SendResult {int card=0,bytes=-1,error=0;std::uint32_t targetIPv4=0;};
    using Observer=std::function<void(const Command&,const SendResult&)>;
    using BeforeObserver=std::function<void(const Command&,int)>;
    ControlSocket()=default;~ControlSocket();
    bool open(const std::string& localIp,const std::vector<std::string>& targets,std::uint16_t targetPort,std::string& error);
    void close();
    bool send(const Command&,const std::vector<int>& cards,Observer={},BeforeObserver={});
    std::uint16_t localPort()const{return localPort_;}
    int timeoutOptionError()const{return timeoutOptionError_;}
#ifdef PAIMAGE_SOCKET_TEST_SEAM
    // TEST ONLY: deterministic failure injection; delivery builds do not
    // compile this hook or its behavior branch.
    using TestSendResultHook=std::function<void(const Command&,SendResult&)>;
    using TestSendHook=std::function<void(const Command&,const SendResult&)>;
    void setTestSendResultHook(TestSendResultHook hook){testSendResultHook_=std::move(hook);}
    void setTestSendHook(TestSendHook hook){testSendHook_=std::move(hook);}
#endif
private:
    std::uintptr_t socket_=~std::uintptr_t(0);
    std::vector<std::uint32_t> targets_;std::uint16_t targetPort_=0,localPort_=0;
    int timeoutOptionError_=-1;bool wsa_=false;
#ifdef PAIMAGE_SOCKET_TEST_SEAM
    TestSendResultHook testSendResultHook_;
    TestSendHook testSendHook_;
#endif
};
}
