#include "RingReconRoundState.h"
#include "RingReconCompletion.h"
#include "RingSnapshotCopy.h"
#include "RingRoundIdentity.h"
#include "RingBlockAssembler.h"
#include "RingRoundUiState.h"
#include "RingRoundPresentation.h"
#include "PaimageAcquisition/PhysicalRoundNormalizer.h"
#include <QCoreApplication>
#include <QSharedMemory>
#include <QUuid>
#include <algorithm>
#include <array>
#include <iostream>
#include <vector>

int failures = 0;
void check(bool ok, const char *message) {
    if (!ok) { ++failures; std::cerr << "FAIL: " << message << std::endl; }
}

void barrierTest() {
    paimage::RingReconRoundState state;
    using A = paimage::RingReconRoundState::Admission;
    // Model all reconstruction state, including the cross-block WL2 tail.
    int accumulator = 0, wl2Tail = 0, resets = 0;
    auto block = [&](paimage::RoundIdentity round, int value, bool final) {
        const auto result = state.admit(round);
        if (result == A::Stale || result == A::Invalid) return false;
        if (result == A::First || result == A::Transition) {
            accumulator = wl2Tail = 0; ++resets;
        }
        accumulator += value; wl2Tail = value;
        if (final) { state.close(); accumulator = wl2Tail = 0; }
        return true;
    };
    check(!block({0,0}, 1, false), "invalid identity cannot establish accumulator");
    check(block({1,0}, 7, false) && block({1,0}, 7, false), "same round accumulates");
    check(accumulator == 14 && resets == 1, "no counter-based intermediate reset");
    check(block({1,1}, 3, false) && accumulator == 3 && wl2Tail == 3, "new round discards old accumulator and tail");
    check(!block({1,0}, 99, false) && accumulator == 3, "late old round cannot mutate new pixels");
    check(block({1,1}, 3, true) && accumulator == 0 && wl2Tail == 0, "source final closes and clears round");
    check(!block({1,1}, 99, true), "duplicate final cannot reopen closed round");
    check(block({1,5}, 4, false) && accumulator == 4, "generation gap resets");
    state.close(); accumulator = wl2Tail = 0;
    check(!block({1,5}, 9, false), "timeout preserves closed identity floor");
    check(block({2,0}, 5, false) && accumulator == 5, "new session starts clean");
    check(!block({1,99}, 8, false), "older session stays stale regardless of generation");
    check(state.transitions() == 3 && state.staleDrops() == 4, "barrier diagnostics reconcile");
}

struct FakeShm {
    alignas(RingImagingShmHeader) std::array<unsigned char, 128> bytes{};
    bool locked = false, failLock = false, missing = false, mutateAtUnlock = false;
    int locks = 0, unlocks = 0;
    bool lock() { ++locks; if (failLock) return false; locked = true; return true; }
    void *data() { check(locked, "header accessed under lock"); return missing ? nullptr : bytes.data(); }
    void unlock() {
        check(locked, "balanced unlock"); ++unlocks; locked = false;
        if (mutateAtUnlock) {
            auto *h = reinterpret_cast<RingImagingShmHeader *>(bytes.data());
            ++h->frame_seq;
            float newer[2] = {99, 100};
            std::memcpy(h + 1, newer, sizeof(newer));
        }
    }
};
void copyTest() {
    FakeShm shm;
    auto *h = reinterpret_cast<RingImagingShmHeader *>(shm.bytes.data());
    h->frame_seq = 2;
    float pixels[2] = {10, 20};
    std::memcpy(h+1, pixels, sizeof(pixels));
    float out[2] = {-1, -1};
    std::uint32_t actual = 0;
    using R = ring_snapshot::CopyResult;
    check(ring_snapshot::copy(shm, 1, 0, 0, 1, out, actual) == R::SequenceMismatch
          && actual == 2 && out[0] == -1, "old notification never copies newer pixels");
    shm.mutateAtUnlock = true;
    check(ring_snapshot::copy(shm, 2, 0, 0, 1, out, actual) == R::Copied
          && out[0] == 10 && out[1] == 20 && h->frame_seq == 3, "check and copy occur before unlock mutation");
    shm.mutateAtUnlock = false;
    check(ring_snapshot::copy(shm, 3, 0, 0, 1, out, actual) == R::Copied
          && out[0] == 99, "new notification still succeeds after stale drop");
    shm.missing = true;
    check(ring_snapshot::copy(shm, 3, 0, 0, 1, out, actual) == R::MissingData, "missing data unlocks");
    check(shm.locks == shm.unlocks && !shm.locked, "every acquired lock released");
    shm.failLock = true;
    check(ring_snapshot::copy(shm, 3, 0, 0, 1, out, actual) == R::LockFailed, "failed lock never reads");

    QSharedMemory real(QStringLiteral("ring-copy-") + QUuid::createUuid().toString());
    check(real.create(128), "real SHM created");
    if (!real.isAttached()) return;
    real.lock();
    h = static_cast<RingImagingShmHeader *>(real.data());
    h->frame_seq = UINT32_MAX;
    std::memcpy(h+1, pixels, sizeof(pixels));
    real.unlock();
    check(ring_snapshot::copy(real, UINT32_MAX, 0, 0, 1, out, actual) == R::Copied
          && out[1] == 20, "real QSharedMemory uses same copy transaction");
}

