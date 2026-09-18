#include "RingRoundPresentation.h"

namespace paimage {

bool RingPresentationTransition::fromCommit(
    const AutoSaveRoundCoordinator::CommitResult &commit,
    RingPresentationTransition &out) noexcept
{
    if ((!commit.committed && !commit.alreadyApplied) || commit.failed ||
        commit.measurementSession == 0 || commit.roundGeneration == 0)
        return false;

    out.commit = commit;
    out.nextRound = {commit.measurementSession, commit.roundGeneration};
    out.oldRound = {commit.measurementSession, commit.roundGeneration - 1};
    return out.oldRound.valid() && out.nextRound.valid() &&
           out.oldRound < out.nextRound;
}

void RingRoundPresentationState::reset() noexcept
{
    transitions_.clear();
    insertionOrder_.clear();
    hasCurrentTarget_ = false;
    currentTarget_ = RoundIdentity{};
}

RingRoundPresentationState::RegisterResult
RingRoundPresentationState::registerTransition(
    const RingPresentationTransition &transition)
{
    if (!transition.oldRound.valid() || !transition.nextRound.valid() ||
        !(transition.oldRound < transition.nextRound))
        return RegisterResult::Invalid;

    const auto it = transitions_.find(transition.oldRound);
    if (it != transitions_.end())
        return RegisterResult::Duplicate;

    transitions_.emplace(transition.oldRound, Entry{transition, false});
    insertionOrder_.push_back(transition.oldRound);
    trim();
    return RegisterResult::Registered;
}

RingRoundPresentationState::SnapshotResult
RingRoundPresentationState::completeSnapshot(const RoundIdentity &round)
{
    SnapshotResult result;
    result.transition.oldRound = round;
    if (!round.valid()) {
        result.action = SnapshotAction::Invalid;
        return result;
    }

    const auto it = transitions_.find(round);
    if (it != transitions_.end()) {
        result.exactMatch = true;
        result.transition = it->second.transition;
        if (it->second.consumed) {
            result.action = SnapshotAction::Duplicate;
            return result;
        }

        it->second.consumed = true;
        result.allowOldDirectory = true;
        if (!hasCurrentTarget_ || currentTarget_ < result.transition.nextRound) {
            currentTarget_ = result.transition.nextRound;
            hasCurrentTarget_ = true;
            result.action = SnapshotAction::Applied;
            result.shouldApply = true;
        } else if (currentTarget_ == result.transition.nextRound) {
            result.action = SnapshotAction::AlreadyCurrent;
        } else {
            result.action = SnapshotAction::Stale;
        }
        return result;
    }

    if (!hasCurrentTarget_) {
        result.action = SnapshotAction::Missing;
    } else if (round < currentTarget_) {
        result.action = SnapshotAction::Stale;
    } else if (round == currentTarget_) {
        result.action = SnapshotAction::AlreadyCurrent;
    } else {
        result.action = SnapshotAction::Missing;
    }
    return result;
}

std::size_t RingRoundPresentationState::pendingCount() const noexcept
{
    std::size_t count = 0;
    for (const auto &entry : transitions_)
        if (!entry.second.consumed) ++count;
    return count;
}

void RingRoundPresentationState::trim() noexcept
{
    while (transitions_.size() > kMaxRememberedTransitions &&
           !insertionOrder_.empty()) {
        const RoundIdentity oldest = insertionOrder_.front();
        insertionOrder_.pop_front();
        transitions_.erase(oldest);
    }
}

} // namespace paimage
