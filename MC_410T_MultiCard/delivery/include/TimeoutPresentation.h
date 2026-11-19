#pragma once
#include "RingRoundUiState.h"
#include "RingSnapshotCopy.h"
#include <QDir>
#include <QImage>
#include <array>
#include <cstddef>
#include <QString>
#include <atomic>
#include <functional>
#include <mutex>

namespace paimage {
// The source boundary takes the displayed frame and its render settings
// without waiting for the UI or performing PNG/disk work.
class TimeoutPresentation {
public:
    struct Capture {
        static std::size_t checkedFrameBytes(int nx, std::size_t floats) {
            if (nx <= 0) return 0;
            const auto pixels = ring_snapshot::checkedSquareFrameSize(std::size_t(nx));
            ring_snapshot::Layout layout;
            if (!ring_snapshot::checkedLayout(0, 0, pixels, layout) ||
                floats != layout.frameBytes / sizeof(float)) return 0;
            return layout.frameBytes;
        }
        std::size_t frameBytes = 0;
        QString tag;
        std::function<std::array<QImage, 2>()> render;
        int nx = 0;
        explicit operator bool() const { return bool(render); }
    };
    struct Target {
        QString directory;
        QString suffix;
        std::uint64_t requestId = 0;
        QString listenId;
        explicit operator bool() const { return !directory.isEmpty(); }
    };
    struct Job {
        Target target;
        Capture capture;
        explicit operator bool() const { return bool(capture); }
    };
    void configure(bool enabled, const QString& directory, const QString& suffix) {
        std::lock_guard<std::mutex> lock(mutex_);
        enabled_ = enabled; directory_ = directory; suffix_ = suffix;
    }
    bool accepts(std::uint64_t session, std::uint64_t generation) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return !closed_ || session > closedSession_ ||
            (session == closedSession_ && generation > closedGeneration_);
    }
    void publish(std::uint64_t session, std::uint64_t generation, Capture capture) {
        std::lock_guard<std::mutex> lock(mutex_);
        session_ = session; generation_ = generation; capture_ = std::move(capture);
    }
    void updateCapture(Capture capture) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (capture_) capture_ = std::move(capture);
    }
    template<class Reset>
    Job close(std::uint64_t session, std::uint64_t nextGeneration,
              bool autoSave, const QString& oldDirectory, Reset reset) {
        Target target;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (enabled_)
                target = {autoSave ? (oldDirectory.isEmpty() ? QString() : QDir(oldDirectory).filePath("recon_png"))
                                   : directory_, suffix_, 0};
        }
        return close(session, nextGeneration, std::move(target), std::move(reset));
    }
    template<class Reset>
    Job close(std::uint64_t session, std::uint64_t nextGeneration, Target target, Reset reset) {
        Job job{std::move(target), {}};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (job.target && capture_ && session_ == session && nextGeneration > 0 &&
                generation_ == nextGeneration - 1)
                job.capture = std::move(capture_);
            capture_ = {};
            closed_ = true; closedSession_ = session; closedGeneration_ = nextGeneration - 1;
        }
        // Capture is complete. The owned pixels/ranges and old directory stay
        // valid even if reset immediately destroys every live presentation.
        reset();
        return job;
    }
    static void applyResetResult(bool sent, std::uint64_t cutoff,
                                 RingRoundUiState& ui, std::atomic<bool>& blocked) {
        if (sent) ui.armStaleCutoff(cutoff);
        else { blocked.store(true, std::memory_order_release); ui.disarmStaleCutoff(); }
    }
    void reset() {
        std::lock_guard<std::mutex> lock(mutex_); capture_ = {}; closed_ = false;
    }
private:
    mutable std::mutex mutex_;
    Capture capture_;
    QString directory_, suffix_;
    bool enabled_ = false, closed_ = false;
    std::uint64_t session_ = 0, generation_ = 0, closedSession_ = 0, closedGeneration_ = 0;
};
}
