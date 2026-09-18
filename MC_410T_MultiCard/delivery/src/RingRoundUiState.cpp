#include "RingRoundUiState.h"

#include <algorithm>

namespace paimage {

void RingRoundUiState::reset()
{
    std::lock_guard<std::mutex> lock(mutex_);
    frameCount_ = 0;
    blockCount_ = 0;
    snapshotsThisRound_ = 0;
    submissionsTotal_ = 0;
    epoch_ = 0;
    lastSubmitIndex_ = 0;
    staleCutoffSubmitIndex_ = 0;
    hasStaleCutoff_ = false;
    staleDropped_ = 0;
    duplicateSnapshots_ = 0;
    frameEnds_ = 0;
    admittedSubmitOrder_.clear();
    admittedSubmitIndices_.clear();
}

void RingRoundUiState::recordSubmitIndex(std::uint64_t submitIndex)
{
    if (submitIndex == 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    ++submissionsTotal_;
    if (submitIndex > lastSubmitIndex_)
        lastSubmitIndex_ = submitIndex;
}

void RingRoundUiState::onBlock()
{
    std::lock_guard<std::mutex> lock(mutex_);
    ++blockCount_;
}

RingRoundUiState::SnapshotAdmission
RingRoundUiState::admitSnapshotDetailed(std::uint64_t submitIndex)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (submitIndex == 0)
        return SnapshotAdmission::Missing;
    if (hasStaleCutoff_ && submitIndex <= staleCutoffSubmitIndex_)
        return SnapshotAdmission::Stale;
    if (admittedSubmitIndices_.find(submitIndex) != admittedSubmitIndices_.end())
        return SnapshotAdmission::Duplicate;

    admittedSubmitIndices_.insert(submitIndex);
    admittedSubmitOrder_.push_back(submitIndex);
    while (admittedSubmitOrder_.size() > kMaxAdmittedSubmitIndices) {
        admittedSubmitIndices_.erase(admittedSubmitOrder_.front());
        admittedSubmitOrder_.pop_front();
    }
    return SnapshotAdmission::Accepted;
}

bool RingRoundUiState::admitSnapshot(std::uint64_t submitIndex)
{
    return admitSnapshotDetailed(submitIndex) == SnapshotAdmission::Accepted;
}

void RingRoundUiState::noteStaleSnapshot()
{
    std::lock_guard<std::mutex> lock(mutex_);
    ++staleDropped_;
}

void RingRoundUiState::noteDuplicateSnapshot()
{
    std::lock_guard<std::mutex> lock(mutex_);
    ++duplicateSnapshots_;
}

bool RingRoundUiState::noteSnapshot(bool roundComplete)
{
    std::lock_guard<std::mutex> lock(mutex_);
    ++snapshotsThisRound_;
    if (roundComplete) {
        ++frameEnds_;
        return true;
    }
    return false;
}

void RingRoundUiState::onFrame()
{
    std::lock_guard<std::mutex> lock(mutex_);
    ++frameCount_;
}

void RingRoundUiState::resetFrameCount()
{
    std::lock_guard<std::mutex> lock(mutex_);
    frameCount_ = 0;
}

void RingRoundUiState::onCountBoundary()
{
    // CountBoundary keeps the seq space monotonic (RingBlockAssembler keeps
    // blockSeq running) and the frame counter resets on the final frame of the
    // round, driven by noteSnapshot(roundComplete). Nothing to do here; the
    // coordinator records the event for diagnostics at the call site.
}

void RingRoundUiState::onTimeoutBoundary()
{
    std::lock_guard<std::mutex> lock(mutex_);
    ++epoch_;
    frameCount_ = 0;
    blockCount_ = 0;
    snapshotsThisRound_ = 0;
}

void RingRoundUiState::armStaleCutoff(std::uint64_t cutoffSubmitIndex)
{
    std::lock_guard<std::mutex> lock(mutex_);
    // Ring block seq and svc frame_seq are not the identity domain.  The
    // caller supplies the producer submit_index that was reserved by the
    // actual ring_block_ready command path.
    staleCutoffSubmitIndex_ = cutoffSubmitIndex;
    hasStaleCutoff_ = true;
}

void RingRoundUiState::disarmStaleCutoff()
{
    std::lock_guard<std::mutex> lock(mutex_);
    staleCutoffSubmitIndex_ = 0;
    hasStaleCutoff_ = false;
}

RingRoundUiState::Snapshot RingRoundUiState::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    Snapshot s;
    s.frameCount = frameCount_;
    s.blockCount = blockCount_;
    s.snapshotsThisRound = snapshotsThisRound_;
    s.submissionsTotal = submissionsTotal_;
    s.lastSubmitIndex = lastSubmitIndex_;
    s.staleCutoffSubmitIndex = staleCutoffSubmitIndex_;
    s.hasStaleCutoff = hasStaleCutoff_;
    s.epoch = epoch_;
    s.staleSnapshotsDropped = staleDropped_;
    s.duplicateSnapshots = duplicateSnapshots_;
    s.frameEndEvents = frameEnds_;
    return s;
}

std::uint64_t RingRoundUiState::frameCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return frameCount_;
}

std::uint64_t RingRoundUiState::blockCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return blockCount_;
}

std::uint64_t RingRoundUiState::staleSnapshotsDropped() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return staleDropped_;
}

std::uint64_t RingRoundUiState::duplicateSnapshots() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return duplicateSnapshots_;
}

} // namespace paimage
