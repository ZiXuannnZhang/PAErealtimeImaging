#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <mstcpip.h>
#include "PaimageAcquisition/SocketTimestamp.h"

#include <sstream>

// MinGW hides the Windows 10 20H1 declarations unless NTDDI_VERSION is set
// high enough. Keep the documented numeric ABI available for runtime probing;
// an older OS will fail the IOCTL and remains explicitly disabled.
#ifndef SIO_TIMESTAMPING
#define SIO_TIMESTAMPING _WSAIOW(IOC_VENDOR,235)
typedef struct _TIMESTAMPING_CONFIG {
    ULONG Flags;
    USHORT TxTimestampsBuffered;
} TIMESTAMPING_CONFIG, *PTIMESTAMPING_CONFIG;
#define TIMESTAMPING_FLAG_RX 0x1
#define TIMESTAMPING_FLAG_TX 0x2
#endif
#ifndef SO_TIMESTAMP
#define SO_TIMESTAMP 0x300A
#endif

namespace paimage {
namespace {
LPFN_WSARECVMSG resolveRecvMsg(SOCKET socketHandle) {
    GUID guid=WSAID_WSARECVMSG;
    LPFN_WSARECVMSG function=nullptr;
    DWORD bytes=0;
    if(WSAIoctl(socketHandle,SIO_GET_EXTENSION_FUNCTION_POINTER,&guid,sizeof(guid),
                &function,sizeof(function),&bytes,nullptr,nullptr)!=0)
        return nullptr;
    return function;
}
std::string errorText(const char* operation,int error) {
    std::ostringstream text;text<<operation<<" failed (WSA "<<error<<")";return text.str();
}
SocketTimestampResult tryHardware(SOCKET socketHandle,SocketTimestampResult result) {
#if defined(SIO_TIMESTAMPING) && defined(TIMESTAMPING_FLAG_RX)
    TIMESTAMPING_CONFIG config{};config.Flags=TIMESTAMPING_FLAG_RX;
    DWORD returned=0;
    if(WSAIoctl(socketHandle,SIO_TIMESTAMPING,&config,sizeof(config),nullptr,0,
                &returned,nullptr,nullptr)==0){
        result.enabled=true;result.selected=SocketTimestampMode::Hardware;
        result.status="hardware receive timestamping enabled via SIO_TIMESTAMPING";
        return result;
    }
    result.status=errorText("SIO_TIMESTAMPING",WSAGetLastError());
#else
    result.status="SIO_TIMESTAMPING is unavailable in the build headers";
#endif
    return result;
}
SocketTimestampResult trySoftware(SOCKET socketHandle,SocketTimestampResult result) {
#if defined(SO_TIMESTAMP)
    int enabled=1;
    if(setsockopt(socketHandle,SOL_SOCKET,SO_TIMESTAMP,reinterpret_cast<const char*>(&enabled),sizeof(enabled))==0){
        result.enabled=true;result.selected=SocketTimestampMode::Software;
        result.status="software receive timestamping enabled via SO_TIMESTAMP";
        return result;
    }
    result.status=errorText("SO_TIMESTAMP",WSAGetLastError());
#else
    result.status="SO_TIMESTAMP is unavailable in the build headers";
#endif
    return result;
}
}

const char* socketTimestampModeName(SocketTimestampMode mode) {
    switch(mode){
    case SocketTimestampMode::Software:return "software";
    case SocketTimestampMode::Hardware:return "hardware";
    case SocketTimestampMode::Auto:return "auto";
    default:return "off";
    }
}

SocketTimestampCapability querySocketTimestampCapability(std::uintptr_t rawHandle) {
    SocketTimestampCapability result;
    if(!rawHandle){result.status="socket handle is invalid";return result;}
    const SOCKET socketHandle=static_cast<SOCKET>(rawHandle);
    result.wsaRecvMsg=resolveRecvMsg(socketHandle)!=nullptr;
#if defined(SIO_TIMESTAMPING)
    result.sioTimestamping=true;
#endif
#if defined(SO_TIMESTAMP)
    result.soTimestamp=true;
#endif
    std::ostringstream status;
    status<<"WSARecvMsg="<<(result.wsaRecvMsg?"available":"unavailable")
          <<", SIO_TIMESTAMPING="<<(result.sioTimestamping?"available":"unavailable")
          <<", SO_TIMESTAMP="<<(result.soTimestamp?"available":"unavailable");
    result.status=status.str();
    return result;
}

SocketTimestampResult configureSocketTimestamp(std::uintptr_t rawHandle,SocketTimestampMode mode) {
    SocketTimestampResult result;result.requested=mode!=SocketTimestampMode::Off;result.selected=SocketTimestampMode::Off;
    if(mode==SocketTimestampMode::Off){result.status="disabled by configuration";return result;}
    if(!rawHandle){result.status="socket handle is invalid";return result;}
    const SOCKET socketHandle=static_cast<SOCKET>(rawHandle);
    const auto capability=querySocketTimestampCapability(rawHandle);
    if(!capability.wsaRecvMsg){result.status="WSARecvMsg unavailable; timestamp mode remains disabled";return result;}
    result.recvMsgFunction=reinterpret_cast<std::uintptr_t>(resolveRecvMsg(socketHandle));
    if(mode==SocketTimestampMode::Hardware){
        result=tryHardware(socketHandle,result);
    }else if(mode==SocketTimestampMode::Software){
        result=trySoftware(socketHandle,result);
    }else{
        result=tryHardware(socketHandle,result);
        if(!result.enabled){
            const std::string hardwareStatus=result.status;
            result=trySoftware(socketHandle,result);result.fallback=result.enabled;
            if(result.enabled)result.status="auto fallback to software after "+hardwareStatus;
        }
    }
    if(!result.enabled)result.recvMsgFunction=0;
    return result;
}
}
