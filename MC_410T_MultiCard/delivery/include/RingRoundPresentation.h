#pragma once

#include "RoundIdentity.h"
#include "PaimageAcquisition/AutoSaveRoundCoordinator.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>

namespace paimage {

// A committed data-plane auto-save transition is indexed by the old physical
// round.  The last snapshot of that old round is the only event allowed to
// apply the next presentation target.
struct RingPresentationTransition {
    RoundIdentity oldRound;
    RoundIdentity nextRound;
    AutoSaveRoundCoordinator::CommitResult commit;

    static bool fromCommit(const AutoSaveRoundCoordinator::CommitResult &commit,
                           RingPresentationTransition &out) noexcept;
};

class RingRoundPresentationState final {
public:
    enum class RegisterResult : std::uint8_t {
        Registered,
        Duplicate,
        Invalid
    };

    enum class SnapshotAction : std::uint8_t {
        Applied,
        AlreadyCurrent,
        Duplicate,
        Stale,
        Missing,
        Invalid
    };

    struct SnapshotResult {
        SnapshotAction action = SnapshotAction::Invalid;
        bool exactMatch = false;
        bool shouldApply = false;
        bool allowOldDirectory = false;
        RingPresentationTransition transition;
    };

    static constexpr std::size_t kMaxRememberedTransitions = 128;

    void reset() noexcept;

    RegisterResult registerTransition(const RingPresentationTransition &transition);

    // Admit a snapshot by its physical identity.  The identity is matched to
    // the old round of a pending transition; no current/latest fallback is
    // permitted for a missing transition.
    SnapshotResult completeSnapshot(const RoundIdentity &round);

    std::size_t pendingCount() const noexcept;
    std::size_t rememberedCount() const noexcept { return transitions_.size(); }
    bool hasCurrentTarget() const noexcept { return hasCurrentTarget_; }
    RoundIdentity currentTarget() const noexcept { return currentTarget_; }

private:
    struct Entry {
        RingPresentationTransition transition;
        bool consumed = false;
    };

    void trim() noexcept;

    std::map<RoundIdentity, Entry> transitions_;
    std::deque<RoundIdentity> insertionOrder_;
    bool hasCurrentTarget_ = false;
    RoundIdentity currentTarget_;
};

} // namespace paimage
