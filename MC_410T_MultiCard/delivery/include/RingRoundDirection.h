#pragma once

#include "RoundIdentity.h"
#include <array>
#include <mutex>
#include <optional>

namespace paimage {
// Bind a motor setting at physical RoundStarted, before any frontend worker
// receives that round. Old groups can still be in flight after a motor update.
class RingRoundDirection {
public:
    struct Selection {
        int sectorCcw = 1;
        std::uint64_t revision = 0;
    };

    Selection request(int sectorCcw) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (requested_.sectorCcw != sectorCcw)
            requested_ = {sectorCcw, requested_.revision + 1};
        return requested_;
    }

    Selection bind(const RoundIdentity &round) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto old = findLocked(round)) return *old;
        history_[next_] = {round, requested_};
        next_ = (next_ + 1) % history_.size();
        return requested_;
    }

    std::optional<Selection> find(const RoundIdentity &round) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return findLocked(round);
    }

private:
    struct Binding { RoundIdentity round; Selection selection; };
    std::optional<Selection> findLocked(const RoundIdentity &round) const {
        if (!round.valid()) return std::nullopt;
        for (std::size_t age = 1; age <= history_.size(); ++age) {
            const auto &entry = history_[(next_ + history_.size() - age) % history_.size()];
            if (!entry.round.valid()) break;
            if (entry.round == round) return entry.selection;
        }
        return std::nullopt;
    }
    mutable std::mutex mutex_;
    Selection requested_;
    std::array<Binding, 256> history_{};
    std::size_t next_ = 0;
};
}
