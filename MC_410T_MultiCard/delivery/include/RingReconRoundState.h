#pragma once
#include "RoundIdentity.h"
#include <cstdint>

namespace paimage {
// Service ownership barrier. Resetting pixels must not erase the identity
// floor: late blocks from a closed round can never reopen its accumulator.
class RingReconRoundState {
public:
    enum class Admission { Invalid, Stale, Same, First, Transition };
    Admission admit(const RoundIdentity &round) {
        if (!round.valid()) return Admission::Invalid;
        if (active_.valid() && (round < active_ || (closed_ && round == active_))) {
            ++staleDrops_;
            return Admission::Stale;
        }
        if (round == active_) return Admission::Same;
        const bool first = !active_.valid();
        active_ = round;
        closed_ = false;
        if (!first) ++transitions_;
        return first ? Admission::First : Admission::Transition;
    }
    void close() { closed_ = active_.valid(); }
    void resetSession() { *this = {}; }
    RoundIdentity active() const { return active_; }
    std::uint64_t transitions() const { return transitions_; }
    std::uint64_t staleDrops() const { return staleDrops_; }
private:
    RoundIdentity active_;
    bool closed_ = false;
    std::uint64_t transitions_ = 0;
    std::uint64_t staleDrops_ = 0;
};
}
