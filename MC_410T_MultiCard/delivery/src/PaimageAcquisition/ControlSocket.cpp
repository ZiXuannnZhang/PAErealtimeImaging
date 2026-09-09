#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include "PaimageAcquisition/ControlSocket.h"
namespace paimage {
ControlSocket::~ControlSocket(){close();}
bool ControlSocket::open(const std::string& localIp,const std::vector<std::string>& targets,std::uint16_t port,std::string& error){
    close();sockaddr_in local{};local.sin_family=AF_INET;
    WSADATA data{};int status=WSAStartup(MAKEWORD(2,2),&data);if(status){error="WSAStartup "+std::to_string(status);return false;}wsa_=true;
    if(inet_pton(AF_INET,localIp.c_str(),&local.sin_addr)!=1||local.sin_addr.s_addr==0){error="control socket requires an explicit local interface IPv4 address";close();return false;}
    if(targets.empty()||targets.size()>32||!port){error="invalid control targets";close();return false;}
    for(const auto& ip:targets){in_addr address{};if(inet_pton(AF_INET,ip.c_str(),&address)!=1){error="invalid target IPv4";close();return false;}targets_.push_back(address.s_addr);}
    socket_=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if(socket_==INVALID_SOCKET){error="control socket "+std::to_string(WSAGetLastError());close();return false;}
    if(bind(SOCKET(socket_),reinterpret_cast<sockaddr*>(&local),sizeof(local))){error="control bind "+std::to_string(WSAGetLastError());close();return false;}
    DWORD timeout=500;
    timeoutOptionError_=setsockopt(SOCKET(socket_),SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout))?WSAGetLastError():0;
    int length=sizeof(local);if(getsockname(SOCKET(socket_),reinterpret_cast<sockaddr*>(&local),&length)==0)localPort_=ntohs(local.sin_port);
    targetPort_=port;return true;
}
void ControlSocket::close(){if(socket_!=INVALID_SOCKET){closesocket(SOCKET(socket_));socket_=INVALID_SOCKET;}
    if(wsa_){WSACleanup();wsa_=false;}targets_.clear();localPort_=targetPort_=0;timeoutOptionError_=-1;}
bool ControlSocket::send(const Command& command,const std::vector<int>& cards,Observer observer){
    if(socket_==INVALID_SOCKET)return false;
    bool success=true;
    for(int card:cards){
        if(card<0||std::size_t(card)>=targets_.size())return false;
        sockaddr_in target{};target.sin_family=AF_INET;target.sin_port=htons(targetPort_);target.sin_addr.s_addr=targets_[card];
        int count=sendto(SOCKET(socket_),reinterpret_cast<const char*>(command.data()),int(command.size()),0,reinterpret_cast<sockaddr*>(&target),sizeof(target));
        int error=count==int(command.size())?0:WSAGetLastError();
        if(count!=int(command.size()))success=false;
        if(observer)observer(command,{card,count,error,targets_[card]});
    }
    return success;
}
}
