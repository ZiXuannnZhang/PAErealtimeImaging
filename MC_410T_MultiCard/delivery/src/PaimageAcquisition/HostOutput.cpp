#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "PaimageAcquisition/HostOutput.h"
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
                const bool canFail = saveRequested_ && !failedBoundary_ && saveBoundaryFaulted_ &&
                    (event.kind == PhysicalRoundEvent::Kind::CountBoundary ||
                     event.kind == PhysicalRoundEvent::Kind::TimeoutBoundary) &&
                    !saveBoundaryFaulted_();
                if (observer) observer(event);
                if (canFail && saveBoundaryFaulted_()) failSaveBoundary(event);
            });
        normalizer_->setTimeoutResetSec(physicalRoundTimeoutSec);
        normalizer_->setStartupFilterTriggerCount(startupFilterTriggerCount);
        normalizer_->setDisableCountBoundary(disableCountBoundary);
    }
    for(std::size_t i=0;i<processors_.size();++i){
        auto saver=savers_[i];processors_[i]->setDirectSaveSink([saver](const TriggerGroupPtr& f){return saver->consumeTriggerGroup(f);});
    }
    workers_.setSavingHooks([this]{for(auto s:savers_)s->serviceCloseRequest();},
                           [this]{for(auto s:savers_){if(configurationRestart_)s->suspendForSourceRestart();else s->stopSaving();}});
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
    workers_.enqueueSaveBarrier([this,card,round]{savers_.at(card)->sealRound(round);});
}
void HostOutput::beginTailSeals(const PhysicalRoundEvent& event){
    if(!event.roundGeneration)return;
    const RoundIdentity old{event.measurementSession,event.roundGeneration-1};
    const Time cutoff=preBoundarySnapshot_.lastDistinctTriggerTimeNs+preBoundarySnapshot_.timeoutResetNs;
    for(std::size_t c=0;c<tails_.size();++c){
        if(tails_[c].waiting)sealCard(int(c),tails_[c].round);
        auto& t=tails_[c];t={};const auto& a=activeAssemblies_[c];
        t.round=old;t.deadline=cutoff+kSaveTailGraceNs;
        t.generation=saveSessionResolver_ ? saveSessionResolver_(old.measurementSession,old.roundGeneration) : 0;
        t.waiting=a.round==old && cardSaveRounds_[c]==old && a.ingress && a.first>0 && a.first<=cutoff;
        if(t.waiting)t.active=a;
        if(!t.waiting)sealCard(int(c),old);
    }
}
void HostOutput::finishTailSeals(Time now,const std::vector<Time>* cardWatermarks){
    for(std::size_t c=0;c<tails_.size();++c)if(tails_[c].waiting && (cardWatermarks ? (c<cardWatermarks->size()?(*cardWatermarks)[c]:0) : now)>tails_[c].deadline){
        tails_[c].waiting=false;sealCard(int(c),tails_[c].round);
        if (failedBoundary_ && failedBoundary_->round == tails_[c].round && saveTailSink_)
            saveTailSink_(tails_[c].round, failedBoundary_->trigger, int(c), false);
    }
}
void HostOutput::cancelTailState(bool seal){
    for(std::size_t c=0;c<tails_.size();++c){
        if(seal && tails_[c].waiting)sealCard(int(c),tails_[c].round);
        tails_[c]={};activeAssemblies_[c]={};cardSaveRounds_[c]={};
    }
}
void HostOutput::failSaveBoundary(const PhysicalRoundEvent& event)
{
    if (!event.roundGeneration) return;
    FailedBoundary failure;
    failure.round = {event.measurementSession, event.roundGeneration - 1};
    failure.count = event.kind == PhysicalRoundEvent::Kind::CountBoundary;
    failure.trigger = event.triggerSeq;
    if (failure.count) {
        failure.saveGeneration = saveSessionResolver_
            ? saveSessionResolver_(failure.round.measurementSession, failure.round.roundGeneration) : 0;
        failure.deadline = boundaryObservedNs_ + kSaveTailGraceNs;
        failure.waiting.assign(savers_.size(), true);
    }
    failedBoundary_ = std::move(failure);
}

bool HostOutput::waitingForFailedBoundary() const
{
    if (!failedBoundary_) return false;
    if (failedBoundary_->count)
        return std::any_of(failedBoundary_->waiting.begin(), failedBoundary_->waiting.end(),
                           [](bool waiting) { return waiting; });
    return std::any_of(tails_.begin(), tails_.end(), [this](const Tail& tail) {
        return tail.round == failedBoundary_->round && tail.waiting;
    });
}

void HostOutput::finishFailedBoundary()
{
    if (waitingForFailedBoundary()) return;
    auto controls = std::move(deferredSaveControls_);
    deferredSaveControls_.clear();
    for (auto& apply : controls) apply();
}

