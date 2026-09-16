#include "PaimageAcquisition/OutputQueues.h"
#include <algorithm>
#include <stdexcept>
namespace paimage {
OutputQueues::OutputQueues(int cards,int block):blockSize_(block),perCard_(cards>0?cards:0){
    if(cards<1||cards>32||block<1)throw std::invalid_argument("output queue configuration");
}
bool OutputQueues::pushCard(Frame f){
    if(!f||f->card<0||std::size_t(f->card)>=perCard_.size())return false;
    auto& depth=perCard_[f->card];if(depth>=400)return false;
    cards_.push_back(std::move(f));++depth;return true;
}
Frame OutputQueues::popCard(){
    if(cards_.empty())return {};
    auto f=std::move(cards_.front());cards_.pop_front();--perCard_[f->card];return f;
}
std::vector<SyncFrame> OutputQueues::pushSync(std::uint16_t trigger,const std::vector<Frame>& frames,bool startup){
    SyncFrame f{trigger,session_,index_/unsigned(blockSize_)+1,index_%unsigned(blockSize_),frames};++index_;
    std::vector<SyncFrame> evicted;
    if(!startup&&normal_.size()>=std::size_t(blockSize_)*2){
        const auto oldest=normal_.front().block;
        do{evicted.push_back(std::move(normal_.front()));normal_.pop_front();}
        while(!normal_.empty()&&normal_.front().block==oldest);
    }
    (startup?startup_:normal_).push_back(std::move(f));return evicted;
}
std::optional<SyncFrame> OutputQueues::popSync(){
    auto& q=startup_.empty()?normal_:startup_;if(q.empty())return {};
    auto f=std::move(q.front());q.pop_front();return f;
}
void OutputQueues::beginSession(std::uint64_t s){
    // 1430d7..1430f6 clears only sync FIFOs and advances sync session.
    // The independent saving FIFO is NOT measurement-session gated.
    session_=s;index_=0;normal_.clear();startup_.clear();
}
void OutputQueues::clearCards(){
    cards_.clear();
    std::fill(perCard_.begin(),perCard_.end(),0);
}
}
