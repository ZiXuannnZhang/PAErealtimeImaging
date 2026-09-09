#pragma once
#include "SourceCore.h"
#include <deque>
#include <optional>

namespace paimage {
// Recovered queue policy only. Caller holds the corresponding source output
// mutex. Workers and host callbacks are deliberately outside this policy.
// VA 134020/144172: one FIFO, per-card limit 400, reject newest.
// VA 133d7e/13a226: separate startup FIFO (priority) and normal FIFO.
struct SyncFrame {
    std::uint16_t trigger=0;
    std::uint64_t session=0,block=0,index=0;
    std::vector<Frame> cards;
};
class OutputQueues {
public:
    explicit OutputQueues(int cards,int blockSize);
    bool pushCard(Frame);
    Frame popCard();
    // Returns exact discarded objects. Source cumulative 0x790 counts one
    // eviction event, not this number; diagnostics keep both units separate.
    std::vector<SyncFrame> pushSync(std::uint16_t,const std::vector<Frame>&,bool startup);
    std::optional<SyncFrame> popSync();
    void beginSession(std::uint64_t);
    void clearCards();
    std::size_t cardDepth() const {return cards_.size();}
    unsigned cardDepth(int card) const {return perCard_.at(card);}
    std::size_t syncDepth() const {return normal_.size()+startup_.size();}
private:
    int blockSize_;
    std::uint64_t session_=0,index_=0;
    std::vector<unsigned> perCard_;
    std::deque<Frame> cards_;
    std::deque<SyncFrame> normal_,startup_;
};
}
