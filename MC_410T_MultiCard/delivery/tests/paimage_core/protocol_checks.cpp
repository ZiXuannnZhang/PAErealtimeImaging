#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include "PaimageAcquisition/ControlSocket.h"
#include "PaimageAcquisition/ControlState.h"
#include "PaimageAcquisition/SocketReceiver.h"
#include <chrono>
#include <iostream>
#include <stdexcept>
using namespace paimage;
void require(bool value,const char* text){if(!value)throw std::runtime_error(text);}
template<class F> bool until(F f){auto end=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(!f()&&std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(std::chrono::milliseconds(1));return f();}
int main(){try{
    WSADATA w{};require(WSAStartup(MAKEWORD(2,2),&w)==0,"WSA startup");
    SOCKET hardware=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);require(hardware!=INVALID_SOCKET,"emulator socket");
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(19080);inet_pton(AF_INET,"127.0.0.1",&address.sin_addr);
    require(bind(hardware,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,"emulator bind");
    DWORD timeout=1000;setsockopt(hardware,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<char*>(&timeout),sizeof(timeout));
    ControlSocket control;std::string error;
    require(!control.open("0.0.0.0",{"127.0.0.1"},19080,error),"source rejects unspecified control interface");
    require(control.open("127.0.0.1",{"127.0.0.1"},19080,error),"control bind");
    require(control.localPort()!=0&&control.timeoutOptionError()==0,"control socket parameters");
    std::atomic<unsigned> ready{0},ack{0},frames{0};
    SocketReceiver receiver({1,16,32,0},{{19001,"127.0.0.1"}},{19000,"127.0.0.1"},{"127.0.0.1"},nullptr,nullptr,nullptr,
        [&](Frame f){require(f->bytes.size()==128&&f->complete,"feedback data output");++frames;},{});
    receiver.feedbackSink=[&](int card,int type,long long){if(card==0){if(type==1)++ready;if(type==2)++ack;}};
    require(receiver.start(error),"receiver bind");require(receiver.feedbackReceiveBuffer()==67108864,"feedback 64MiB buffer");
    ControlState state(1,[&](const Command& cmd,const auto& cards){return control.send(cmd,cards);},SocketReceiver::now);
    state.setListening(true);
    auto readCommand=[&](Command expected){Command bytes{};sockaddr_in sender{};int size=sizeof(sender);
        int n=recvfrom(hardware,reinterpret_cast<char*>(bytes.data()),int(bytes.size()),0,reinterpret_cast<sockaddr*>(&sender),&size);
        require(n==58&&bytes==expected&&ntohs(sender.sin_port)==control.localPort(),"wire command/source port");};
    auto feedback=[&](std::vector<unsigned char> bytes){address.sin_port=htons(19000);
        require(sendto(hardware,reinterpret_cast<char*>(bytes.data()),int(bytes.size()),0,reinterpret_cast<sockaddr*>(&address),sizeof(address))==int(bytes.size()),"emulator send");};
    auto config=configCommand(20000,200,200);require(state.configure(config,SocketReceiver::now()),"config send");readCommand(config);
    feedback(std::vector<unsigned char>(18));require(until([&]{return ready==1;}),"18-byte feedback");state.feedback(0,1);require(!state.configured(),"ready is not config ACK");
    feedback(std::vector<unsigned char>(60));require(until([&]{return ack==1;}),"60-byte feedback");state.feedback(0,2);require(state.configured(),"config ACK");
    require(state.start([&]{receiver.prepareStart(1);},[&](bool ok){receiver.completeStart(ok);}),"start send");readCommand(startCommand());
    std::vector<unsigned char> bytes(132);bytes[2]=1;feedback(bytes);require(until([&]{return frames==1;}),"data carried by feedback socket");
    require(state.stop([&]{receiver.prepareStop();},[&](bool ok){receiver.completeStop(ok);}),"stop send");readCommand(stopCommand());
    bytes[2]=2;feedback(bytes);require(until([&]{return receiver.ingress()>=4;}),"stop tail observed");receiver.stop();require(frames==1,"stop tail rejected by source gate");
    control.close();closesocket(hardware);WSACleanup();
    std::cout<<"PASS real UDP protocol emulator: CONFIG/18/60/START/data-on-feedback/STOP\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
