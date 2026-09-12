#pragma once

#include <cstdint>
#include <string>

namespace paimage {

// Experimental receive timestamping. Off is the production default; enabling
// a mode only changes the options on the sockets owned by this listener.
enum class SocketTimestampMode : std::uint8_t { Off=0, Software=1, Hardware=2, Auto=3 };

struct SocketTimestampCapability {
    bool wsaRecvMsg=false;
    bool sioTimestamping=false;
    bool soTimestamp=false;
    std::string status;
};

struct SocketTimestampResult {
    bool requested=false;
    bool enabled=false;
    bool fallback=false;
    SocketTimestampMode selected=SocketTimestampMode::Off;
    std::uintptr_t recvMsgFunction=0;
    std::string status;
};

SocketTimestampCapability querySocketTimestampCapability(std::uintptr_t socketHandle);
SocketTimestampResult configureSocketTimestamp(std::uintptr_t socketHandle, SocketTimestampMode mode);
const char* socketTimestampModeName(SocketTimestampMode mode);

}
