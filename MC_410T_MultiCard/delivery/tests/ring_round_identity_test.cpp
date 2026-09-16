#include "RingRoundIdentity.h"
#include "RingRoundPresentation.h"
#include "RingRoundUiState.h"

#include <cstdint>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const std::string &message)
{
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

paimage::AutoSaveRoundCoordinator::CommitResult commit(
    std::uint64_t session, std::uint64_t generation)
{
    paimage::AutoSaveRoundCoordinator::CommitResult result;
    result.enabled = true;
    result.committed = true;
    result.measurementSession = session;
    result.roundGeneration = generation;
    result.boundaryKind = paimage::AutoSaveBoundaryKind::Count;
    result.oldSessionGen = generation;
    result.newSessionGen = generation + 1;
    result.publishedSessionGen = result.newSessionGen;
    result.oldDirectory = QStringLiteral("old-%1").arg(
        static_cast<qulonglong>(generation - 1));
    result.directory = QStringLiteral("new-%1").arg(
        static_cast<qulonglong>(generation));
    return result;
}

void testIdentityCodec()
{
    QJsonObject object;
    const paimage::RoundIdentity identity{
        UINT64_C(18446744073709551615), UINT64_C(18446744073709551614)};
    ring_round_identity::add(object, identity);
    const auto parsed = ring_round_identity::parse(object);
    check(parsed.valid && parsed.identity == identity,
          "R14 uint64 RoundIdentity decimal-string round trip");

    QJsonObject numeric = object;
    numeric[QString::fromLatin1(ring_round_identity::kMeasurementSession)] =
        static_cast<qint64>(1);
    check(!ring_round_identity::parse(numeric).valid,
          "R14 numeric JSON identity is rejected instead of lossy parsing");
    QJsonObject missing;
    check(!ring_round_identity::parse(missing).valid,
          "R5 missing identity is rejected");
}

void testMultipleTransitions()
{
    paimage::RingRoundPresentationState state;
    paimage::RingPresentationTransition first;
    paimage::RingPresentationTransition second;
    check(paimage::RingPresentationTransition::fromCommit(commit(7, 1), first),
          "R6 first transition can be built");
    check(paimage::RingPresentationTransition::fromCommit(commit(7, 2), second),
          "R6 second transition can be built");
    check(state.registerTransition(first) ==
              paimage::RingRoundPresentationState::RegisterResult::Registered,
          "R6 first transition registered");
    check(state.registerTransition(second) ==
              paimage::RingRoundPresentationState::RegisterResult::Registered,
          "R6 second transition registered without overwriting first");

    const auto firstSnapshot = state.completeSnapshot({7, 0});
    check(firstSnapshot.action ==
              paimage::RingRoundPresentationState::SnapshotAction::Applied &&
              firstSnapshot.shouldApply && firstSnapshot.transition.nextRound ==
                  paimage::RoundIdentity{7, 1} &&
              firstSnapshot.allowOldDirectory,
          "R6 old round exact match applies its own transition");
    const auto secondSnapshot = state.completeSnapshot({7, 1});
    check(secondSnapshot.action ==
              paimage::RingRoundPresentationState::SnapshotAction::Applied &&
              state.currentTarget() == paimage::RoundIdentity{7, 2},
          "R6 second old round exact match advances to the second target");
}

void testReverseSnapshotDoesNotRollback()
{
    paimage::RingRoundPresentationState state;
    paimage::RingPresentationTransition first;
    paimage::RingPresentationTransition second;
    paimage::RingPresentationTransition::fromCommit(commit(8, 1), first);
    paimage::RingPresentationTransition::fromCommit(commit(8, 2), second);
    state.registerTransition(first);
    state.registerTransition(second);

    const auto newer = state.completeSnapshot({8, 1});
    const auto delayed = state.completeSnapshot({8, 0});
    check(newer.shouldApply && newer.transition.nextRound ==
              paimage::RoundIdentity{8, 2},
          "R7 newer snapshot can arrive first");
    check(delayed.action == paimage::RingRoundPresentationState::SnapshotAction::Stale &&
              delayed.allowOldDirectory && !delayed.shouldApply &&
              delayed.transition.oldRound == paimage::RoundIdentity{8, 0},
          "R7 delayed old snapshot is stale but retains its own old directory");
    check(state.currentTarget() == paimage::RoundIdentity{8, 2},
          "R7 delayed old snapshot cannot roll presentation target back");
}

void testDuplicateIsIdempotent()
{
    paimage::RingRoundPresentationState state;
    paimage::RingPresentationTransition transition;
    paimage::RingPresentationTransition::fromCommit(commit(9, 1), transition);
    state.registerTransition(transition);
    const auto first = state.completeSnapshot({9, 0});
    const auto duplicate = state.completeSnapshot({9, 0});
    check(first.shouldApply && duplicate.action ==
              paimage::RingRoundPresentationState::SnapshotAction::Duplicate &&
              !duplicate.shouldApply,
          "R8 duplicate final snapshot is idempotent");
    check(state.registerTransition(transition) ==
              paimage::RingRoundPresentationState::RegisterResult::Duplicate,
          "R8 duplicate boundary registration does not allocate another transition");
}

void testTimeoutCutoffAndSessionRestart()
{
    paimage::RingRoundUiState ui;
    ui.recordSubmitIndex(1);
    ui.recordSubmitIndex(2);
    ui.onTimeoutBoundary();
    ui.armStaleCutoff(2);
    check(ui.admitSnapshotDetailed(2) ==
              paimage::RingRoundUiState::SnapshotAdmission::Stale,
          "R9 old snapshot is rejected by submit_index cutoff");
    check(ui.admitSnapshotDetailed(3) ==
              paimage::RingRoundUiState::SnapshotAdmission::Accepted,
          "R9 new submit_index remains admissible after cutoff");

    paimage::RingRoundPresentationState state;
    paimage::RingPresentationTransition oldTransition;
    paimage::RingPresentationTransition::fromCommit(commit(10, 1), oldTransition);
    state.registerTransition(oldTransition);
    // The stale UI admission returns before presentation lookup, so the old
    // transition remains unconsumed and cannot affect the new round.
    check(state.pendingCount() == 1,
          "R9 stale snapshot does not consume presentation transition");

    state.reset();
    paimage::RingPresentationTransition newSessionTransition;
    paimage::RingPresentationTransition::fromCommit(commit(11, 1),
                                                     newSessionTransition);
    state.registerTransition(newSessionTransition);
    check(state.completeSnapshot({10, 0}).action ==
              paimage::RingRoundPresentationState::SnapshotAction::Missing,
          "R10 old measurement session cannot match new session state");
    check(state.completeSnapshot({11, 0}).shouldApply &&
              state.currentTarget() == paimage::RoundIdentity{11, 1},
          "R10 new measurement session starts a clean presentation map");
}

} // namespace

int main()
{
    testIdentityCodec();
    testMultipleTransitions();
    testReverseSnapshotDoesNotRollback();
    testDuplicateIsIdempotent();
    testTimeoutCutoffAndSessionRestart();
    if (failures != 0) {
        std::cerr << "ring_round_identity_test: " << failures
                  << " FAILURE(S)\n";
        return 1;
    }
    std::cout << "ring_round_identity_test: ALL PASS (R5-R10,R14)\n";
    return 0;
}