void HostOutput::expireFailedBoundary(Time now, const std::vector<Time>* cardWatermarks)
{
    if (failedBoundary_ && failedBoundary_->count) {
        auto& failure = *failedBoundary_;
        for (std::size_t card = 0; card < failure.waiting.size(); ++card) {
            const auto watermark = cardWatermarks && card < cardWatermarks->size()
                ? (*cardWatermarks)[card] : now;
            if (!failure.waiting[card] || watermark <= failure.deadline) continue;
            failure.waiting[card] = false;
            if (saveTailSink_) saveTailSink_(failure.round, failure.trigger, int(card), false);
        }
    }
    finishFailedBoundary();
}

void HostOutput::pushSaveFrame(const Frame& frame, const PhysicalRoundClassification& classification,
                               bool timeoutEligible)
{
    if (!failedBoundary_) {
        workers_.pushCard(frame);
        return;
    }
    auto& failure = *failedBoundary_;
    if (!failure.count) {
        if (timeoutEligible) {
            const bool queued = workers_.pushCard(frame);
            if (saveTailSink_) saveTailSink_(failure.round, frame->trigger, frame->card, queued);
        }
        return;
    }
    const RoundIdentity round{classification.measurementSession, classification.roundGeneration};
    if (round != failure.round || frame->trigger != failure.trigger || !classification.isFinalLogicalTrigger ||
        !failure.waiting[frame->card] || frame->closed <= 0 || frame->closed > failure.deadline)
        return;

    failure.waiting[frame->card] = false;
    converter_.tagSaveSession(frame, failure.saveGeneration);
    const bool queued = workers_.pushCard(frame);
    if (saveTailSink_) saveTailSink_(failure.round, frame->trigger, frame->card, queued);
    finishFailedBoundary();
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
    converter_.tagSaveSession(f,saveGeneration);const auto begin=SocketReceiver::now();const auto session=f->measurementSession,link=f->firstIngressId;const int card=f->card;
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
    if(token){
        const bool accept=tail.waiting && f->closed>0 && f->closed<=tail.deadline;
        converter_.tagSaveOverride(f,session,tail.round.roundGeneration,tail.generation,!accept);
        if (tail.waiting) {
            tail.waiting = false;
            const auto closing = tail.round;
            pushSaveFrame(f, classification, accept);
            if (!accept && failedBoundary_ && failedBoundary_->round == closing && saveTailSink_)
                saveTailSink_(closing, f->trigger, card, false);
            sealCard(card, closing);
            auto& active = activeAssemblies_[card];
            if (active.session == session && active.ingress == link && active.first == f->first) active = {};
            finishFailedBoundary();
            return;
        }
    }else if(staleCached || (classification.measurementSession==timeoutSession_ && classification.roundGeneration<timeoutGeneration_)){
        converter_.tagSaveOverride(f,session,classification.roundGeneration,saveGeneration,true);
    }
    if(tail.waiting && classification.roundGeneration>tail.round.roundGeneration &&
       !(failedBoundary_ && !failedBoundary_->count && failedBoundary_->round == tail.round)){
        tail.waiting=false;sealCard(card,tail.round);
    }
    if(classification.decision==PhysicalTriggerDecision::LogicalScan)cardSaveRounds_[card]={session,classification.roundGeneration};
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
void HostOutput::consumeCard(Frame f){
    const auto begin=SocketReceiver::now();
    auto group=converter_.convertForSave(f);
    auto result=processors_.at(f->card)->deliverAssembled(group,true,false);
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
std::uint64_t HostOutput::configureSaving(bool enabled, std::function<void()> apply)
{
    const auto generation = workers_.reserveSavingConfiguration();
    auto control = [this, enabled, generation, apply = std::move(apply)]() mutable {
        cancelTailState(false);
        if (enabled) failedBoundary_.reset();
        saveRequested_ = enabled;
        workers_.configureSaving(enabled, std::move(apply), generation);
    };
    if (waitingForFailedBoundary()) deferredSaveControls_.push_back(std::move(control));
    else control();
    return generation;
}

std::uint64_t HostOutput::startSaving(const QString& dir, int count, const QString& suffix,
                                     std::function<QString(std::uint64_t)> directoryFor)
{
    std::lock_guard<std::mutex> lock(normalizationMutex_);
    return configureSaving(true, [this, dir, count, suffix,
                                           directoryFor = std::move(directoryFor)] {
        for (auto saver : savers_) {
            if (directoryFor) saver->setSessionDirResolver(directoryFor);
            saver->startSaving(dir, count, suffix);
        }
    });
}

std::uint64_t HostOutput::stopSaving(SaveResultSink done)
{
    std::lock_guard<std::mutex> lock(normalizationMutex_);
    return configureSaving(false, [this, done = std::move(done)] {
        for (auto saver : savers_) {
            const auto result = saver->stopSaving();
            // Release the old request's bindings only after its accepted prefix
            // has been written and both lanes have settled.
            if (done) {
                saver->setSessionDirResolver({});
                done(result);
            }
        }
    });
}
std::uint64_t HostOutput::resumeSaving(){
    std::lock_guard<std::mutex> lock(normalizationMutex_);
    return configureSaving(true,[this]{for(auto s:savers_)s->resumeAfterSourceRestart();});
}
void HostOutput::requestClose(){for(auto s:savers_)s->requestClose();}
}
