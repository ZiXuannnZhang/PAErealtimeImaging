#pragma once
#include "FrameConverter.h"
#include "OutputWorkers.h"
#include "TraceWriter.h"
#include "TimingWriter.h"
#include "DataProcessor.h"
#include "FileSaver.h"
#include <functional>
#include <QUuid>
#include <limits>
#include <memory>
#include <utility>
#include "RoundIdentity.h"
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
    void stop();
    void requestStop();
    void beginSession(std::uint64_t s){
        std::lock_guard<std::mutex> lock(normalizationMutex_);
        cancelTailState(true);
        expireFailedBoundary(0, nullptr, true);
        closedFailedCountRound_ = {};
        captureSession_ = s;
        captureId_ = std::make_shared<const std::string>(
            QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString());
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
        cancelTailState(true);
        expireFailedBoundary(0, nullptr, true);
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
    void setSaveBoundaryFaultProbe(std::function<bool()> probe) {
        saveBoundaryFaulted_ = std::move(probe);
    }
    enum class SaveTailOutcome {
        Admitted, SourceIncomplete, SourceMissing, UnverifiedRound,
        Expired, SessionEnded, BindingUnavailable, WriterFault, QueueFull, Rejected
    };
    using SaveTailSink = std::function<void(RoundIdentity, std::uint16_t, int, SaveTailOutcome)>;
    void setSaveTailSink(SaveTailSink sink) { saveTailSink_ = std::move(sink); }
    void setMeasurementSessionBinder(MeasurementSessionBinder binder){
        measurementSessionBinder_=std::move(binder);
    }
    void card(Frame f);
    // Called by the receiver with its core lock held, before packet assembly.
    void assemblyStarted(int card,std::uint64_t session,std::uint64_t ingress,Time first);
    void sync(std::uint16_t,const std::vector<Frame>&,bool startup);
    bool pollPhysicalRoundTimeout(Time idleWatermark, const std::vector<Time>* cardWatermarks = nullptr,
                                  Time now = 0);
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
    QString captureId() {
        std::lock_guard<std::mutex> lock(normalizationMutex_);
        return captureId_ ? QString::fromStdString(*captureId_) : QString{};
    }
    using SaveResultSink = std::function<void(const FileSaver::SaveResult&)>;
    using SaveHealthSink = std::function<void(int healthy, int total)>;
    std::uint64_t startSaving(const QString&, int, const QString&,
                              std::function<QString(std::uint64_t)> directoryFor = {},
                              SaveResultSink = {}, SaveHealthSink = {});
    std::uint64_t stopSaving();
    void prepareConfigurationRestart(){configurationRestart_=true;}
    std::uint64_t resumeSaving(SaveResultSink = {}, SaveHealthSink = {});
    bool savingApplied(std::uint64_t g)const{return workers_.savingConfigurationApplied(g);}
    void requestClose();
    unsigned cardDepth(int card){return workers_.cardDepth(card);}
    OutputWorkers::Snapshot stats() const noexcept{return workers_.snapshot();}
private:
    void beginTailSeals(const PhysicalRoundEvent&);
    void finishTailSeals(Time,const std::vector<Time>*);
    void sealCard(int,RoundIdentity);
    void cancelTailState(bool seal);
    void failSaveBoundary(const PhysicalRoundEvent&, std::uint64_t oldSaveGeneration);
    void expireFailedBoundary(Time, const std::vector<Time>*, bool sessionEnded = false);
    void finishFailedBoundary();
    bool waitingForFailedBoundary() const;
    void closeFailedCountCard(int);
    void reportSaveTail(RoundIdentity, std::uint16_t, int, SaveTailOutcome);
    bool waitingForFailedCount(std::size_t card, RoundIdentity round) const;
    void pushSaveFrame(const Frame&, const PhysicalRoundClassification&, bool timeoutEligible = false);
    struct SaveRequest {
        struct Card {
            std::uint64_t admitted = 0, rejected = 0;
            std::uint64_t callbackFailures = 0, confirmedAtStart = 0;
            std::atomic<bool> fault{false};
        };
        SaveRequest(std::size_t count, SaveResultSink result, SaveHealthSink health)
            : cards(count), settled(std::move(result)), healthChanged(std::move(health)),
              healthy(int(count)) {}
        std::vector<Card> cards;
        SaveResultSink settled;
        SaveHealthSink healthChanged;
        int healthy; // Sole saving worker.
    };
    std::uint64_t configureSaving(std::shared_ptr<SaveRequest>, std::function<void()> apply);
    SaveTailOutcome enqueueSaveFrame(const Frame&, bool eligible);
    void updateSaveHealth(int card, bool notify = true);
    void applySaveRequest(const std::shared_ptr<SaveRequest>&, const std::function<void(FileSaver&)>&);
    void settleSaveRequest(bool reconfigure);
    void consumeCard(Frame);void consumeSync(const SyncFrame&);
    void observe(Frame,std::uint8_t stage,std::uint8_t reason,std::uint32_t value=0);
    std::vector<DataProcessor*> processors_;std::vector<FileSaver*> savers_;
    TraceWriter* trace_;TimingWriter* timing_;FrameConverter converter_;OutputWorkers workers_;
    std::unique_ptr<PhysicalRoundNormalizer> normalizer_;
    // Serialize boundary notification/binding/reset with source enqueue, not
    // merely the normalizer counters. Observers must never wait on the UI.
    std::mutex normalizationMutex_;
    std::uint64_t timeoutSession_ = 0, timeoutGeneration_ = 0;
    struct ActiveAssembly {
        std::uint64_t session=0, ingress=0;
        Time first=0;
        RoundIdentity round;
    };
    std::vector<ActiveAssembly> activeAssemblies_;
    struct Tail {ActiveAssembly active;RoundIdentity round;std::uint64_t generation=0;Time deadline=0;bool waiting=false;};
    std::vector<Tail> tails_;
    std::vector<RoundIdentity> cardSaveRounds_;
    PhysicalRoundNormalizer::Snapshot preBoundarySnapshot_;
    struct FailedBoundary {
        RoundIdentity round;
        std::uint64_t saveGeneration = 0;
        std::uint16_t trigger = 0;
        Time observed = 0, deadline = 0;
        bool count = false;
        struct CountCard { ActiveAssembly inFlight; bool waiting = false; };
        std::vector<CountCard> cards;
    };
    std::optional<FailedBoundary> failedBoundary_;
    RoundIdentity closedFailedCountRound_;
    std::deque<std::function<void()>> deferredSaveControls_;
    Time boundaryObservedNs_ = 0;
    std::uint64_t captureSession_ = 0;
    std::shared_ptr<const std::string> captureId_;
    std::shared_ptr<SaveRequest> sourceSaveRequest_; // Under normalizationMutex_.
    std::shared_ptr<SaveRequest> writerSaveRequest_; // Sole saving worker.
    SaveSessionResolver saveSessionResolver_;
    std::function<bool()> saveBoundaryFaulted_;
    SaveTailSink saveTailSink_;
    MeasurementSessionBinder measurementSessionBinder_;
    FrontendSessionSink frontendSessionSink_;
    FrontendBarrierSink frontendBarrierSink_;
    std::atomic<bool> configurationRestart_{false};
};
}
