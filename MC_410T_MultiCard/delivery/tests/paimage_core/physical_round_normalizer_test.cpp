#include "PaimageAcquisition/PhysicalRoundNormalizer.h"

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
                                     std::uint16_t trigger) {
    return normalizer.classify(session, trigger);
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

    std::cout << "PASS physical round normalizer contract (T1-T12)\n";
}