void finalMarkerTest() {
    RingBlockAssembler assembler;
    int channels[8] = {1,1,0,0,0,0,0,0};
    assembler.configure(channels, 2, 1, 0, 180, 1, 4, 1, 0);
    std::vector<bool> finals;
    std::vector<paimage::RoundIdentity> rounds;
    assembler.setBlockCallback([&](auto&&, auto&&, auto&&, int, const auto &round, bool final) {
        finals.push_back(final); rounds.push_back(round);
    });
    float line = 1;
    auto push = [&](int channel, int trigger, int generation, bool final) {
        assembler.pushChannelLine(channel, trigger, {1,static_cast<std::uint64_t>(generation)}, &line, 1, final);
    };
    push(0,0,0,false); push(1,0,0,false);
    push(0,1,0,true); push(1,1,0,false);
    check(finals.size() == 1 && finals[0], "one-shot marker retained across channel fan-in");
    push(0,2,0,false); push(1,2,0,false);
    check(finals.size() == 1 && assembler.snapshot().blockTriggers == 0, "closed round rejects delayed channels");
    push(0,3,1,true); push(1,3,1,false);
    check(finals.size() == 1 && assembler.snapshot().blockTriggers == 0, "partial final block discarded without final snapshot");
    push(0,4,2,false); push(1,4,2,false);
    push(0,5,2,false); push(1,5,2,false);
    check(finals.size() == 2 && !finals[1] && rounds[1] == paimage::RoundIdentity{1,2}, "new round cannot inherit final marker");
    const auto message = ring_round_identity::makeSnapshotReady(7, 9, {1,2}, true);
    check(message.value("round_complete").toBool() && ring_round_identity::parse(message).identity == rounds[1], "snapshot codec keeps source completion and identity together");
}

void droppedFinalTest() {
    paimage::RingRoundUiState ui;
    paimage::RingRoundPresentationState presentation;
    paimage::RingPresentationTransition transition;
    transition.oldRound = {1,0}; transition.nextRound = {1,1};
    presentation.registerTransition(transition);
    // Four of five old snapshots arrive; final snapshot is lost.
    for (int i=0; i<4; ++i) check(!ui.noteSnapshot(false), "old partial is not final");
    ui.onCountBoundary();
    check(!ui.noteSnapshot(false), "new round first snapshot cannot fill old modulo");
    check(ui.snapshot().frameEndEvents == 0 && presentation.pendingCount() == 1, "lost final neither saves PNG nor consumes old presentation");
    check(ui.noteSnapshot(true), "source final completes even with missing intermediate snapshots");
    check(!presentation.completeSnapshot({1,1}).exactMatch, "new final cannot consume old round transition");
    check(presentation.completeSnapshot({1,0}).exactMatch, "late matching old final still resolves its own transition");
}

// C1/C2/C3: service completion semantics, driven by the same production seam
// (paimage::evaluateRingReconCompletion) that ImagingSvc::processRingPulse
// calls. The model mirrors the production lifecycle: identity admission via
// RingReconRoundState, consumed-block counting, close/reset on source end.
namespace {
struct SvcCompletionModel {
    explicit SvcCompletionModel(int expectedBlocks) : expected(expectedBlocks) {}
    paimage::RingReconRoundState round;
    std::vector<bool> snapshotFinals;
    std::vector<paimage::RingReconCompletion> mismatches;
    int blockIndex = 0;
    int expected;
    // Returns false when the identity barrier rejected the block before
    // consumption.
    bool consume(paimage::RoundIdentity identity, bool sourceRoundComplete) {
        using A = paimage::RingReconRoundState::Admission;
        const auto admission = round.admit(identity);
        if (admission == A::Stale || admission == A::Invalid) return false;
        if (admission == A::First || admission == A::Transition) {
            blockIndex = 0;   // resetRingRecon on the admission barrier
        }
        ++blockIndex;
        const auto completion = paimage::evaluateRingReconCompletion(
            sourceRoundComplete, blockIndex, expected);
        snapshotFinals.push_back(completion.reconstructionComplete);
        if (completion.sourceRoundComplete) {
            if (completion.blockCountMismatch) mismatches.push_back(completion);
            round.close();
            blockIndex = 0;   // resetRingRecon on source round end
        }
        return true;
    }
};
} // namespace

