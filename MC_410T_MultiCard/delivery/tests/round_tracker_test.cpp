#include "RoundTracker.h"

#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool value, const char *message)
{
    if (!value) throw std::runtime_error(message);
}

RoundTracker::Observation observe(RoundTracker &tracker, std::uint16_t wire,
                                  std::uint64_t time = 1)
{
    FrameIdentity identity;
    identity.wireTrigger = wire;
    return tracker.observe(identity, time);
}

void testHolesAndReorder()
{
    RoundTracker tracker;
    const auto zero = observe(tracker, 0);
    const auto two = observe(tracker, 2);
    const auto one = observe(tracker, 1);
    require(zero.accepted && zero.relativePosition == 0, "first visible position");
    require(two.accepted && two.relativePosition == 2, "forward gap must remain a gap");
    require(one.accepted && one.late && one.relativePosition == 1,
            "small backward step must be reordered, not compressed");
}

void testWrapAndAmbiguousReset()
{
    RoundTracker tracker;
    require(observe(tracker, 65534).relativePosition == 0, "wrap anchor");
    require(observe(tracker, 65535).relativePosition == 1, "wrap tail");
    require(observe(tracker, 0).relativePosition == 2, "wrap zero");
    require(observe(tracker, 1).relativePosition == 3, "wrap one");

    const auto before = tracker.snapshot().roundId;
    const auto ambiguous = observe(tracker, 32769);
    require(ambiguous.newRound && ambiguous.unknownEpoch,
            "half range must start an unknown epoch");
    require(ambiguous.roundId > before, "unknown epoch must advance round id");
    require(ambiguous.positionConfidence == PositionConfidence::Unknown,
            "unknown epoch confidence");
}

void testTimeoutAndPublication()
{
    RoundTracker::Config config;
    config.timeoutResetSec = 2.0;
    config.metadataCapacity = 1;
    RoundTracker tracker(config);
    RoundTracker::Metadata metadata;
    metadata.identity.wireTrigger = 4;
    metadata.observedMonotonicNs = 10;
    require(tracker.tryPublish(metadata), "metadata publish");
    require(!tracker.tryPublish(metadata), "full metadata queue must not wait");
    std::vector<RoundTracker::Metadata> drained;
    require(tracker.drain(drained) == 1 && drained.size() == 1, "metadata drain count");
    const auto first = tracker.observePublished(drained.front());
    require(first.accepted && first.metadataDropped &&
            first.positionConfidence == PositionConfidence::Unknown,
            "metadata loss must lower position confidence");
    const auto timeout = observe(tracker, 5, 3'000'000'011ULL);
    require(timeout.newRound && timeout.round.closeReason == RoundCloseReason::IdleTimeout,
            "idle timeout round boundary");
    tracker.confirmWithOracle(1000);
    require(tracker.snapshot().positionConfidence == PositionConfidence::Confirmed,
            "oracle confirmation");
}
}

int main()
{
    try {
        testHolesAndReorder();
        testWrapAndAmbiguousReset();
        testTimeoutAndPublication();
        std::cout << "PASS RoundTracker: holes, reorder, wrap, unknown epoch, timeout, bounded metadata\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL RoundTracker: " << error.what() << '\n';
        return 1;
    }
}
