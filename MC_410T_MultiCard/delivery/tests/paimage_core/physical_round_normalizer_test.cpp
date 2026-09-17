#include "PaimageAcquisition/PhysicalRoundNormalizer.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace paimage;

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

PhysicalRoundClassification classify(PhysicalRoundNormalizer& normalizer,
                                     std::uint64_t session,
                                     std::uint16_t trigger,
                                     std::int64_t observedMonotonicNs = 0) {
    return normalizer.classify(session, trigger, observedMonotonicNs);
}
}

int main() {
    // T1: one operational control identity plus N logical identities.
    {
        std::vector<PhysicalRoundEvent> events;
        PhysicalRoundNormalizer normalizer(3, [&](const auto& event) { events.push_back(event); });
        normalizer.beginSession(7);
        require(classify(normalizer, 7, 10).decision ==
                    PhysicalTriggerDecision::OperationalStartupControl,
                "T1 control");
        require(classify(normalizer, 7, 11).logicalTriggerIndex == 0, "T1 index zero");
        require(classify(normalizer, 7, 12).logicalTriggerIndex == 1, "T1 index one");
        const auto last = classify(normalizer, 7, 13);
        require(last.decision == PhysicalTriggerDecision::LogicalScan &&
                    last.logicalTriggerIndex == 2 && last.roundComplete,
                "T1 final logical");
        const auto snapshot = normalizer.snapshot();
        require(snapshot.physicalDistinctObserved == 4 &&
                    snapshot.operationalControlFiltered == 1 &&
                    snapshot.logicalDistinctAccepted == 3 &&
                    snapshot.countBoundaryResets == 1 &&
                    snapshot.roundGeneration == 1 &&
                    snapshot.state == PhysicalRoundState::AwaitingControl,
                "T1 counters");
        require(events.size() == 2 &&
                    events.front().kind == PhysicalRoundEvent::Kind::ControlFiltered &&
                    events.front().basis == "first-visible-operational" &&
                    events.back().kind == PhysicalRoundEvent::Kind::CountBoundary,
                "T1 events");
    }

    // T1 also covers the production-sized configured value. The normalizer
    // receives the count from configuration; 4000 is not embedded in its
    // implementation.
    {
        PhysicalRoundNormalizer normalizer(4000);
        normalizer.beginSession(7000);
        require(classify(normalizer, 7000, 0).decision ==
                    PhysicalTriggerDecision::OperationalStartupControl,
                "T1 4000 control");
        for (std::uint16_t trigger = 1; trigger <= 4000; ++trigger) {
            const auto classification = classify(normalizer, 7000, trigger);
            require(classification.decision == PhysicalTriggerDecision::LogicalScan &&
                        classification.logicalTriggerIndex == trigger - 1 &&
                        classification.roundComplete == (trigger == 4000),
                    "T1 4000 logical sequence");
        }
        const auto snapshot = normalizer.snapshot();
        require(snapshot.physicalDistinctObserved == 4001 &&
                    snapshot.operationalControlFiltered == 1 &&
                    snapshot.logicalDistinctAccepted == 4000 &&
                    snapshot.countBoundaryResets == 1,
                "T1 4000 counters");
    }

    // T2: three independent rounds use exactly 1+N identities each.
    {
        PhysicalRoundNormalizer normalizer(3);
        normalizer.beginSession(8);
        for (std::uint16_t base : {100, 110, 120}) {
            require(classify(normalizer, 8, base).decision ==
                        PhysicalTriggerDecision::OperationalStartupControl,
                    "T2 control");
            classify(normalizer, 8, static_cast<std::uint16_t>(base + 1));
            classify(normalizer, 8, static_cast<std::uint16_t>(base + 2));
            require(classify(normalizer, 8, static_cast<std::uint16_t>(base + 3)).roundComplete,
                    "T2 boundary");
        }
        const auto snapshot = normalizer.snapshot();
        require(snapshot.physicalDistinctObserved == 12 &&
                    snapshot.logicalDistinctAccepted == 9 &&
                    snapshot.countBoundaryResets == 3 && snapshot.roundGeneration == 3,
                "T2 counters");
    }

    // T3: a late card for the last logical identity keeps the original
    // decision after the count boundary and cannot become the next control.
    {
        PhysicalRoundNormalizer normalizer(2);
        normalizer.beginSession(9);
        classify(normalizer, 9, 200);
        classify(normalizer, 9, 201);
        const auto final = classify(normalizer, 9, 202);
        const auto late = classify(normalizer, 9, 202);
        require(final.roundComplete && late.decision == PhysicalTriggerDecision::LogicalScan &&
                    late.logicalTriggerIndex == 1 && !late.roundComplete && !late.newDistinct,
                "T3 late card");
        require(classify(normalizer, 9, 203).decision ==
                    PhysicalTriggerDecision::OperationalStartupControl,
                "T3 next control");
        require(normalizer.snapshot().physicalDistinctObserved == 4, "T3 distinct count");
    }

    // T4: reordered multi-card delivery shares one cached decision per
    // physical identity and does not advance counters twice.
    {
        PhysicalRoundNormalizer normalizer(2);
        normalizer.beginSession(10);
        const auto c0 = classify(normalizer, 10, 300);
        const auto c1 = classify(normalizer, 10, 300);
        const auto s0 = classify(normalizer, 10, 301);
        const auto s1 = classify(normalizer, 10, 301);
        require(c0.decision == c1.decision && s0.decision == s1.decision &&
                    c0.newDistinct && !c1.newDistinct && s0.newDistinct && !s1.newDistinct,
                "T4 card sharing");
        require(normalizer.snapshot().physicalDistinctObserved == 2 &&
                    normalizer.snapshot().logicalDistinctAccepted == 1,
                "T4 counters");
    }

    // T5: completeness is not part of classification; a partial first
    // identity is still the operational control filter.
    {
        PhysicalRoundNormalizer normalizer(2);
        normalizer.beginSession(11);
        require(classify(normalizer, 11, 400).decision ==
                    PhysicalTriggerDecision::OperationalStartupControl,
                "T5 partial control");
        require(classify(normalizer, 11, 401).logicalTriggerIndex == 0,
                "T5 logical after partial control");
    }

    // T6: a control identity visible on only one card still establishes the
    // shared decision; the next identity is logical for all cards.
    {
        PhysicalRoundNormalizer normalizer(2);
        normalizer.beginSession(12);
        const auto card0Control = classify(normalizer, 12, 500);
        const auto card1Control = classify(normalizer, 12, 500);
        const auto card1Scan = classify(normalizer, 12, 501);
        const auto card0Scan = classify(normalizer, 12, 501);
        require(card0Control.decision == PhysicalTriggerDecision::OperationalStartupControl &&
                    card1Control.decision == card0Control.decision &&
                    card1Scan.decision == PhysicalTriggerDecision::LogicalScan &&
                    card0Scan.decision == card1Scan.decision,
                "T6 shared decision");
    }

    // T7: when the operational control is invisible, the first visible scan
    // is filtered and only N-1 logical scans remain in the round.
    {
        PhysicalRoundNormalizer normalizer(4);
        normalizer.beginSession(13);
        require(classify(normalizer, 13, 600).decision ==
                    PhysicalTriggerDecision::OperationalStartupControl,
                "T7 first visible filter");
        for (std::uint16_t trigger = 601; trigger <= 603; ++trigger)
            require(classify(normalizer, 13, trigger).decision ==
                        PhysicalTriggerDecision::LogicalScan,
                    "T7 logical");
        const auto snapshot = normalizer.snapshot();
        require(snapshot.logicalDistinctAccepted == 3 &&
                    snapshot.currentLogicalDistinctCount == 3 &&
                    snapshot.countBoundaryResets == 0,
                "T7 N-1");
    }

    // T8: a timeout clears a partial logical round and requires a new control.
    {
        std::vector<PhysicalRoundEvent> events;
        PhysicalRoundNormalizer normalizer(4, [&](const auto& event) { events.push_back(event); });
        normalizer.beginSession(14);
        classify(normalizer, 14, 700);
        classify(normalizer, 14, 701);
        normalizer.timeoutBoundary(14);
        auto snapshot = normalizer.snapshot();
        require(snapshot.timeoutBoundaryResets == 1 && snapshot.roundGeneration == 1 &&
                    snapshot.currentLogicalDistinctCount == 0 &&
                    snapshot.state == PhysicalRoundState::AwaitingControl,
                "T8 timeout");
        require(classify(normalizer, 14, 702).decision ==
                    PhysicalTriggerDecision::OperationalStartupControl,
                "T8 control after timeout");
        require(events.size() == 3 &&
                    events[1].kind == PhysicalRoundEvent::Kind::TimeoutBoundary &&
                    events[2].kind == PhysicalRoundEvent::Kind::ControlFiltered,
                "T8 event");
    }

    // T9: timeout after a count boundary is an idempotent no-op.
    {
        PhysicalRoundNormalizer normalizer(1);
        normalizer.beginSession(15);
        classify(normalizer, 15, 800);
        require(classify(normalizer, 15, 801).roundComplete, "T9 count boundary");
        normalizer.timeoutBoundary(15);
        const auto snapshot = normalizer.snapshot();
        require(snapshot.countBoundaryResets == 1 && snapshot.timeoutBoundaryResets == 0 &&
                    snapshot.roundGeneration == 1,
                "T9 no double reset");
        require(classify(normalizer, 15, 802).decision ==
                    PhysicalTriggerDecision::OperationalStartupControl,
                "T9 next control");
    }

    // T10: uint16 trigger sequence wrap does not change identity handling.
    {
        PhysicalRoundNormalizer normalizer(3);
        normalizer.beginSession(16);
        require(classify(normalizer, 16, 0xffffU - 1U).decision ==
                    PhysicalTriggerDecision::OperationalStartupControl,
                "T10 control before wrap");
        require(classify(normalizer, 16, 0xffffU).logicalTriggerIndex == 0,
                "T10 65535");
        require(classify(normalizer, 16, 0).logicalTriggerIndex == 1, "T10 zero");
        require(classify(normalizer, 16, 1).roundComplete, "T10 one");
    }

    // T12: a new measurement session clears the prior identity cache and
    // starts in AwaitingControl even when the wire sequence is reused.
    {
        PhysicalRoundNormalizer normalizer(2);
        normalizer.beginSession(17);
        classify(normalizer, 17, 900);
        classify(normalizer, 17, 901);
        normalizer.beginSession(18);
        const auto first = classify(normalizer, 18, 900);
        require(first.decision == PhysicalTriggerDecision::OperationalStartupControl &&
                    first.newDistinct && normalizer.snapshot().physicalDistinctObserved == 1,
                    "T12 session reset");
    }

    // T13 / timeout T1: a production-style monotonic idle gap is detected
    // while classifying the next distinct identity. That identity becomes the
    // new operational control and the partial logical count is discarded.
    {
        std::vector<PhysicalRoundEvent> events;
        PhysicalRoundNormalizer normalizer(3, [&](const auto& event) { events.push_back(event); });
        normalizer.setTimeoutResetSec(1.0e-6);
        normalizer.beginSession(19);
        classify(normalizer, 19, 100, 1000);
        classify(normalizer, 19, 101, 1500);
        const auto late = classify(normalizer, 19, 101, 5000);
        const auto control = classify(normalizer, 19, 102, 3501);
        require(!late.newDistinct && control.decision ==
                    PhysicalTriggerDecision::OperationalStartupControl,
                "timeout T1 late card/control classification");
        const auto snapshot = normalizer.snapshot();
        require(snapshot.timeoutBoundaryResets == 1 && snapshot.roundGeneration == 1 &&
                    snapshot.currentLogicalDistinctCount == 0 &&
                    snapshot.lastDistinctTriggerSeq == 102 &&
                    snapshot.timeoutResetNs == 1000,
                "timeout T1 snapshot");
        require(events.size() == 3 &&
                    events[1].kind == PhysicalRoundEvent::Kind::TimeoutBoundary &&
                    events[1].basis == "physical-idle-timeout" &&
                    events[1].idleDurationNs == 2001 &&
                    events[1].configuredTimeoutNs == 1000 &&
                    events[1].hasLastDistinctTriggerSeq &&
                    events[1].lastDistinctTriggerSeq == 101 &&
                    events[1].hasNextVisibleTriggerSeq &&
                    events[1].nextVisibleTriggerSeq == 102,
                "timeout T1 diagnostic event");
        require(classify(normalizer, 19, 103, 3600).logicalTriggerIndex == 0,
                "timeout T1 post-control index zero");
    }

    // T14 / timeout T2: zero disables the idle boundary; a long gap alone
    // cannot filter the next identity or increment timeout counters.
    {
        PhysicalRoundNormalizer normalizer(2);
        normalizer.setTimeoutResetSec(0.0);
        normalizer.beginSession(20);
        require(classify(normalizer, 20, 110, 1000).decision ==
                    PhysicalTriggerDecision::OperationalStartupControl,
                "timeout T2 control");
        require(classify(normalizer, 20, 111, 2000000000LL).logicalTriggerIndex == 0,
                "timeout T2 first logical");
        const auto final = classify(normalizer, 20, 112, 4000000000LL);
        const auto snapshot = normalizer.snapshot();
        require(final.roundComplete && snapshot.timeoutBoundaryResets == 0 &&
                    snapshot.countBoundaryResets == 1,
                "timeout T2 disabled");
    }

    // T15 / timeout T4: count completion already leaves the normalizer idle;
    // a later identity starts the next round without a second timeout reset.
    {
        PhysicalRoundNormalizer normalizer(1);
        normalizer.setTimeoutResetSec(1.0e-6);
        normalizer.beginSession(21);
        classify(normalizer, 21, 120, 1000);
        require(classify(normalizer, 21, 121, 1500).roundComplete,
                "timeout T4 count boundary");
        const auto next = classify(normalizer, 21, 122, 1000000000LL);
        const auto snapshot = normalizer.snapshot();
        require(next.decision == PhysicalTriggerDecision::OperationalStartupControl &&
                    snapshot.countBoundaryResets == 1 &&
                    snapshot.timeoutBoundaryResets == 0 &&
                    snapshot.roundGeneration == 1,
                "timeout T4 idempotence");
    }

    // T16 / timeout T5: a cached late card must not move the idle anchor. The
    // later distinct identity therefore observes the original physical gap.
    {
        std::vector<PhysicalRoundEvent> events;
        PhysicalRoundNormalizer normalizer(3, [&](const auto& event) { events.push_back(event); });
        normalizer.setTimeoutResetSec(1.0e-6);
        normalizer.beginSession(22);
        classify(normalizer, 22, 130, 1000);
        classify(normalizer, 22, 131, 1500);
        classify(normalizer, 22, 131, 100000);
        classify(normalizer, 22, 132, 2000);
        const auto control = classify(normalizer, 22, 133, 4001);
        const auto snapshot = normalizer.snapshot();
        require(control.decision == PhysicalTriggerDecision::OperationalStartupControl &&
                    snapshot.timeoutBoundaryResets == 1 && events.size() == 3 &&
                    events[1].lastDistinctTriggerSeq == 132 &&
                    events[1].idleDurationNs == 2001,
                "timeout T5 late card anchor");
    }

    // T17 / timeout T6: the idle boundary carries the wire identity across
    // uint16 wrap without using numeric trigger order.
    {
        std::vector<PhysicalRoundEvent> events;
        PhysicalRoundNormalizer normalizer(2, [&](const auto& event) { events.push_back(event); });
        normalizer.setTimeoutResetSec(1.0e-6);
        normalizer.beginSession(23);
        classify(normalizer, 23, 0xffffU, 1000);
        classify(normalizer, 23, 0, 1500);
        const auto control = classify(normalizer, 23, 1, 4000);
        require(control.decision == PhysicalTriggerDecision::OperationalStartupControl &&
                    events.size() == 3 &&
                    events[1].lastDistinctTriggerSeq == 0 &&
                    events[1].nextVisibleTriggerSeq == 1,
                "timeout T6 wrap");
    }

    // T18: the final logical trigger carries a stable terminal data property.
    // The one-shot roundComplete pulse fires once, but isFinalLogicalTrigger
    // stays true on cached classifications of the same trigger identity, and
    // the CountBoundary observer still fires exactly once.
    {
        std::vector<PhysicalRoundEvent> events;
        PhysicalRoundNormalizer normalizer(3, [&](const auto& event) { events.push_back(event); });
        normalizer.beginSession(24);
        require(classify(normalizer, 24, 900).decision ==
                    PhysicalTriggerDecision::OperationalStartupControl,
                "T18 control");
        require(!classify(normalizer, 24, 900).isFinalLogicalTrigger,
                "T18 control is never terminal");
        const auto mid = classify(normalizer, 24, 901);
        require(!mid.roundComplete && !mid.isFinalLogicalTrigger, "T18 mid not final");
        require(!classify(normalizer, 24, 901).isFinalLogicalTrigger,
                "T18 cached mid stays non-final");
        classify(normalizer, 24, 902);
        const auto finalFirst = classify(normalizer, 24, 903);
        require(finalFirst.roundComplete && finalFirst.isFinalLogicalTrigger &&
                    finalFirst.logicalTriggerIndex == 2,
                "T18 first final classification");
        const auto finalCached = classify(normalizer, 24, 903);
        require(!finalCached.roundComplete && finalCached.isFinalLogicalTrigger &&
                    !finalCached.newDistinct &&
                    finalCached.logicalTriggerIndex == finalFirst.logicalTriggerIndex &&
                    finalCached.roundGeneration == finalFirst.roundGeneration,
                "T18 cached final keeps the stable terminal property");
        // A late duplicate classified after the count boundary still reports
        // the stable property for the closed round identity.
        const auto late = classify(normalizer, 24, 903);
        require(!late.roundComplete && late.isFinalLogicalTrigger &&
                    late.roundGeneration == finalFirst.roundGeneration,
                "T18 late card after boundary keeps terminal property");
        const auto snapshot = normalizer.snapshot();
        require(snapshot.countBoundaryResets == 1 &&
                    std::count_if(events.begin(), events.end(), [](const auto& e) {
                        return e.kind == PhysicalRoundEvent::Kind::CountBoundary;
                    }) == 1,
                "T18 CountBoundary observer stays one-shot");
        const auto control = classify(normalizer, 24, 904);
        require(control.decision == PhysicalTriggerDecision::OperationalStartupControl &&
                    !control.isFinalLogicalTrigger && !control.roundComplete,
                "T18 next control is not terminal");
    }

    // A1: the default policy remains one startup filter followed by a fixed
    // logical CountBoundary.
    {
        PhysicalRoundNormalizer normalizer(2);
        normalizer.beginSession(100);
        require(classify(normalizer, 100, 1).decision ==
                    PhysicalTriggerDecision::OperationalStartupControl,
                "A1 default startup filter");
        require(classify(normalizer, 100, 2).logicalTriggerIndex == 0,
                "A1 logical zero");
        require(classify(normalizer, 100, 3).roundComplete,
                "A1 default CountBoundary");
        const auto snapshot = normalizer.snapshot();
        require(snapshot.startupFilterTriggerCount == 1 &&
                    !snapshot.disableCountBoundary &&
                    snapshot.lastCompletedPhysicalDistinctCount == 3 &&
                    snapshot.lastCompletedStartupFilteredCount == 1,
                "A1 default snapshot");
    }

    // A2: X=0 admits the first distinct identity as logical #0.
    {
        PhysicalRoundNormalizer normalizer(2);
        normalizer.setStartupFilterTriggerCount(0);
        normalizer.beginSession(101);
        const auto first = classify(normalizer, 101, 10);
        require(first.decision == PhysicalTriggerDecision::LogicalScan &&
                    first.logicalTriggerIndex == 0,
                "A2 first trigger is logical");
        require(normalizer.snapshot().currentStartupFilteredCount == 0,
                "A2 no startup filter");
        require(classify(normalizer, 101, 11).roundComplete,
                "A2 CountBoundary");
    }

    // A3: X=N counts new physical identities, not packets or cards.
    {
        PhysicalRoundNormalizer normalizer(2);
        normalizer.setStartupFilterTriggerCount(7);
        normalizer.beginSession(102);
        for (std::uint16_t trigger = 1; trigger <= 7; ++trigger)
            require(classify(normalizer, 102, trigger).decision ==
                        PhysicalTriggerDecision::OperationalStartupControl,
                    "A3 startup trigger filtered");
        const auto firstLogical = classify(normalizer, 102, 8);
        require(firstLogical.decision == PhysicalTriggerDecision::LogicalScan &&
                    firstLogical.logicalTriggerIndex == 0,
                "A3 eighth trigger is logical zero");
        const auto snapshot = normalizer.snapshot();
        require(snapshot.currentPhysicalDistinctCount == 8 &&
                    snapshot.currentStartupFilteredCount == 7,
                "A3 per-round counts");
    }

    // A4: duplicate multicard classifications share one physical/filter slot.
    {
        PhysicalRoundNormalizer normalizer(2);
        normalizer.setStartupFilterTriggerCount(2);
        normalizer.beginSession(103);
        const auto first = classify(normalizer, 103, 20);
        const auto duplicate = classify(normalizer, 103, 20);
        const auto second = classify(normalizer, 103, 21);
        const auto logical = classify(normalizer, 103, 22);
        require(first.newDistinct && !duplicate.newDistinct &&
                    duplicate.decision == first.decision &&
                    second.decision == PhysicalTriggerDecision::OperationalStartupControl &&
                    logical.logicalTriggerIndex == 0,
                "A4 shared classification");
        const auto snapshot = normalizer.snapshot();
        require(snapshot.currentPhysicalDistinctCount == 3 &&
                    snapshot.currentStartupFilteredCount == 2 &&
                    snapshot.operationalControlFiltered == 2 &&
                    snapshot.logicalDistinctAccepted == 1,
                "A4 duplicate counters");
    }

    // A5: a late card from the completed round remains a cache hit after the
    // boundary and cannot consume the next round's startup slot.
    {
        PhysicalRoundNormalizer normalizer(1);
        normalizer.beginSession(104);
        classify(normalizer, 104, 30);
        const auto final = classify(normalizer, 104, 31);
        const auto late = classify(normalizer, 104, 31);
        require(final.roundComplete && !late.newDistinct && !late.roundComplete &&
                    late.logicalTriggerIndex == 0,
                "A5 late cached final");
        const auto beforeNext = normalizer.snapshot();
        require(beforeNext.currentPhysicalDistinctCount == 0 &&
                    beforeNext.lastCompletedPhysicalDistinctCount == 2,
                "A5 boundary latch");
        require(classify(normalizer, 104, 32).decision ==
                    PhysicalTriggerDecision::OperationalStartupControl,
                "A5 next round startup");
        require(normalizer.snapshot().currentPhysicalDistinctCount == 1 &&
                    normalizer.snapshot().currentStartupFilteredCount == 1,
                "A5 late card did not consume next slot");
    }

    // A6: X=0 still uses the normal fixed-count boundary.
    {
        PhysicalRoundNormalizer normalizer(2);
        normalizer.setStartupFilterTriggerCount(0);
        normalizer.beginSession(105);
        require(classify(normalizer, 105, 40).logicalTriggerIndex == 0,
                "A6 logical zero");
        const auto final = classify(normalizer, 105, 41);
        const auto snapshot = normalizer.snapshot();
        require(final.roundComplete && final.isFinalLogicalTrigger &&
                    snapshot.countBoundaryResets == 1 && snapshot.roundGeneration == 1,
                "A6 CountBoundary enabled");
    }

    // A7: X=7 reaches CountBoundary only after the configured logical count,
    // then starts the next round with seven fresh filter slots.
    {
        PhysicalRoundNormalizer normalizer(2);
        normalizer.setStartupFilterTriggerCount(7);
        normalizer.beginSession(106);
        for (std::uint16_t trigger = 50; trigger < 57; ++trigger)
            classify(normalizer, 106, trigger);
        classify(normalizer, 106, 57);
        const auto final = classify(normalizer, 106, 58);
        require(final.roundComplete && normalizer.snapshot().roundGeneration == 1,
                "A7 CountBoundary after X=7");
        for (std::uint16_t trigger = 60; trigger < 67; ++trigger)
            require(classify(normalizer, 106, trigger).decision ==
                        PhysicalTriggerDecision::OperationalStartupControl,
                    "A7 next round refilters");
    }

    // A8: disabling fixed-count boundaries keeps the logical index and
    // RoundIdentity in the same round after the configured count.
    {
        PhysicalRoundNormalizer normalizer(4);
        normalizer.setStartupFilterTriggerCount(0);
        normalizer.setDisableCountBoundary(true);
        normalizer.beginSession(107);
        for (std::uint16_t trigger = 70; trigger < 76; ++trigger) {
            const auto classification = classify(normalizer, 107, trigger);
            require(classification.decision == PhysicalTriggerDecision::LogicalScan &&
                        classification.logicalTriggerIndex == trigger - 70 &&
                        !classification.roundComplete &&
                        !classification.isFinalLogicalTrigger,
                    "A8 disabled CountBoundary");
        }
        const auto snapshot = normalizer.snapshot();
        require(snapshot.currentLogicalDistinctCount == 6 &&
                    snapshot.currentPhysicalDistinctCount == 6 &&
                    snapshot.countBoundaryResets == 0 && snapshot.roundGeneration == 0,
                "A8 disabled counters");
    }

    // A9: timeout remains effective in timeout-only mode and resets the
    // over-counted logical window.
    {
        PhysicalRoundNormalizer normalizer(2);
        normalizer.setStartupFilterTriggerCount(0);
        normalizer.setDisableCountBoundary(true);
        normalizer.setTimeoutResetSec(1.0e-6);
        normalizer.beginSession(108);
        classify(normalizer, 108, 80, 1000);
        classify(normalizer, 108, 81, 1500);
        classify(normalizer, 108, 82, 2000);
        normalizer.timeoutBoundary(108, 5000);
        const auto snapshot = normalizer.snapshot();
        require(snapshot.timeoutBoundaryResets == 1 && snapshot.roundGeneration == 1 &&
                    snapshot.currentPhysicalDistinctCount == 0 &&
                    snapshot.currentLogicalDistinctCount == 0 &&
                    snapshot.lastCompletedPhysicalDistinctCount == 3 &&
                    snapshot.lastCompletedStartupFilteredCount == 0,
                "A9 timeout-only reset");
        require(classify(normalizer, 108, 83, 6000).logicalTriggerIndex == 0,
                "A9 next timeout-only round");
    }

    // A10: a partial startup-filter round is active and can be sealed by an
    // explicit timeout, preserving the observed partial counts.
    {
        PhysicalRoundNormalizer normalizer(3);
        normalizer.setStartupFilterTriggerCount(7);
        normalizer.setTimeoutResetSec(1.0e-6);
        normalizer.beginSession(109);
        classify(normalizer, 109, 90, 1000);
        classify(normalizer, 109, 91, 1500);
        classify(normalizer, 109, 92, 1800);
        normalizer.timeoutBoundary(109, 4000);
        const auto snapshot = normalizer.snapshot();
        require(snapshot.lastCompletedPhysicalDistinctCount == 3 &&
                    snapshot.lastCompletedStartupFilteredCount == 3 &&
                    snapshot.currentPhysicalDistinctCount == 0 &&
                    snapshot.currentStartupFilteredCount == 0 &&
                    snapshot.roundGeneration == 1,
                "A10 partial startup latch");
        require(classify(normalizer, 109, 93, 5000).decision ==
                    PhysicalTriggerDecision::OperationalStartupControl,
                "A10 next startup slot");
    }

    // A11: current/last-completed per-round counters remain distinct from
    // cumulative counters across a boundary.
    {
        PhysicalRoundNormalizer normalizer(2);
        normalizer.beginSession(110);
        classify(normalizer, 110, 100);
        classify(normalizer, 110, 101);
        classify(normalizer, 110, 102);
        auto snapshot = normalizer.snapshot();
        require(snapshot.physicalDistinctObserved == 3 &&
                    snapshot.logicalDistinctAccepted == 2 &&
                    snapshot.currentPhysicalDistinctCount == 0 &&
                    snapshot.lastCompletedPhysicalDistinctCount == 3,
                "A11 completed versus cumulative");
        classify(normalizer, 110, 103);
        classify(normalizer, 110, 104);
        snapshot = normalizer.snapshot();
        require(snapshot.physicalDistinctObserved == 5 &&
                    snapshot.logicalDistinctAccepted == 3 &&
                    snapshot.currentPhysicalDistinctCount == 2 &&
                    snapshot.currentStartupFilteredCount == 1 &&
                    snapshot.lastCompletedPhysicalDistinctCount == 3,
                "A11 current versus last completed");
    }

    // A12: beginSession clears both per-round views and the old identity
    // cache, so a reused trigger sequence starts a fresh startup policy.
    {
        PhysicalRoundNormalizer normalizer(2);
        normalizer.setStartupFilterTriggerCount(2);
        normalizer.beginSession(111);
        classify(normalizer, 111, 120);
        classify(normalizer, 111, 121);
        normalizer.timeoutBoundary(111);
        require(normalizer.snapshot().lastCompletedPhysicalDistinctCount == 2,
                "A12 pre-session completed count");
        normalizer.beginSession(112);
        const auto snapshot = normalizer.snapshot();
        require(snapshot.physicalDistinctObserved == 0 &&
                    snapshot.currentPhysicalDistinctCount == 0 &&
                    snapshot.currentStartupFilteredCount == 0 &&
                    snapshot.lastCompletedPhysicalDistinctCount == 0 &&
                    snapshot.lastCompletedStartupFilteredCount == 0,
                "A12 session counter reset");
        const auto first = classify(normalizer, 112, 120);
        require(first.newDistinct &&
                    first.decision == PhysicalTriggerDecision::OperationalStartupControl,
                "A12 session cache reset");
    }

    std::cout << "PASS physical round normalizer contract (T1-T18, A1-A12)\n";
}