void completionGateTest() {
    // C1: complete round — only the exact-count final snapshot completes.
    {
        SvcCompletionModel svc(5);
        for (int b = 1; b <= 5; ++b)
            check(svc.consume({1, 0}, b == 5), "C1 complete round blocks admitted");
        check(svc.snapshotFinals == std::vector<bool>({false, false, false, false, true}),
              "C1 only the exact-count final snapshot is complete");
        check(svc.mismatches.empty(), "C1 complete round reports no mismatch");
        check(!svc.consume({1, 0}, true),
              "C1 closed round rejects a late duplicate source-final block");
        for (int b = 1; b <= 5; ++b)
            check(svc.consume({1, 1}, b == 5), "C1 next round admitted");
        check(svc.snapshotFinals.size() == 10 &&
                  std::count(svc.snapshotFinals.begin(), svc.snapshotFinals.end(), true) == 2,
              "C1 next round completes from a clean accumulator");
    }

    // C2: missing middle block, source final still arrives — the remaining
    // production blocker. No final business signal, yet the round still
    // closes and the next round starts clean.
    {
        SvcCompletionModel svc(5);
        check(svc.consume({2, 0}, false), "C2 B1 consumed");
        check(svc.consume({2, 0}, false), "C2 B2 consumed");
        check(svc.consume({2, 0}, false), "C2 B4 consumed");
        check(svc.consume({2, 0}, true), "C2 B5 source-final consumed");
        check(svc.snapshotFinals.size() == 4 &&
                  std::count(svc.snapshotFinals.begin(), svc.snapshotFinals.end(), true) == 0,
              "C2 no snapshot may publish reconstruction complete");
        check(svc.mismatches.size() == 1 &&
                  svc.mismatches.front().blocksConsumed == 4 &&
                  svc.mismatches.front().expectedBlocks == 5 &&
                  svc.mismatches.front().sourceRoundComplete &&
                  !svc.mismatches.front().reconstructionComplete,
              "C2 round_block_count_mismatch carries exact diagnostics");
        check(!svc.consume({2, 0}, false),
              "C2 source end closed the round: late old block rejected");
        for (int b = 1; b <= 5; ++b)
            check(svc.consume({2, 1}, b == 5), "C2 next round admitted");
        check(svc.snapshotFinals.size() == 9 && svc.snapshotFinals.back() &&
                  std::count(svc.snapshotFinals.begin(), svc.snapshotFinals.end(), true) == 1,
              "C2 next round completes at its own exact count");
        check(svc.mismatches.size() == 1, "C2 complete next round adds no mismatch");
    }

    // C3: exact-count truth table of the production seam.
    {
        const auto under = paimage::evaluateRingReconCompletion(true, 4, 5);
        check(!under.reconstructionComplete && under.blockCountMismatch,
              "C3 source end with fewer blocks fails closed");
        const auto exact = paimage::evaluateRingReconCompletion(true, 5, 5);
        check(exact.reconstructionComplete && !exact.blockCountMismatch,
              "C3 source end with the exact count completes");
        const auto over = paimage::evaluateRingReconCompletion(true, 6, 5);
        check(!over.reconstructionComplete && over.blockCountMismatch,
              "C3 source end with more blocks fails closed (no >=)");
        const auto noEnd = paimage::evaluateRingReconCompletion(false, 5, 5);
        check(!noEnd.reconstructionComplete && !noEnd.blockCountMismatch,
              "C3 exact count without source end is not complete");
    }
}

