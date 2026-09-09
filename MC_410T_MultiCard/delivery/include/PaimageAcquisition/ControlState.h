#pragma once
#include "SourceCore.h"
#include <string>
namespace paimage {
// Controller-thread state. Host marshals feedback to the same thread. A host
// timer represents the source condition-variable deadline; no UI blocking wait.
// Source VA 131550: four sends at most, only unacknowledged targets; a send
// failure aborts immediately. 18-byte ready and 60-byte config ACK are separate.
class ControlState {
public:
    using Sender=std::function<bool(const Command&,const std::vector<int>&)>;
    explicit ControlState(int cards,Sender,std::function<Time()> clock={});
    void setListening(bool);
    bool configure(Command,Time now,bool waitForAck=true);
    void feedback(int card,int type);
    void poll(Time now);
    bool start(const std::function<void()>& prepare,const std::function<void(bool)>& complete);
    bool stop(const std::function<void()>& prepare,const std::function<void(bool)>& complete);
    bool configured()const{return configured_;}
    bool configuring()const{return configuring_;}
    unsigned rounds()const{return rounds_;}
    Time deadline()const{return deadline_;}
    const std::vector<bool>& ready()const{return ready_;}
    const std::vector<bool>& acknowledgements()const{return ack_;}
private:
    bool sendRound(Time);
    std::vector<int> allCards_;
    std::vector<bool> ready_,ack_;Sender sender_;Command config_{};
    std::function<Time()> clock_;
    bool listening_=false,configured_=false,configuring_=false,waitAck_=true;
    unsigned rounds_=0;Time deadline_=0;
};
}
