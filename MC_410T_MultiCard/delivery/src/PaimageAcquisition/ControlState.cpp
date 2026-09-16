#include "PaimageAcquisition/ControlState.h"
#include <algorithm>
#include <stdexcept>
namespace paimage {
ControlState::ControlState(int cards,Sender send,std::function<Time()> clock):ready_(cards>0?cards:0),ack_(cards>0?cards:0),sender_(std::move(send)),clock_(std::move(clock)){
    if(cards<1||cards>32||!sender_)throw std::invalid_argument("control configuration");
    for(int i=0;i<cards;++i)allCards_.push_back(i);
}
void ControlState::setListening(bool value){
    listening_=value;configured_=configuring_=false;rounds_=0;deadline_=0;
    std::fill(ready_.begin(),ready_.end(),false);std::fill(ack_.begin(),ack_.end(),false);
}
bool ControlState::configure(Command command,Time now,bool wait){
    if(!listening_)return false;
    config_=command;waitAck_=wait;configured_=false;configuring_=true;rounds_=0;
    std::fill(ack_.begin(),ack_.end(),false);return sendRound(now);
}
bool ControlState::sendRound(Time now){
    std::vector<int> missing;for(auto i:allCards_)if(!ack_[i])missing.push_back(i);
    if(missing.empty()){configured_=true;configuring_=false;return true;}
    ++rounds_;
    if(!sender_(config_,missing)){configuring_=false;return false;}
    if(!waitAck_){std::fill(ack_.begin(),ack_.end(),true);configured_=true;configuring_=false;}
    else deadline_=(clock_?clock_():now)+1000000000; // source waits after send returns
    return true;
}
void ControlState::feedback(int card,int type){
    if(card<0||std::size_t(card)>=ack_.size())return;
    if(type==1)ready_[card]=true;
    if(type==2){ack_[card]=true;if(configuring_&&std::all_of(ack_.begin(),ack_.end(),[](bool b){return b;})){
        configured_=true;configuring_=false;}}
}
void ControlState::poll(Time now){
    if(!configuring_||now<deadline_)return;
    if(rounds_>=4){configuring_=false;return;}
    sendRound(now);
}
bool ControlState::start(const std::function<void()>& prepare,const std::function<void(bool)>& complete){
    if(!listening_||!configured_)return false;
    prepare();bool ok=sender_(startCommand(),allCards_);complete(ok);return ok;
}
bool ControlState::stop(const std::function<void()>& prepare,const std::function<void(bool)>& complete){
    if(!listening_)return false;
    prepare();bool ok=sender_(stopCommand(),allCards_);complete(ok);return ok;
}
}
