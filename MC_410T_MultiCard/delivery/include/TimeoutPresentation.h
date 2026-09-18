#pragma once
#include "RingRoundUiState.h"
#include <QDir>
#include <QString>
#include <atomic>
#include <functional>
#include <mutex>

namespace paimage {
// UI publishes an immutable frame/range writer. The source boundary captures
// that payload without waiting for the UI or performing PNG/disk work.
class TimeoutPresentation {
public:
    using Writer = std::function<bool(const QString&, const QString&)>;
    using Job = std::function<bool()>;
    void configure(bool enabled, const QString& directory, const QString& suffix) {
        std::lock_guard<std::mutex> lock(mutex_);
        enabled_ = enabled; directory_ = directory; suffix_ = suffix;
    }
    bool accepts(std::uint64_t session, std::uint64_t generation) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return !closed_ || session > closedSession_ ||
            (session == closedSession_ && generation > closedGeneration_);
    }
    void publish(std::uint64_t session, std::uint64_t generation, Writer writer) {
        std::lock_guard<std::mutex> lock(mutex_);
        session_ = session; generation_ = generation; writer_ = std::move(writer);
    }
    void updateWriter(Writer writer) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (writer_) writer_ = std::move(writer);
    }
    template<class Reset>
    Job close(std::uint64_t session, std::uint64_t nextGeneration,
              bool autoSave, const QString& oldDirectory, Reset reset) {
        Job job;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const QString target = autoSave
                ? (oldDirectory.isEmpty() ? QString() : QDir(oldDirectory).filePath("recon_png"))
                : directory_;
            if (enabled_ && writer_ && !target.isEmpty() &&
                session_ == session && nextGeneration > 0 && generation_ == nextGeneration - 1) {
                const auto writer = writer_;
                const auto suffix = suffix_;
                job = [writer, target, suffix] { return writer(target, suffix); };
            }
            writer_ = {};
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
        std::lock_guard<std::mutex> lock(mutex_); writer_ = {}; closed_ = false;
    }
private:
    mutable std::mutex mutex_;
    Writer writer_;
    QString directory_, suffix_;
    bool enabled_ = false, closed_ = false;
    std::uint64_t session_ = 0, generation_ = 0, closedSession_ = 0, closedGeneration_ = 0;
};
}