// M2/M3: the Ring final marker rides the stable isFinalLogicalTrigger data
// property, so a Ring-disabled card classified first cannot swallow the round
// end, and duplicate/late terminal arrivals stay idempotent.
void stableTerminalMarkerTest() {
    std::vector<paimage::PhysicalRoundEvent> events;
    paimage::PhysicalRoundNormalizer normalizer(
        2, [&](const paimage::PhysicalRoundEvent &e) { events.push_back(e); });
    normalizer.beginSession(42);

    struct OutBlock { paimage::RoundIdentity round; bool sourceRoundComplete; };
    float line = 1.0f;
    // Feed one card frame exactly like the MainWindow Ring feed: only
    // Ring-enabled channels enter the assembler and the final marker comes
    // from the stable terminal property.
    auto feedCard = [&](RingBlockAssembler &target, const int enabled[8],
                        int card, std::uint16_t trigger) {
        const auto c = normalizer.classify(42, trigger);
        if (c.decision != paimage::PhysicalTriggerDecision::LogicalScan)
            return c;   // control identities never enter the Ring data plane
        const paimage::RoundIdentity round{c.measurementSession, c.roundGeneration};
        const int chA = card * 2;
        for (int ch : {chA, chA + 1}) {
            if (!enabled[ch]) continue;   // ImagingBypass drops Ring-disabled cards
            target.pushChannelLine(ch, trigger, round, &line, 1,
                                   c.isFinalLogicalTrigger);
        }
        return c;
    };

    // M2: card 0 owns Ring channels 0/1; card 1's channels 2/3 are disabled.
    RingBlockAssembler assembler;
    int enabledA[8] = {1, 1, 0, 0, 0, 0, 0, 0};
    assembler.configure(enabledA, 2, 1, 0, 180, 1, 4, 1, 0);
    std::vector<OutBlock> blocks;
    assembler.setBlockCallback([&](auto&&, auto&&, auto&&, int,
                                   const auto &round, bool sourceRoundComplete) {
        blocks.push_back({round, sourceRoundComplete});
    });

    feedCard(assembler, enabledA, 0, 100);   // operational control, filtered
    const auto mid = feedCard(assembler, enabledA, 0, 101);
    check(!mid.roundComplete && !mid.isFinalLogicalTrigger, "M2 mid-round trigger");
    check(blocks.empty(), "M2 partial block not emitted");

    // The final logical trigger is first classified on the Ring-disabled
    // card: the one-shot pulse is consumed where no Ring frame is fed.
    const auto disabledFirst = normalizer.classify(42, 102);
    check(disabledFirst.roundComplete && disabledFirst.isFinalLogicalTrigger,
          "M2 first classification on the disabled card owns the one-shot pulse");

    // The enabled card arrives later with the cached classification: the
    // one-shot pulse is gone, the stable terminal property remains.
    const auto enabledLate = feedCard(assembler, enabledA, 0, 102);
    check(!enabledLate.roundComplete && enabledLate.isFinalLogicalTrigger,
          "M2 enabled late card keeps the stable terminal property");
    check(blocks.size() == 1 && blocks[0].sourceRoundComplete &&
              blocks[0].round == paimage::RoundIdentity{42, 0},
          "M2 complete block still carries source round end");

    // M3: a late same-identity terminal line can neither emit a second final
    // block nor reopen the closed round.
    assembler.pushChannelLine(0, 102, {42, 0}, &line, 1, true);
    check(blocks.size() == 1, "M3 late duplicate terminal line cannot reopen the round");
    check(assembler.snapshot().staleRoundDrops >= 1,
          "M3 late terminal line is stale-dropped");

    // M3 OR: with both cards Ring-enabled, every channel of the final trigger
    // carries the stable terminal property; the fan-in OR still yields
    // exactly one final block for the next round.
    RingBlockAssembler assemblerBoth;
    int enabledB[8] = {1, 1, 1, 1, 0, 0, 0, 0};
    assemblerBoth.configure(enabledB, 2, 1, 0, 180, 1, 4, 1, 0);
    std::vector<OutBlock> blocksBoth;
    assemblerBoth.setBlockCallback([&](auto&&, auto&&, auto&&, int,
                                       const auto &round, bool sourceRoundComplete) {
        blocksBoth.push_back({round, sourceRoundComplete});
    });
    feedCard(assemblerBoth, enabledB, 0, 103);   // control of generation 1
    feedCard(assemblerBoth, enabledB, 0, 104);
    feedCard(assemblerBoth, enabledB, 1, 104);
    const auto finalCard1 = feedCard(assemblerBoth, enabledB, 1, 105);
    const auto finalCard0 = feedCard(assemblerBoth, enabledB, 0, 105);
    check(finalCard1.roundComplete && finalCard1.isFinalLogicalTrigger &&
              !finalCard0.roundComplete && finalCard0.isFinalLogicalTrigger,
          "M3 terminal property stable across card arrival order");
    check(blocksBoth.size() == 1 && blocksBoth[0].sourceRoundComplete &&
              blocksBoth[0].round == paimage::RoundIdentity{42, 1},
          "M3 fan-in OR emits exactly one final block");
    assemblerBoth.pushChannelLine(2, 105, {42, 1}, &line, 1, true);
    check(blocksBoth.size() == 1,
          "M3 late terminal channel after final cannot reopen the round");

    const auto boundaries = std::count_if(
        events.begin(), events.end(), [](const paimage::PhysicalRoundEvent &e) {
            return e.kind == paimage::PhysicalRoundEvent::Kind::CountBoundary;
        });
    check(boundaries == 2, "M2/M3 CountBoundary observer fired once per round");
}

