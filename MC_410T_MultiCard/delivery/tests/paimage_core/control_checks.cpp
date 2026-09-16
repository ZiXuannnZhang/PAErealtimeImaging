#include "PaimageAcquisition/ControlState.h"
#include <iostream>
#include <stdexcept>
using namespace paimage;
void require(bool p){if(!p)throw std::runtime_error("control state check");}
int main(){
    std::vector<std::vector<int>> sends;bool sendOk=true;std::vector<int> order;
    ControlState s(4,[&](const Command&,const std::vector<int>& cards){sends.push_back(cards);order.push_back(2);return sendOk;});
    auto command=configCommand(20000,200,200);
    require(!s.configure(command,0));s.setListening(true);require(s.configure(command,0));
    for(int i=0;i<4;++i)s.feedback(i,1);require(!s.configured());
    s.feedback(0,2);s.feedback(2,2);s.poll(999999999);require(sends.size()==1);
    s.poll(1000000000);require(sends.back()==std::vector<int>({1,3}));
    s.feedback(1,2);s.feedback(3,2);require(s.configured()&&!s.configuring());
    order.clear();require(s.start([&]{order.push_back(1);},[&](bool ok){require(ok);order.push_back(3);}));
    require(order==std::vector<int>({1,2,3}));
    sendOk=false;order.clear();require(!s.stop([&]{order.push_back(1);},[&](bool ok){require(!ok);order.push_back(3);}));
    require(order==std::vector<int>({1,2,3}));
    require(!s.configure(command,0)&&!s.configuring()&&!s.configured());
    sendOk=true;sends.clear();require(s.configure(command,0));
    for(int i=1;i<=4;++i)s.poll(i*1000000000ll);
    require(sends.size()==4&&s.rounds()==4&&!s.configuring()&&!s.configured());
    require(s.configure(command,0,false)&&s.configured());
    std::cout<<"PASS control policy: separate ACKs, selective four rounds, deadlines, failure order\n";
}
