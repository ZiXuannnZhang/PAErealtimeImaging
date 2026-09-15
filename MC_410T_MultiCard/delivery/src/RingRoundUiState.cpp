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
    staleCutoffSeq_ = 0;
    hasStaleCutoff_ = false;
    staleDropped_ = 0;
    frameEnds_ = 0;
}

void RingRoundUiState::recordSubmit()
{
    std::lock_guard<std::mutex> lock(mutex_);
    ++submissionsTotal_;
}

void RingRoundUiState::onBlock()
{
    std::lock_guard<std::mutex> lock(mutex_);
    ++blockCount_;
}

bool RingRoundUiState::admitSnapshot(std::int64_t svcGlobalSeq)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (svcGlobalSeq < 0)
        return false;
    if (hasStaleCutoff_ && static_cast<std::uint64_t>(svcGlobalSeq) <= staleCutoffSeq_)
        return false;
    return true;
}

void RingRoundUiState::noteStaleSnapshot()
{
    std::lock_guard<std::mutex> lock(mutex_);
    ++staleDropped_;
}

bool RingRoundUiState::noteSnapshot(int blocksPerFrame)
{
    std::lock_guard<std::mutex> lock(mutex_);
    ++snapshotsThisRound_;
    if (blocksPerFrame > 0 &&
        snapshotsThisRound_ % static_cast<std::uint64_t>(blocksPerFrame) == 0) {
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
    // round, driven by noteSnapshot(blocksPerFrame). Nothing to do here; the
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

void RingRoundUiState::armStaleCutoff()
{
    std::lock_guard<std::mutex> lock(mutex_);
    // Ring block seq restarts at 0 after the assembler/service reset, while the
    // svc shm frame_seq keeps counting. The submission total tracks the same
    // FIFO block stream as frame_seq, so it is the exact stale cut: any
    // snapshot produced from pre-reset blocks has svc seq <= the cut.
    staleCutoffSeq_ = submissionsTotal_;
    hasStaleCutoff_ = true;
}

RingRoundUiState::Snapshot RingRoundUiState::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    Snapshot s;
    s.frameCount = frameCount_;
    s.blockCount = blockCount_;
    s.snapshotsThisRound = snapshotsThisRound_;
    s.submissionsTotal = submissionsTotal_;
    s.epoch = epoch_;
    s.staleSnapshotsDropped = staleDropped_;
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

} // namespace paimage
