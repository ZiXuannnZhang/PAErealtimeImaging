#include "PaimageAcquisition/OutputQueues.h"
#include <iostream>
#include <stdexcept>
using namespace paimage;
void require(bool p){if(!p)throw std::runtime_error("output queue check");}
Frame frame(int c,int t){auto f=std::make_shared<CardFrame>();f->card=c;f->trigger=t;return f;}
int main(){
    OutputQueues q(4,3);q.beginSession(7);
    for(int i=0;i<400;++i)require(q.pushCard(frame(0,i)));
    require(!q.pushCard(frame(0,400)));require(q.pushCard(frame(1,400)));
    for(int i=0;i<400;++i)require(q.popCard()->trigger==i);
    require(q.popCard()->card==1);require(!q.popCard());
    for(int i=0;i<6;++i)require(q.pushSync(i,{},false).empty());
    auto dropped=q.pushSync(6,{},false);require(dropped.size()==3);
    for(int i=0;i<3;++i)require(dropped[i].trigger==i&&dropped[i].block==1);
    require(q.pushSync(7,{},true).empty());
    auto f=q.popSync();require(f->trigger==7&&f->session==7&&f->block==3&&f->index==1);
    require(q.popSync()->trigger==3);
    q.pushCard(frame(2,888));q.beginSession(8);
    require(q.cardDepth()==1&&q.syncDepth()==0);require(q.popCard()->trigger==888);
    q.pushSync(65535,{},true);q.pushSync(0,{},false);
    require(q.popSync()->block==1);f=q.popSync();require(f->trigger==0&&f->index==1&&f->session==8);
    require(!q.popSync());std::cout<<"PASS output queue policy (not worker/GUI integration)\n";
}
