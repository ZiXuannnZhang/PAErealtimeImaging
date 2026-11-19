#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "PaimageAcquisition/HostOutput.h"
#include "PaimageAcquisition/AutoSaveRoundCoordinator.h"
#include "PaimageAcquisition/SocketReceiver.h"
#include <QDateTime>
#include <algorithm>
#include <limits>
#include <stdexcept>
namespace paimage {
namespace {
constexpr Time kSaveTailGraceNs = 100000000;

Time normalizationTime(const Frame& frame) {
    if (!frame) return 0;
    // SourceCore records first/closed using SocketReceiver::now(), i.e. the
    // same steady-clock domain used by the physical-round timeout. Do not
    // substitute wall-clock time when a source timestamp is absent.
    return frame->first > 0 ? frame->first : frame->closed;
}
}
HostOutput::HostOutput(int bits,int block,std::vector<DataProcessor*> processors,
    std::vector<FileSaver*> savers,TraceWriter* trace,TimingWriter* timing,
    std::uint64_t logicalTriggersPerRound,PhysicalRoundNormalizer::Observer normalizerObserver,
    double physicalRoundTimeoutSec,std::uint64_t startupFilterTriggerCount,
    bool disableCountBoundary)
    :processors_(std::move(processors)),savers_(std::move(savers)),trace_(trace),timing_(timing),
     converter_(bits,QDateTime::currentMSecsSinceEpoch(),SocketReceiver::now()),
     workers_(int(processors_.size()),block,[this](Frame f){consumeCard(f);},
        [this](const SyncFrame& f){consumeSync(f);},
        [this](auto r,Frame f){observe(f,5,std::uint8_t(r));}) {
    if(processors_.size()!=savers_.size())throw std::invalid_argument("host card mapping");
    activeAssemblies_.resize(processors_.size());
    tails_.resize(processors_.size());cardSaveRounds_.resize(processors_.size());
    if(logicalTriggersPerRound){
        normalizer_=std::make_unique<PhysicalRoundNormalizer>(logicalTriggersPerRound,
            [this, observer=std::move(normalizerObserver)](const PhysicalRoundEvent& event) {
                if(event.kind==PhysicalRoundEvent::Kind::TimeoutBoundary){
                    timeoutSession_=event.measurementSession;
                    timeoutGeneration_=event.roundGeneration;
                    // Physical-round TimeoutBoundary advances the frontend
                    // stale barrier from the normalizer's own boundary facts.
                    // CountBoundary deliberately does not: its final logical
                    // trigger must still be dispatched exactly once.
                    if(frontendBarrierSink_)
                        frontendBarrierSink_(event.measurementSession,event.roundGeneration);
                    beginTailSeals(event);
                }
                const bool canFail = sourceSaveRequest_ && !failedBoundary_ && saveBoundaryFaulted_ &&
                    (event.kind == PhysicalRoundEvent::Kind::CountBoundary ||
                     event.kind == PhysicalRoundEvent::Kind::TimeoutBoundary) &&
                    !saveBoundaryFaulted_();
                const auto oldGeneration = canFail && event.kind == PhysicalRoundEvent::Kind::CountBoundary &&
                    event.roundGeneration && saveSessionResolver_
                    ? saveSessionResolver_(event.measurementSession, event.roundGeneration - 1) : 0;
                if (observer) observer(event);
                if (canFail && saveBoundaryFaulted_()) failSaveBoundary(event, oldGeneration);
            });
        normalizer_->setTimeoutResetSec(physicalRoundTimeoutSec);
        normalizer_->setStartupFilterTriggerCount(startupFilterTriggerCount);
        normalizer_->setDisableCountBoundary(disableCountBoundary);
    }
    for(std::size_t i=0;i<processors_.size();++i){
        auto saver=savers_[i];processors_[i]->setDirectSaveSink([saver](const TriggerGroupPtr& f){return saver->consumeTriggerGroup(f);});
    }
    workers_.setSavingHooks([this] {
        for (std::size_t card = 0; card < savers_.size(); ++card) {
            try { savers_[card]->serviceCloseRequest(); }
            catch (...) { savers_[card]->failSaveCallback(); }
            updateSaveHealth(int(card));
        }
    }, [this] { settleSaveRequest(configurationRestart_.load()); });
}
HostOutput::~HostOutput(){stop();}

void HostOutput::stop()
{
    requestStop();
    workers_.stop();
}

void HostOutput::requestStop()
{
    std::lock_guard<std::mutex> lock(normalizationMutex_);
    cancelTailState(true);
    expireFailedBoundary((std::numeric_limits<Time>::max)(), nullptr);
    sourceSaveRequest_.reset();
    workers_.requestStop();
}
void HostOutput::assemblyStarted(int card,std::uint64_t session,std::uint64_t ingress,Time first){
    std::lock_guard<std::mutex> lock(normalizationMutex_);
    if(card<0 || static_cast<std::size_t>(card)>=activeAssemblies_.size())return;
    auto& a=activeAssemblies_[card];a={session,ingress,first,{}};
    if(normalizer_){const auto s=normalizer_->snapshot();
        if(s.measurementSession==session && s.currentPhysicalDistinctCount)
            a.round={session,s.roundGeneration};}
}
void HostOutput::sealCard(int card,RoundIdentity round){
    workers_.enqueueSaveBarrier([this, card, round] {
        try { savers_.at(card)->sealRound(round); }
        catch (...) { savers_.at(card)->failSaveCallback(); }
        updateSaveHealth(card);
    });
}
void HostOutput::beginTailSeals(const PhysicalRoundEvent& event){
    if(!event.roundGeneration)return;
    const RoundIdentity old{event.measurementSession,event.roundGeneration-1};
    const Time cutoff=preBoundarySnapshot_.lastDistinctTriggerTimeNs+preBoundarySnapshot_.timeoutResetNs;
    for(std::size_t c=0;c<tails_.size();++c){
        // A newer Timeout seal would advance FileSaver's watermark past
        // the failed Count round before its final column is delivered.
        if (waitingForFailedCount(c, old)) continue;
        if(tails_[c].waiting)sealCard(int(c),tails_[c].round);
        auto& t=tails_[c];t={};const auto& a=activeAssemblies_[c];
        t.round=old;t.deadline=cutoff+kSaveTailGraceNs;
        t.generation=saveSessionResolver_ ? saveSessionResolver_(old.measurementSession,old.roundGeneration) : 0;
        const bool healthy = !sourceSaveRequest_ || !sourceSaveRequest_->cards[c].fault.load();
        t.waiting=healthy && a.round==old && cardSaveRounds_[c]==old && a.ingress && a.first>0 && a.first<=cutoff;
        if(t.waiting)t.active=a;
        if(!t.waiting)sealCard(int(c),old);
    }
}
void HostOutput::finishTailSeals(Time now,const std::vector<Time>* cardWatermarks){
    for (std::size_t c = 0; c < tails_.size(); ++c) {
        auto& tail = tails_[c];
        if (!tail.waiting) continue;
        const auto watermark = cardWatermarks ? (c < cardWatermarks->size() ? (*cardWatermarks)[c] : 0) : now;
        const bool failed = failedBoundary_ && !failedBoundary_->count && failedBoundary_->round == tail.round;
        const bool writerFault = failed && sourceSaveRequest_->cards[c].fault.load();
        if (watermark <= tail.deadline && !(failed && now > tail.deadline) && !writerFault) continue;
        tail.waiting = false;
        sealCard(int(c), tail.round);
        if (failed)
            reportSaveTail(tail.round, failedBoundary_->trigger, int(c),
                           writerFault ? SaveTailOutcome::WriterFault : SaveTailOutcome::SourceMissing);
    }
}
void HostOutput::cancelTailState(bool seal){
    for(std::size_t c=0;c<tails_.size();++c){
        if(seal && tails_[c].waiting)sealCard(int(c),tails_[c].round);
        tails_[c]={};activeAssemblies_[c]={};cardSaveRounds_[c]={};
    }
}
void HostOutput::failSaveBoundary(const PhysicalRoundEvent& event, std::uint64_t oldSaveGeneration)
{
    if (!event.roundGeneration) return;
    FailedBoundary failure;
    failure.round = {event.measurementSession, event.roundGeneration - 1};
    failure.count = event.kind == PhysicalRoundEvent::Kind::CountBoundary;
    failure.trigger = event.triggerSeq;
    if (failure.count) {
        failure.saveGeneration = oldSaveGeneration;
        failure.observed = boundaryObservedNs_;
        failure.deadline = failure.observed + kSaveTailGraceNs;
        failure.cards.resize(savers_.size());
        closedFailedCountRound_ = failure.round;
        for (std::size_t card = 0; card < savers_.size(); ++card) {
            const bool fault = sourceSaveRequest_->cards[card].fault.load();
            failure.cards[card] = {activeAssemblies_[card], !fault};
            if (fault) {
                sealCard(int(card), failure.round);
                reportSaveTail(failure.round, failure.trigger, int(card), SaveTailOutcome::WriterFault);
            }
        }
    }
    failedBoundary_ = std::move(failure);
}

bool HostOutput::waitingForFailedBoundary() const
{
    if (!failedBoundary_) return false;
    if (failedBoundary_->count)
        return std::any_of(failedBoundary_->cards.begin(), failedBoundary_->cards.end(),
                           [](const auto& card) { return card.waiting; });
    return std::any_of(tails_.begin(), tails_.end(), [this](const Tail& tail) {
        return tail.round == failedBoundary_->round && tail.waiting;
    });
}

void HostOutput::finishFailedBoundary()
{
    if (deferredSaveControls_.empty() || waitingForFailedBoundary()) return;
    auto controls = std::move(deferredSaveControls_);
    deferredSaveControls_.clear();
    for (auto& apply : controls) apply();
}

void HostOutput::reportSaveTail(RoundIdentity round, std::uint16_t trigger, int card,
                                 SaveTailOutcome outcome)
{
    if (saveTailSink_) saveTailSink_(round, trigger, card, outcome);
}

void HostOutput::closeFailedCountCard(int card)
{
    failedBoundary_->cards[card].waiting = false;
    sealCard(card, failedBoundary_->round);
}

void HostOutput::expireFailedBoundary(Time now, const std::vector<Time>* cardWatermarks)
{
    if (failedBoundary_ && failedBoundary_->count) {
        const auto& failure = *failedBoundary_;
        for (std::size_t card = 0; card < failure.cards.size(); ++card) {
            if (!failure.cards[card].waiting) continue;
            const auto watermark = cardWatermarks && card < cardWatermarks->size()
                ? (*cardWatermarks)[card] : 0;
            const bool fault = sourceSaveRequest_->cards[card].fault.load();
            if (!fault && now <= failure.deadline && watermark <= failure.deadline) continue;
            reportSaveTail(failure.round, failure.trigger, int(card),
                           fault ? SaveTailOutcome::WriterFault : SaveTailOutcome::Expired);
            closeFailedCountCard(int(card));
        }
    }
    finishFailedBoundary();
}

bool HostOutput::waitingForFailedCount(std::size_t card, RoundIdentity round) const
{
    return failedBoundary_ && failedBoundary_->count && failedBoundary_->cards[card].waiting &&
           round.measurementSession == failedBoundary_->round.measurementSession &&
           round.roundGeneration >= failedBoundary_->round.roundGeneration;
}

void HostOutput::pushSaveFrame(const Frame& frame, const PhysicalRoundClassification& classification,
                               bool timeoutEligible)
{
    const RoundIdentity round{classification.measurementSession, classification.roundGeneration};
    if (!failedBoundary_) {
        if (closedFailedCountRound_.valid() && round.measurementSession == closedFailedCountRound_.measurementSession &&
            round <= closedFailedCountRound_) return;
        enqueueSaveFrame(frame, true);
        return;
    }
    auto& failure = *failedBoundary_;
    if (!failure.count) {
        const auto outcome = enqueueSaveFrame(frame, timeoutEligible);
        if (timeoutEligible)
            reportSaveTail(failure.round, frame->trigger, frame->card, outcome);
        return;
    }

    auto& card = failure.cards[frame->card];
    const auto& active = card.inFlight;
    const bool inFlight = active.session == frame->measurementSession && active.ingress &&
        active.ingress == frame->firstIngressId && active.first == frame->first &&
        active.first > 0 && active.first <= failure.observed;
    if (inFlight) card.inFlight = {};
    const bool final = frame->trigger == failure.trigger && classification.isFinalLogicalTrigger;
    const bool belongs = round == failure.round && classification.logicalTriggerIndex >= 0 &&
        (final || (inFlight && !classification.newDistinct));

    if (!card.waiting || !belongs) {
        enqueueSaveFrame(frame, false);
        if (card.waiting && inFlight)
            reportSaveTail(failure.round, frame->trigger, frame->card, SaveTailOutcome::UnverifiedRound);
        return;
    }

    SaveTailOutcome outcome;
    if (frame->closed <= 0 || frame->closed > failure.deadline) {
        outcome = SaveTailOutcome::Expired;
        enqueueSaveFrame(frame, false);
    } else if (failure.saveGeneration == AutoSaveRoundCoordinator::kFailedGeneration) {
        outcome = SaveTailOutcome::BindingUnavailable;
        enqueueSaveFrame(frame, false);
    } else {
        // Already classified in the original logical round. Use the regular
        // writer so a split at the configured file length can still advance.
        converter_.tagSaveSession(frame, failure.saveGeneration, captureId_);
        outcome = enqueueSaveFrame(frame, true);
    }
    reportSaveTail(failure.round, frame->trigger, frame->card, outcome);
    if (final || outcome == SaveTailOutcome::WriterFault) {
        closeFailedCountCard(frame->card);
        finishFailedBoundary();
    }
}

bool HostOutput::pollPhysicalRoundTimeout(Time idleWatermark, const std::vector<Time>* cardWatermarks, Time now)
{
    std::lock_guard<std::mutex> lock(normalizationMutex_);
    if (!normalizer_) return false;
    preBoundarySnapshot_ = normalizer_->snapshot();
    const bool result = idleWatermark > 0 && normalizer_->timeoutBoundaryIfIdle(idleWatermark);
    if (!now) now = idleWatermark;
    finishTailSeals(now, cardWatermarks);
    expireFailedBoundary(now, cardWatermarks);
    return result;
}

void HostOutput::card(Frame f){
    if(!f)return;
    std::lock_guard<std::mutex> lock(normalizationMutex_);
    PhysicalRoundClassification classification;
    if(normalizer_){
        preBoundarySnapshot_=normalizer_->snapshot();
        boundaryObservedNs_ = f->closed > 0 ? f->closed : normalizationTime(f);
        classification=normalizer_->classify(f->measurementSession,f->trigger,
                                              normalizationTime(f));
        converter_.tagNormalization(f,classification);
        const auto& t=tails_.at(f->card);
        const bool token=t.round.valid() && t.active.session==f->measurementSession && t.active.ingress &&
                         t.active.ingress==f->firstIngressId && t.active.first==f->first;
        const bool oldControl=classification.roundGeneration==t.round.roundGeneration &&
                              classification.decision==PhysicalTriggerDecision::OperationalStartupControl;
        if(classification.decision==PhysicalTriggerDecision::OperationalStartupControl && (!token || oldControl)){
            // SourceCore::Decision::Timeout is a single-card packet assembly
            // timeout. It must never be promoted to a physical-round idle
            // boundary; only the normalizer's monotonic idle check owns that
            // boundary.
            auto& active=activeAssemblies_[f->card];
            if(active.session==f->measurementSession && active.ingress==f->firstIngressId && active.first==f->first)active={};
            return;
        }
    }
    const auto saveGeneration = saveSessionResolver_
        ? saveSessionResolver_(classification.measurementSession,
                               classification.roundGeneration)
        : processors_.at(f->card)->captureSaveSessionGen();
    // The resolver returns the coordinator's fail-closed sentinel for an
    // enabled lookup miss.  FrameConverter stores it on this source frame so
    // FileSaver cannot silently reuse its previous directory.
    converter_.tagSaveSession(f, saveGeneration,
        sourceSaveRequest_ && f->measurementSession == captureSession_ ? captureId_ : nullptr);
    const auto begin=SocketReceiver::now();const auto session=f->measurementSession,link=f->firstIngressId;const int card=f->card;
    auto& tail=tails_.at(card);
    const bool token=tail.round.valid() && tail.active.session==session && tail.active.ingress && tail.active.ingress==link && tail.active.first==f->first;
    // A cache hit bypasses the normalizer's idle test.  Do not let a frame
    // first seen after the effective cutoff reopen the old official file in
    // the interval before the next idle poll.  This is save-only: frontend
    // and shared normalizer decisions remain untouched.
    const bool staleCached = preBoundarySnapshot_.measurementSession==session &&
        preBoundarySnapshot_.hasLastDistinctTrigger && preBoundarySnapshot_.timeoutResetNs>0 &&
        f->first>preBoundarySnapshot_.lastDistinctTriggerTimeNs &&
        f->first-preBoundarySnapshot_.lastDistinctTriggerTimeNs>preBoundarySnapshot_.timeoutResetNs &&
        !classification.newDistinct && classification.roundGeneration==preBoundarySnapshot_.roundGeneration;
    // Count recovery owns saving for this card while the old request is
    // draining. Keep the normalized index and avoid Timeout's late/tail tags.
    const bool recoveringCount = waitingForFailedCount(card, {session, classification.roundGeneration});
    if(token && !recoveringCount){
        const bool accept=tail.waiting && f->closed>0 && f->closed<=tail.deadline;
        converter_.tagSaveOverride(f,session,tail.round.roundGeneration,tail.generation,!accept);
        if (tail.waiting) {
            tail.waiting = false;
            const auto closing = tail.round;
            pushSaveFrame(f, classification, accept);
            if (!accept && failedBoundary_ && !failedBoundary_->count && failedBoundary_->round == closing)
                reportSaveTail(closing, f->trigger, card, SaveTailOutcome::Expired);
            sealCard(card, closing);
            auto& active = activeAssemblies_[card];
            if (active.session == session && active.ingress == link && active.first == f->first) active = {};
            finishFailedBoundary();
            return;
        }
    }else if(!recoveringCount && (staleCached || (classification.measurementSession==timeoutSession_ && classification.roundGeneration<timeoutGeneration_))){
        converter_.tagSaveOverride(f,session,classification.roundGeneration,saveGeneration,true);
    }
    if(tail.waiting && classification.roundGeneration>tail.round.roundGeneration &&
       !recoveringCount &&
       !(failedBoundary_ && !failedBoundary_->count && failedBoundary_->round == tail.round)){
        tail.waiting=false;sealCard(card,tail.round);
    }
    const RoundIdentity round{session, classification.roundGeneration};
    if (classification.decision == PhysicalTriggerDecision::LogicalScan && cardSaveRounds_[card] < round)
        cardSaveRounds_[card] = round;
    // Publication contract: classification, save generation and any save
    // override are all tagged before this enqueue. SourceCore delivers each
    // card before publishing the corresponding sync group (also on startup).
    pushSaveFrame(f, classification);
    {auto& active=activeAssemblies_[card];if(active.session==session&&active.ingress==link&&active.first==f->first)active={};}
    if(timing_){const auto end=SocketReceiver::now();TimingRecord r;r.startNs=begin;r.endNs=end;r.session=session;r.correlation=link;r.threadId=GetCurrentThreadId();r.card=card;r.kind=std::uint16_t(TimingKind::CardEnqueue);timing_->observe(r,end-begin>=500000);}
}
void HostOutput::sync(std::uint16_t trigger,const std::vector<Frame>& frames,bool startup){
    std::lock_guard<std::mutex> lock(normalizationMutex_);
    // Three-state group gate (count-boundary frontend-only, task 20260929):
    //   Drop         -- startup control trigger or expired-round frame; the
    //                   whole group is discarded exactly as before, in every
    //                   mode, with the original precedence.
    //   FrontendOnly -- disableCountBoundary && LogicalScan at or beyond the
    //                   configured per-round count: the group still reaches
    //                   the frontend signal path (time/frequency windows keep
    //                   refreshing) while the imaging publisher stays frozen
    //                   (consumeSync -> deliverAssembled(imagingSubmit=false)).
    //   Pass         -- everything else keeps the verbatim previous behavior.
    // Drop outranks FrontendOnly; one FrontendOnly frame marks the whole group.
    bool filter=false;
    bool frontendOnly=false;
    if(normalizer_){
        for(const auto& f:frames){
            if(!f)continue;
            preBoundarySnapshot_=normalizer_->snapshot();
            const auto classification=normalizer_->classify(f->measurementSession,f->trigger,
                                                              normalizationTime(f));
            converter_.tagNormalization(f,classification);
            if(classification.decision==PhysicalTriggerDecision::OperationalStartupControl)filter=true;
            if(classification.measurementSession==timeoutSession_ &&
               classification.roundGeneration<timeoutGeneration_)filter=true;
            if(normalizer_->disableCountBoundary() &&
               classification.decision==PhysicalTriggerDecision::LogicalScan &&
               classification.logicalTriggerIndex >= 0 &&
               static_cast<std::uint64_t>(classification.logicalTriggerIndex) >=
                   normalizer_->configuredLogicalTriggersPerRound())frontendOnly=true;
        }
        if(filter)return;
    }
    const auto begin=SocketReceiver::now();const auto session=frames.empty()?0:frames.front()->measurementSession;workers_.pushSync(trigger,frames,startup,frontendOnly);
    if(timing_){const auto end=SocketReceiver::now();TimingRecord r;r.startNs=begin;r.endNs=end;r.session=session;r.threadId=GetCurrentThreadId();r.card=-1;r.kind=std::uint16_t(TimingKind::SyncEnqueue);r.value0=std::uint32_t(frames.size());r.flags=startup?1:0;timing_->observe(r,end-begin>=500000);}
}
PhysicalRoundNormalizer::Snapshot HostOutput::normalizerSnapshot() const{
    if(normalizer_)return normalizer_->snapshot();
    PhysicalRoundNormalizer::Snapshot result;result.firstVisibleFilterMode=false;return result;
}
void HostOutput::observe(Frame f,std::uint8_t stage,std::uint8_t reason,std::uint32_t value){
    if(!trace_)return;TraceRecord r;r.monotonicNs=SocketReceiver::now();
    r.threadId=GetCurrentThreadId();r.stage=stage;r.reason=reason;r.value=value;
    if(f){r.session=f->measurementSession;r.correlation=f->firstIngressId;
        r.card=f->card;r.trigger=f->trigger;r.packet=f->basePacket;r.sourceIPv4=f->sourceIPv4;}
    trace_->push(r);
}
HostOutput::SaveTailOutcome HostOutput::enqueueSaveFrame(const Frame& frame, bool eligible)
{
    auto* card = sourceSaveRequest_ ? &sourceSaveRequest_->cards[frame->card] : nullptr;
    const bool fault = card && card->fault.load();
    OutputWorkers::Result rejection = OutputWorkers::Result::CardDisabled;
    if (eligible && !fault && workers_.pushCard(frame, &rejection)) {
        if (card) ++card->admitted;
        return frame->complete ? SaveTailOutcome::Admitted : SaveTailOutcome::SourceIncomplete;
    }
    if (card) ++card->rejected;
    if (fault) return SaveTailOutcome::WriterFault;
    if (rejection == OutputWorkers::Result::CardFull) return SaveTailOutcome::QueueFull;
    return SaveTailOutcome::Rejected;
}

void HostOutput::updateSaveHealth(int card, bool notify)
{
    if (!writerSaveRequest_ || savers_[card]->isSaving()) return;
    auto& request = *writerSaveRequest_;
    if (request.cards[card].fault.exchange(true)) return;
    --request.healthy;
    if (notify && request.healthChanged)
        request.healthChanged(request.healthy, int(savers_.size()));
}

void HostOutput::consumeCard(Frame f){
    const auto begin=SocketReceiver::now();
    if (!writerSaveRequest_ || writerSaveRequest_->cards[f->card].fault.load()) {
        observe(f, 5, std::uint8_t(OutputWorkers::Result::SavingDiscard));
        return;
    }
    auto* saver = savers_[f->card];
    const auto acceptedBefore = saver->acceptedCount();
    DataProcessor::DeliveryResult result;
    try {
        auto group = converter_.convertForSave(f);
        result = processors_.at(f->card)->deliverAssembled(group, true, false);
    } catch (...) {
        result.exception = true;
    }
    if (result.exception) {
        if (acceptedBefore == saver->acceptedCount())
            ++writerSaveRequest_->cards[f->card].callbackFailures;
        saver->failSaveCallback();
    }
    updateSaveHealth(f->card);
    // stage 7: 0 save consumer result, 1 frontend enqueue accepted,
    // 2 frontend submit result (FrontendSubmitResult), 3 publisher returned,
    // 4 exception, 5 session stale after conversion.
    // The card worker owns raw save only; reason 1/2 describe frontend queue
    // admission and never masquerade as the asynchronous Ring outcome.
    observe(f,7,0,std::uint32_t(result.save));
    if(result.exception)observe(f,7,4);
    if(timing_){const auto end=SocketReceiver::now();TimingRecord r;r.startNs=begin;r.endNs=end;r.session=f->measurementSession;r.correlation=f->firstIngressId;r.threadId=GetCurrentThreadId();r.card=f->card;r.kind=std::uint16_t(TimingKind::CardWorker);r.value0=std::uint32_t(result.save);timing_->observe(r,end-begin>=500000);}
}
void HostOutput::consumeSync(const SyncFrame& sync){
    std::lock_guard<std::mutex> lock(normalizationMutex_);
    const auto begin=SocketReceiver::now();
    std::vector<TriggerGroupPtr> groups;groups.reserve(sync.cards.size());
    for(auto f:sync.cards)groups.push_back(converter_.convert(f));
    if(!workers_.isCurrentSession(sync.session)){for(auto f:sync.cards)observe(f,7,5);return;}
    for(std::size_t i=0;i<groups.size();++i){auto f=sync.cards[i];
        // beyondCountBoundary groups submit to the frontend signal path only:
        // imagingSubmit=false freezes the imaging publisher for this group;
        // every Pass group keeps the verbatim imagingSubmit=true behavior.
        auto result=processors_.at(f->card)->deliverAssembled(groups[i],false,true,
                                                              !sync.beyondCountBoundary);
        // reason 1/2 record the frontend enqueue outcome only.  The final Ring
        // Accepted/QueueFull/Busy/Disabled outcome stays with ImagingBypass/Ring.
        observe(f,7,1,result.frontendAccepted);
        observe(f,7,2,std::uint32_t(result.frontendSubmit));
        observe(f,7,3,result.publisherAccepted);
        if(result.exception)observe(f,7,4);
    }
    if(timing_){const auto end=SocketReceiver::now();TimingRecord r;r.startNs=begin;r.endNs=end;r.session=sync.session;r.threadId=GetCurrentThreadId();r.card=-1;r.kind=std::uint16_t(TimingKind::SyncWorker);r.value0=std::uint32_t(sync.cards.size());timing_->observe(r,end-begin>=500000);}
}
std::uint64_t HostOutput::configureSaving(std::shared_ptr<SaveRequest> request,
                                          std::function<void()> apply)
{
    const auto generation = workers_.reserveSavingConfiguration();
    auto control = [this, request = std::move(request), generation, apply = std::move(apply)]() mutable {
        cancelTailState(false);
        if (request) failedBoundary_.reset();
        sourceSaveRequest_ = std::move(request);
        workers_.configureSaving(bool(sourceSaveRequest_), std::move(apply), generation);
    };
    if (waitingForFailedBoundary()) deferredSaveControls_.push_back(std::move(control));
    else control();
    return generation;
}

void HostOutput::applySaveRequest(const std::shared_ptr<SaveRequest>& request,
                                  const std::function<void(FileSaver&)>& apply)
{
    settleSaveRequest(false);
    writerSaveRequest_ = request;
    for (std::size_t card = 0; card < savers_.size(); ++card) {
        auto* saver = savers_[card];
        request->cards[card].confirmedAtStart = saver->savedCount();
        try { apply(*saver); }
        catch (...) { saver->failSaveCallback(); }
        updateSaveHealth(int(card), false);
    }
    if (request->healthChanged) request->healthChanged(request->healthy, int(savers_.size()));
}

void HostOutput::settleSaveRequest(bool reconfigure)
{
    auto request = std::move(writerSaveRequest_);
    if (!request) return;
    for (std::size_t card = 0; card < savers_.size(); ++card) {
        auto* saver = savers_[card];
        const auto& totals = request->cards[card];
        FileSaver::SaveResult result{};
        result.cardId = int(card);
        try {
            result = reconfigure ? saver->suspendForSourceRestart() : saver->stopSaving();
        } catch (...) {
            saver->failSaveCallback();
            result.confirmed = saver->savedCount() - totals.confirmedAtStart;
            result.failed = totals.admitted - std::min(totals.admitted, result.confirmed);
            result.ioFault = true;
            result.configurationSegment = reconfigure;
        }
        result.ioFault = result.ioFault || totals.fault.load();
        result.admitted = totals.admitted;
        result.rejected = totals.rejected;
        result.confirmed = std::min(result.confirmed, result.admitted);
        const auto unconfirmed = result.admitted - result.confirmed;
        result.failed = std::min(unconfirmed, result.failed + totals.callbackFailures);
        result.discarded = unconfirmed - result.failed;
        if (!reconfigure) saver->setSessionDirResolver({});
        if (request->settled) request->settled(result);
    }
}

std::uint64_t HostOutput::startSaving(const QString& dir, int count, const QString& suffix,
                                     std::function<QString(std::uint64_t)> directoryFor,
                                     SaveResultSink settled, SaveHealthSink health)
{
    std::lock_guard<std::mutex> lock(normalizationMutex_);
    auto request = std::make_shared<SaveRequest>(savers_.size(), std::move(settled), std::move(health));
    return configureSaving(request, [this, request, dir, count, suffix,
                                      directoryFor = std::move(directoryFor)] {
        applySaveRequest(request, [&](FileSaver& saver) {
            if (directoryFor) saver.setSessionDirResolver(directoryFor);
            saver.startSaving(dir, count, suffix);
        });
    });
}

std::uint64_t HostOutput::stopSaving()
{
    std::lock_guard<std::mutex> lock(normalizationMutex_);
    return configureSaving({}, [this] { settleSaveRequest(false); });
}

std::uint64_t HostOutput::resumeSaving(SaveResultSink settled, SaveHealthSink health)
{
    std::lock_guard<std::mutex> lock(normalizationMutex_);
    auto request = std::make_shared<SaveRequest>(savers_.size(), std::move(settled), std::move(health));
    return configureSaving(request, [this, request] {
        applySaveRequest(request, [](FileSaver& saver) { saver.resumeAfterSourceRestart(); });
    });
}
void HostOutput::requestClose(){for(auto s:savers_)s->requestClose();}
}
