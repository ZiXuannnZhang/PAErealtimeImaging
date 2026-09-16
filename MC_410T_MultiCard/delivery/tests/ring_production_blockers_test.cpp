#include "RingReconRoundState.h"
#include "RingSnapshotCopy.h"
#include "RingRoundIdentity.h"
#include "RingBlockAssembler.h"
#include "RingRoundUiState.h"
#include "RingRoundPresentation.h"
#include <QCoreApplication>
#include <QSharedMemory>
#include <QUuid>
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
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    barrierTest(); copyTest(); finalMarkerTest(); droppedFinalTest();
    std::cout << "ring_production_blockers_test failures=" << failures << std::endl;
    return failures ? 1 : 0;
}