// T1: TimeoutBoundary regression — force-closed partial round: old identity
// stays stale-dropped, new identity starts clean, submit_index stale cutoff
// holds.
void timeoutCompletionRegressionTest() {
    paimage::PhysicalRoundNormalizer normalizer(2);
    normalizer.beginSession(77);
    RingBlockAssembler assembler;
    int channels[8] = {1, 1, 0, 0, 0, 0, 0, 0};
    assembler.configure(channels, 2, 1, 0, 180, 1, 4, 1, 0);
    assembler.setTimeoutManagedExternally(true);
    struct OutBlock { paimage::RoundIdentity round; bool sourceRoundComplete; };
    std::vector<OutBlock> blocks;
    assembler.setBlockCallback([&](auto&&, auto&&, auto&&, int,
                                   const auto &round, bool sourceRoundComplete) {
        blocks.push_back({round, sourceRoundComplete});
    });
    float line = 1.0f;

    // Partial round {77,0}: one logical trigger, no source end.
    normalizer.classify(77, 200);   // control
    const auto mid = normalizer.classify(77, 201);
    const paimage::RoundIdentity oldRound{mid.measurementSession, mid.roundGeneration};
    for (int ch : {0, 1})
        assembler.pushChannelLine(ch, 201, oldRound, &line, 1, mid.isFinalLogicalTrigger);
    check(blocks.empty(), "T1 partial round emits no block");

    // TimeoutBoundary: normalizer closes the partial logical round, the owner
    // reset clears the assembler residual and floors the identity, and the
    // service side closes like the production ring_reset path.
    normalizer.timeoutBoundary(77);
    assembler.resetAfterPhysicalTimeout();
    paimage::RingReconRoundState svcRound;
    using A = paimage::RingReconRoundState::Admission;
    check(svcRound.admit(oldRound) == paimage::RingReconRoundState::Admission::First,
          "T1 service admitted the pre-timeout round");
    svcRound.close();   // ring_reset: ImagingSvc close + resetRingRecon

    // Late old-identity data remains stale on both planes.
    assembler.pushChannelLine(0, 201, oldRound, &line, 1, false);
    check(blocks.empty() && assembler.snapshot().staleRoundDrops >= 1,
          "T1 old identity stays stale-dropped after timeout");
    check(svcRound.admit(oldRound) == A::Stale,
          "T1 service keeps the closed identity floor");

    // The new identity starts clean and completes at its own source end.
    const auto control = normalizer.classify(77, 202);
    check(control.decision == paimage::PhysicalTriggerDecision::OperationalStartupControl,
          "T1 next visible identity is the operational control");
    for (std::uint16_t trigger : {203, 204}) {
        const auto c = normalizer.classify(77, trigger);
        const paimage::RoundIdentity round{c.measurementSession, c.roundGeneration};
        for (int ch : {0, 1})
            assembler.pushChannelLine(ch, trigger, round, &line, 1,
                                      c.isFinalLogicalTrigger);
    }
    check(blocks.size() == 1 && blocks[0].sourceRoundComplete &&
              blocks[0].round == paimage::RoundIdentity{77, 1},
          "T1 new round completes cleanly after timeout");

    // Producer submit_index stale cutoff keeps rejecting pre-reset snapshots.
    paimage::RingRoundUiState ui;
    ui.recordSubmitIndex(1);
    ui.onTimeoutBoundary();
    ui.armStaleCutoff(1);
    check(ui.admitSnapshotDetailed(1) == paimage::RingRoundUiState::SnapshotAdmission::Stale,
          "T1 submit_index stale cutoff keeps rejecting old snapshots");
    check(ui.admitSnapshotDetailed(2) == paimage::RingRoundUiState::SnapshotAdmission::Accepted,
          "T1 new submit_index admitted after cutoff");
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    barrierTest(); copyTest(); finalMarkerTest(); droppedFinalTest();
    completionGateTest(); stableTerminalMarkerTest(); timeoutCompletionRegressionTest();
    std::cout << "ring_production_blockers_test failures=" << failures << std::endl;
    return failures ? 1 : 0;
}
