#pragma once
#include "FrameConverter.h"
#include "OutputWorkers.h"
#include "TraceWriter.h"
#include "TimingWriter.h"
#include "DataProcessor.h"
#include "FileSaver.h"
#include <functional>
#include <memory>
#include <utility>
namespace paimage {
// Lifetimes: processors/savers and trace outlive this adapter. Their QThreads
// remain unstarted; only the recovered two output workers invoke delivery.
class HostOutput {
public:
    using SaveSessionResolver =
        std::function<std::uint64_t(std::uint64_t measurementSession,
                                    std::uint64_t roundGeneration)>;
    using MeasurementSessionBinder = std::function<std::uint64_t(std::uint64_t)>;
    // Frontend Preprocessing Stage stale-barrier feed.  Both callbacks reuse the
    // existing measurement session and PhysicalRoundNormalizer boundary facts
    // classified here; no independent round inference is introduced.
    // measurementSession == 0 means "measurement stopped / disarmed".
    using FrontendSessionSink = std::function<void(std::uint64_t measurementSession)>;
    using FrontendBarrierSink =
        std::function<void(std::uint64_t measurementSession,
                           std::uint64_t roundGeneration)>;

    // logicalTriggersPerRound is required by the production Backend.  Zero
    // keeps the historical adapter-only tests in pass-through mode.
    HostOutput(int bits,int blockSize,std::vector<DataProcessor*>,std::vector<FileSaver*>,TraceWriter*,
               TimingWriter* = nullptr,
                std::uint64_t logicalTriggersPerRound = 0,
                PhysicalRoundNormalizer::Observer = {},
                double physicalRoundTimeoutSec = 0.0,
                std::uint64_t startupFilterTriggerCount = 1,
                bool disableCountBoundary = false);
    ~HostOutput();
    void start(){workers_.start();}
    void stop(){workers_.stop();}
    void requestStop(){workers_.requestStop();}
    void beginSession(std::uint64_t s){
        std::lock_guard<std::mutex> lock(normalizationMutex_);
        if(measurementSessionBinder_)
            measurementSessionBinder_(s);
        if(normalizer_)normalizer_->beginSession(s);
        workers_.beginSession(s);
        // Frames queued or in flight for the previous measurement session must
        // not reach Display/Ring after the session advanced.
        if(frontendSessionSink_)frontendSessionSink_(s);
    }
    // Measurement stop/disarm: pending frontend work is invalidated so no old
    // frame is dispatched after the measurement stopped.
    void endSession(){
        std::lock_guard<std::mutex> lock(normalizationMutex_);
        if(frontendSessionSink_)frontendSessionSink_(0);
    }
    void setFrontendSessionSink(FrontendSessionSink sink){
        frontendSessionSink_=std::move(sink);
    }
    // Advanced from PhysicalRound TimeoutBoundary only.  CountBoundary must not
    // advance it: the just-completed final logical trigger still has to be
    // dispatched exactly once.
    void setFrontendBarrierSink(FrontendBarrierSink sink){
        frontendBarrierSink_=std::move(sink);
    }
    // Installed before the Backend starts.  The resolver is authoritative for
    // normalized LogicalScan save stamps; the old DataProcessor reader stays
    // only as a compatibility fallback for adapter-only tests.
    void setSaveSessionResolver(SaveSessionResolver resolver){
        saveSessionResolver_=std::move(resolver);
    }
    void setMeasurementSessionBinder(MeasurementSessionBinder binder){
        measurementSessionBinder_=std::move(binder);
    }
    void card(Frame f);
    void sync(std::uint16_t,const std::vector<Frame>&,bool startup);
    bool pollPhysicalRoundTimeout(std::int64_t now){
        std::lock_guard<std::mutex> lock(normalizationMutex_);
        return normalizer_ && normalizer_->timeoutBoundaryIfIdle(now);
    }
    void setConfiguredLogicalTriggersPerRound(std::uint64_t count){
        if(normalizer_)normalizer_->setConfiguredLogicalTriggersPerRound(count);
    }
    void setPhysicalRoundTimeout(double seconds){
        if(normalizer_)normalizer_->setTimeoutResetSec(seconds);
    }
    // Configuration-boundary policy controls for the shared physical-round
    // normalizer. Call before beginSession() for the next measurement.
    void setStartupFilterTriggerCount(std::uint64_t count){
        if(normalizer_)normalizer_->setStartupFilterTriggerCount(count);
    }
    void setDisableCountBoundary(bool disable){
        if(normalizer_)normalizer_->setDisableCountBoundary(disable);
    }
    PhysicalRoundNormalizer::Snapshot normalizerSnapshot() const;
    std::uint64_t startSaving(const QString&,int,const QString&);
    std::uint64_t stopSaving();
    void prepareConfigurationRestart(){configurationRestart_=true;}
    std::uint64_t resumeSaving();
    bool savingApplied(std::uint64_t g)const{return workers_.savingConfigurationApplied(g);}
    void requestClose();
    unsigned cardDepth(int card){return workers_.cardDepth(card);}
    OutputWorkers::Snapshot stats() const noexcept{return workers_.snapshot();}
private:
    void consumeCard(Frame);void consumeSync(const SyncFrame&);
    void observe(Frame,std::uint8_t stage,std::uint8_t reason,std::uint32_t value=0);
    std::vector<DataProcessor*> processors_;std::vector<FileSaver*> savers_;
    TraceWriter* trace_;TimingWriter* timing_;FrameConverter converter_;OutputWorkers workers_;
    std::unique_ptr<PhysicalRoundNormalizer> normalizer_;
    // Serialize boundary notification/binding/reset with source enqueue, not
    // merely the normalizer counters. Observers must never wait on the UI.
    std::mutex normalizationMutex_;
    std::uint64_t timeoutSession_ = 0, timeoutGeneration_ = 0;
    SaveSessionResolver saveSessionResolver_;
    MeasurementSessionBinder measurementSessionBinder_;
    FrontendSessionSink frontendSessionSink_;
    FrontendBarrierSink frontendBarrierSink_;
    std::atomic<bool> configurationRestart_{false};
};
}
