#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include "PaimageAcquisition/SocketReceiver.h"
#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>
using namespace paimage;
int main(){
 WSADATA w{};if(WSAStartup(MAKEWORD(2,2),&w))return 1;SOCKET sender=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);if(sender==INVALID_SOCKET)return 2;
 std::atomic<bool> sending{true};std::atomic<unsigned long long> sent{0},failed{0},counts[4]{},feedback{0},budgets{0};
 std::vector<SocketReceiver::Endpoint> ep;for(int i=0;i<4;++i)ep.push_back({std::uint16_t(29401+i),"127.0.0.1"});
 SocketReceiver r({4,360,32,0},ep,{29400,"127.0.0.1"},{"127.0.0.1"},nullptr,nullptr,nullptr,[](Frame){},[](auto,const auto&,bool){});
 r.ingressSink=[&](int card,const TraceRecord&){if(card>=0&&card<4)++counts[card];};r.feedbackSink=[&](int,int,Time){++feedback;};r.testBudgetHook=[&](int){++budgets;};
 std::string error;if(!r.start(error)){std::cerr<<error;return 3;}r.prepareStart(1);r.completeStart(true);
 auto send=[&](int port,int size){std::array<char,1444> b{};b[2]=7;sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(port);inet_pton(AF_INET,"127.0.0.1",&a.sin_addr);if(sendto(sender,b.data(),size,0,(sockaddr*)&a,sizeof(a))==size)++sent;else ++failed;};
 auto begin=std::chrono::steady_clock::now();std::thread hot([&]{while(sending)send(29401,1444);});
 for(int i=0;i<300;++i){for(int c=1;c<4;++c)send(29401+c,1444);send(29400,18);}
 auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
 while(std::chrono::steady_clock::now()<deadline&&(counts[1]<30||counts[2]<30||counts[3]<30||feedback<30))std::this_thread::yield();
 auto stopStart=std::chrono::steady_clock::now();r.stop();auto stopEnd=std::chrono::steady_clock::now();sending=false;hot.join();
 auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(stopEnd-begin).count();auto stopMs=std::chrono::duration_cast<std::chrono::milliseconds>(stopEnd-stopStart).count();
 std::cout<<"sent="<<sent<<" failed="<<failed<<" ingress="<<r.ingress()<<" card0="<<counts[0]<<" card1="<<counts[1]<<" card2="<<counts[2]<<" card3="<<counts[3]<<" feedback="<<feedback<<" budgetYields="<<budgets<<" durationMs="<<ms<<" stopMs="<<stopMs<<" hardErrors="<<r.hardErrors()<<"\n";
 bool ok=counts[0]>0&&counts[1]>=30&&counts[2]>=30&&counts[3]>=30&&feedback>=30&&stopMs<1000&&failed==0&&r.hardErrors()==0;
 closesocket(sender);WSACleanup();return ok?0:1;
}
