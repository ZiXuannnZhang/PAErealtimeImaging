#include "RingBlockAssembler.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

struct CapturedBlock {
    std::vector<float> raw;
    std::vector<float> angles;
    std::vector<uint8_t> channels;
    int blockSeq = -1;
    paimage::RoundIdentity round;
};

struct Runner {
    bool ok = true;
    std::string test;

    void check(bool condition, const std::string &message)
    {
        if (!condition) {
            ok = false;
            std::cerr << "FAIL " << test << ": " << message << '\n';
        }
    }

    template <typename T>
    void equal(const std::vector<T> &actual,
               const std::vector<T> &expected,
               const std::string &message)
    {
        check(actual == expected, message);
    }

    void rawBytesEqual(const std::vector<float> &actual,
                       const std::vector<float> &expected,
                       const std::string &message)
    {
        const bool sameSize = actual.size() == expected.size();
        const bool sameBytes = sameSize &&
            (actual.empty() || std::memcmp(actual.data(), expected.data(),
                                           actual.size() * sizeof(float)) == 0);
        check(sameBytes, message);
    }
};

struct Fixture {
    RingBlockAssembler assembler;
    std::vector<CapturedBlock> blocks;
    int progress = 0;
    int timeouts = 0;

    void configure(std::initializer_list<int> enabled,
                   int perChannelBlock,
                   int sampDepth,
                   double sectorStartDeg = 0.0,
                   double sectorWidthDeg = 10.0,
                   double stepDeg = 1.0,
                   int perChannelFrame = 32,
                   int triggerWlOdd = 1,
                   double timeoutResetSec = 0.0)
    {
        int channels[8] = {0};
        int index = 0;
        for (int value : enabled) {
            if (index >= 8) break;
            channels[index++] = value;
        }
        assembler.setBlockCallback([this](std::vector<float> &&raw,
                                          std::vector<float> &&angles,
                                          std::vector<uint8_t> &&channelIds,
                                          int blockSeq,
                                          const paimage::RoundIdentity &round, bool roundComplete) {
            blocks.push_back({std::move(raw), std::move(angles),
                              std::move(channelIds), blockSeq, round});
        });
        assembler.setProgressCallback([this] { ++progress; });
        assembler.setTimeoutCallback([this] { ++timeouts; });
        assembler.configure(channels, perChannelBlock, sampDepth,
                            sectorStartDeg, sectorWidthDeg, stepDeg,
                            perChannelFrame, triggerWlOdd, timeoutResetSec);
    }

    void line(int channel, uint16_t trigger, std::initializer_list<float> samples)
    {
        const std::vector<float> values(samples);
        assembler.pushChannelLine(channel, trigger, values.data(),
                                  static_cast<int>(values.size()));
    }

    void lineForRound(int channel, uint16_t trigger,
                      const paimage::RoundIdentity &round,
                      std::initializer_list<float> samples)
    {
        const std::vector<float> values(samples);
        assembler.pushChannelLine(channel, trigger, round, values.data(),
                                  static_cast<int>(values.size()));
    }
};

bool testNormalFullLength(Runner &r)
{
    Fixture f;
    f.configure({0, 1, 0, 1}, 1, 2, 10.0, 20.0, 1.0, 4);
    f.line(3, 7, {30.0f, 31.0f});
    f.line(1, 7, {10.0f, 11.0f});

    r.check(f.blocks.size() == 1, "one full-length block expected");
    if (f.blocks.size() != 1) return false;
    const CapturedBlock &b = f.blocks.front();
    r.rawBytesEqual(b.raw, {10.0f, 11.0f, 30.0f, 31.0f},
                    "normal raw layout changed");
    r.equal(b.angles, {10.0f, 30.0f}, "normal angles changed");
    r.equal(b.channels, {uint8_t(1), uint8_t(3)}, "selected channels changed");
    r.check(b.blockSeq == 0, "first block sequence must be zero");
    r.check(f.progress == 1, "one trigger completion expected");
    return true;
}

bool testShortLineZeroPadding(Runner &r)
{
    Fixture f;
    f.configure({1, 0, 1, 0}, 2, 4);
    f.line(0, 1, {1.0f, 2.0f});
    f.line(2, 1, {3.0f});
    f.line(0, 2, {4.0f});
    f.line(2, 2, {5.0f, 6.0f, 7.0f});

    r.check(f.blocks.size() == 1, "short lines must still complete a block");
    if (f.blocks.size() != 1) return false;
    r.rawBytesEqual(f.blocks.front().raw,
                    {1.0f, 2.0f, 0.0f, 0.0f,
                     3.0f, 0.0f, 0.0f, 0.0f,
                     4.0f, 0.0f, 0.0f, 0.0f,
                     5.0f, 6.0f, 7.0f, 0.0f},
                    "short-line zero padding is not deterministic");
    r.check(f.progress == 2, "short lines must count as completed triggers");
    return true;
}

bool testLongLineTruncation(Runner &r)
{
    Fixture f;
    f.configure({1}, 1, 3);
    f.line(0, 4, {9.0f, 8.0f, 7.0f, 6.0f, 5.0f});

    r.check(f.blocks.size() == 1, "long line must complete one block");
    if (f.blocks.size() != 1) return false;
    r.rawBytesEqual(f.blocks.front().raw, {9.0f, 8.0f, 7.0f},
                    "long line was not truncated at sampDepth");
    return true;
}

bool testDuplicateFirstWins(Runner &r)
{
    Fixture f;
    f.configure({1, 1}, 1, 2);
    f.line(0, 9, {1.0f, 2.0f});
    f.line(0, 9, {9.0f, 9.0f});
    f.line(1, 9, {3.0f, 4.0f});

    r.check(f.blocks.size() == 1, "duplicate test must complete one block");
    if (f.blocks.size() != 1) return false;
    r.rawBytesEqual(f.blocks.front().raw, {1.0f, 2.0f, 3.0f, 4.0f},
                    "duplicate channel overwrote first valid line");
    r.check(f.progress == 1, "duplicate channel counted twice");
    return true;
}

bool testOutOfOrderTriggers(Runner &r)
{
    Fixture f;
    f.configure({1, 1}, 2, 1);
    f.line(0, 10, {10.0f});
    f.line(0, 11, {11.0f});
    f.line(1, 11, {111.0f});
    f.line(1, 10, {110.0f});

    r.check(f.blocks.size() == 1, "out-of-order triggers should fill one block");
    if (f.blocks.size() != 1) return false;
    r.rawBytesEqual(f.blocks.front().raw, {11.0f, 111.0f, 10.0f, 110.0f},
                    "completion order was unexpectedly reordered");
    r.check(f.progress == 2, "out-of-order triggers completed more than once");
    return true;
}

bool testTemporalOverflow(Runner &r)
{
    Fixture f;
    f.configure({1, 1}, 1, 1);
    for (uint16_t trigger = 10; trigger <= 42; ++trigger)
        f.line(0, trigger, {static_cast<float>(trigger)});

    // 10 is the first-seen trigger and must be gone; 11 is retained.
    f.line(1, 11, {1001.0f});
    f.line(1, 10, {1000.0f});
    r.check(f.blocks.size() == 1, "pending overflow did not retain the first 32 triggers");
    if (f.blocks.size() != 1) return false;
    r.rawBytesEqual(f.blocks.front().raw, {11.0f, 1001.0f},
                    "overflow did not evict the first-seen trigger");
    return true;
}

bool testNumericallySmallCurrentTrigger(Runner &r)
{
    Fixture f;
    f.configure({1, 1}, 1, 1);
    for (uint16_t trigger = 100; trigger < 132; ++trigger)
        f.line(0, trigger, {static_cast<float>(trigger)});
    f.line(0, 0, {0.0f});

    f.line(1, 0, {1000.0f});
    f.line(1, 100, {1100.0f});
    r.check(f.blocks.size() == 1, "numerically smallest current trigger was evicted");
    if (f.blocks.size() != 1) return false;
    r.rawBytesEqual(f.blocks.front().raw, {0.0f, 1000.0f},
                    "current trigger did not complete after overflow");
    return true;
}

bool testSequenceWrapOverflow(Runner &r)
{
    Fixture f;
    f.configure({1, 1}, 1, 1);
    for (uint32_t trigger = 65530; trigger <= 65535; ++trigger)
        f.line(0, static_cast<uint16_t>(trigger), {static_cast<float>(trigger)});
    for (uint16_t trigger = 0; trigger <= 26; ++trigger)
        f.line(0, trigger, {static_cast<float>(trigger)});

    f.line(1, 0, {100.0f});
    f.line(1, 65530, {200.0f});
    r.check(f.blocks.size() == 1, "wrap overflow evicted a new trigger");
    if (f.blocks.size() != 1) return false;
    r.rawBytesEqual(f.blocks.front().raw, {0.0f, 100.0f},
                    "trigger zero was incorrectly treated as oldest by numeric order");
    return true;
}

bool testTimeoutReset(Runner &r)
{
    Fixture f;
    f.configure({1, 1}, 1, 1, 0.0, 10.0, 1.0, 32, 1, 0.02);
    f.line(0, 5, {5.0f});
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    f.line(0, 6, {6.0f});
    f.line(1, 6, {60.0f});

    r.check(f.timeouts == 1, "timeout callback was not invoked once");
    r.check(f.blocks.size() == 1, "post-timeout trigger did not complete");
    if (f.blocks.size() != 1) return false;
    r.rawBytesEqual(f.blocks.front().raw, {6.0f, 60.0f},
                    "post-timeout block contains pre-reset data");
    r.check(f.blocks.front().blockSeq == 0, "timeout did not reset block sequence");
    f.line(1, 5, {50.0f});
    r.check(f.blocks.size() == 1, "pre-timeout pending trigger survived reset");
    return true;
}

bool testExternalTimeoutOwnership(Runner &r)
{
    Fixture f;
    f.configure({1, 1}, 2, 1, 0.0, 10.0, 1.0, 32, 1, 0.02);
    f.assembler.setTimeoutManagedExternally(true);
    f.line(0, 5, {5.0f});
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    f.line(0, 6, {6.0f});
    f.line(1, 6, {60.0f});

    r.check(f.timeouts == 0,
            "externally managed timeout must not fire the assembler boundary");
    r.check(f.blocks.empty(),
            "externally managed timeout must retain the partial block until the owner resets it");
    f.assembler.resetAfterPhysicalTimeout();
    r.check(f.timeouts == 1 && f.blocks.empty(),
            "explicit owner reset must remain available");
    return true;
}

bool testLogicalRoundBoundary(Runner &r)
{
    Fixture f;
    f.configure({1}, 2, 1, 0.0, 10.0, 1.0, 4, 1, 0.0);
    f.line(0, 20, {20.0f});
    f.line(0, 21, {21.0f});

    r.check(f.blocks.size() == 1, "pre-boundary block must complete");
    if (f.blocks.size() != 1) return false;
    r.check(f.assembler.completeLogicalRound(),
            "logical round boundary must reset angle phase");

    f.line(0, 30, {30.0f});
    f.line(0, 31, {31.0f});
    r.check(f.blocks.size() == 2, "post-boundary block must complete");
    if (f.blocks.size() != 2) return false;
    r.check(f.blocks[1].blockSeq == 1,
            "logical round boundary must preserve block sequence monotonicity");
    r.equal(f.blocks[1].angles, {0.0f, 1.0f},
            "logical round boundary must restart wavelength angle phase");
    return true;
}

bool testIdentityResidualHardBarrier(Runner &r)
{
    Fixture f;
    f.configure({1, 1}, 4, 1);
    const paimage::RoundIdentity oldRound{77, 4};
    const paimage::RoundIdentity newRound{77, 5};

    // The old round has only three complete Ring triggers.  The physical
    // normalizer may already have emitted CountBoundary, but the observer
    // must not discard this in-flight Ring residual.
    for (uint16_t trigger = 10; trigger <= 12; ++trigger) {
        f.lineForRound(0, trigger, oldRound, {static_cast<float>(trigger)});
        f.lineForRound(1, trigger, oldRound,
                       {static_cast<float>(100 + trigger)});
    }
    const auto beforeBoundary = f.assembler.snapshot();
    r.check(beforeBoundary.blockTriggers == 3 &&
                !f.assembler.completeLogicalRound() &&
                f.assembler.snapshot().blockTriggers == 3,
            "R1 CountBoundary observer must retain old residual");

    // The first new identity must clear the old residual before any NEW line
    // can complete a block.  No old line is fabricated or padded into it.
    for (uint16_t trigger = 20; trigger <= 23; ++trigger) {
        f.lineForRound(0, trigger, newRound, {static_cast<float>(trigger)});
        f.lineForRound(1, trigger, newRound,
                       {static_cast<float>(100 + trigger)});
    }

    r.check(f.blocks.size() == 1,
            "R1 residual transition must emit only the new-round block");
    if (f.blocks.size() != 1) return false;
    r.rawBytesEqual(f.blocks.front().raw,
                    {20.0f, 120.0f, 21.0f, 121.0f,
                     22.0f, 122.0f, 23.0f, 123.0f},
                    "R1 old residual leaked into new block");
    r.check(f.blocks.front().round == newRound,
            "R1 block identity is not the incoming round");
    const auto state = f.assembler.snapshot();
    r.check(state.residualTransitions == 1 &&
                state.residualPendingTriggers == 0 &&
                state.residualBlockTriggers == 3,
            "R1 residual diagnostic counters are incomplete");
    return true;
}

bool testCountObserverDoesNotDropOldFinalSync(Runner &r)
{
    Fixture f;
    f.configure({1, 1}, 1, 1);
    const paimage::RoundIdentity oldRound{88, 0};
    const paimage::RoundIdentity newRound{88, 1};

    // One old block is already complete, so the assembler is clean when the
    // normalizer's CountBoundary observer runs before the final old sync.
    f.lineForRound(0, 20, oldRound, {20.0f});
    f.lineForRound(1, 20, oldRound, {120.0f});
    r.check(f.assembler.snapshot().blockTriggers == 0 &&
                f.assembler.completeLogicalRound(),
            "R2 clean CountBoundary observer must reset only phase");

    // The delayed final old sync remains valid and must form its own old
    // block after the observer has run.
    f.lineForRound(0, 21, oldRound, {21.0f});
    f.lineForRound(1, 21, oldRound, {121.0f});
    r.check(f.blocks.size() == 2 && f.blocks.back().round == oldRound,
            "R2 old final sync was dropped by CountBoundary observer");

    f.lineForRound(0, 30, newRound, {30.0f});
    f.lineForRound(1, 30, newRound, {130.0f});
    r.check(f.blocks.size() == 3 && f.blocks.back().round == newRound,
            "R2 new identity did not start a separate block");
    r.equal(f.blocks.back().angles, {0.0f, 10.0f},
            "R2 new identity did not reset angle phase");
    r.check(f.assembler.snapshot().residualTransitions == 0,
            "R2 clean CountBoundary path recorded a false residual");
    return true;
}

bool testStaleRoundDrop(Runner &r)
{
    Fixture f;
    f.configure({1, 1}, 1, 1);
    const paimage::RoundIdentity oldRound{99, 8};
    const paimage::RoundIdentity newRound{99, 9};
    f.lineForRound(0, 40, newRound, {40.0f});
    f.lineForRound(1, 40, newRound, {140.0f});
    const std::size_t before = f.blocks.size();
    f.lineForRound(0, 41, oldRound, {41.0f});
    f.lineForRound(1, 41, oldRound, {141.0f});
    r.check(f.blocks.size() == before,
            "R3 stale old round entered a block");
    r.check(f.assembler.snapshot().staleRoundDrops >= 2,
            "R3 stale old round was not counted");
    return true;
}

bool testGenerationGapAndSessionFloor(Runner &r)
{
    Fixture f;
    f.configure({1}, 1, 1);
    f.assembler.beginMeasurementSession(500);
    const paimage::RoundIdentity first{500, 1};
    const paimage::RoundIdentity gap{500, 4};
    const paimage::RoundIdentity oldSession{499, 99};
    f.lineForRound(0, 50, oldSession, {50.0f});
    f.lineForRound(0, 51, first, {51.0f});
    f.lineForRound(0, 52, gap, {52.0f});
    r.check(f.blocks.size() == 2,
            "R4 valid incoming identities did not continue after session floor");
    r.check(f.blocks[0].round == first && f.blocks[1].round == gap,
            "R4 block identity changed across generation gap");
    r.check(f.assembler.snapshot().generationGapTransitions == 1,
            "R4 generation gap was not diagnosed");
    return true;
}

bool testMeasurementSessionRestart(Runner &r)
{
    Fixture f;
    f.configure({1}, 1, 1);
    const paimage::RoundIdentity sessionA{600, 0};
    const paimage::RoundIdentity sessionB{601, 0};

    f.lineForRound(0, 60, sessionA, {60.0f});
    f.assembler.beginMeasurementSession(sessionB.measurementSession);
    f.lineForRound(0, 61, sessionB, {61.0f});
    f.lineForRound(0, 62, sessionA, {62.0f});

    r.check(f.blocks.size() == 2 &&
                f.blocks[0].round == sessionA &&
                f.blocks[1].round == sessionB,
            "R10 session restart mixed or lost block identity");
    r.check(f.assembler.snapshot().staleRoundDrops == 1,
            "R10 old session was not rejected after restart");
    return true;
}

bool testNormalGoldenRegression(Runner &r)
{
    Fixture f;
    f.configure({1, 0, 1, 0, 0, 1}, 2, 2,
                 0.0, 90.0, 0.5, 4, 1, 0.0);
    f.line(0, 50, {10.0f, 11.0f});
    f.line(2, 50, {20.0f, 21.0f});
    f.line(5, 50, {30.0f, 31.0f});
    f.line(0, 51, {40.0f, 41.0f});
    f.line(2, 51, {50.0f, 51.0f});
    f.line(5, 51, {60.0f, 61.0f});
    f.line(0, 52, {70.0f, 71.0f});
    f.line(2, 52, {80.0f, 81.0f});
    f.line(5, 52, {90.0f, 91.0f});
    f.line(0, 53, {100.0f, 101.0f});
    f.line(2, 53, {110.0f, 111.0f});
    f.line(5, 53, {120.0f, 121.0f});

    r.check(f.blocks.size() == 2, "golden case must produce two blocks");
    if (f.blocks.size() != 2) return false;
    const std::vector<uint8_t> expectedChannels = {
        uint8_t(0), uint8_t(2), uint8_t(5),
        uint8_t(0), uint8_t(2), uint8_t(5)};
    const std::vector<float> expectedAngles[2] = {
        {0.0f, 90.0f, 180.0f, 0.5f, 90.5f, 180.5f},
        {0.5f, 90.5f, 180.5f, 1.0f, 91.0f, 181.0f},
    };
    for (size_t i = 0; i < f.blocks.size(); ++i) {
        r.check(f.blocks[i].blockSeq == static_cast<int>(i),
                "golden block sequence changed");
        r.equal(f.blocks[i].channels, expectedChannels,
                "golden channel vector changed");
        r.equal(f.blocks[i].angles, expectedAngles[i],
                "golden angle offset changed");
    }
    r.rawBytesEqual(f.blocks[0].raw,
                    {10.0f, 11.0f, 20.0f, 21.0f, 30.0f, 31.0f,
                     40.0f, 41.0f, 50.0f, 51.0f, 60.0f, 61.0f},
                    "golden first block raw bytes changed");
    r.rawBytesEqual(f.blocks[1].raw,
                    {70.0f, 71.0f, 80.0f, 81.0f, 90.0f, 91.0f,
                     100.0f, 101.0f, 110.0f, 111.0f, 120.0f, 121.0f},
                    "golden second block raw bytes changed");
    r.check(f.progress == 4, "golden progress count changed");
    return true;
}

} // namespace

int main()
{
    Runner runner;
    const std::vector<std::pair<const char *, std::function<bool(Runner &)>>> tests = {
        {"T1 normal full-length", testNormalFullLength},
        {"T2 short line zero-padding", testShortLineZeroPadding},
        {"T3 long line truncation", testLongLineTruncation},
        {"T4 duplicate first-wins", testDuplicateFirstWins},
        {"T5 out-of-order triggers", testOutOfOrderTriggers},
        {"T6 temporal overflow", testTemporalOverflow},
        {"T7 numerically smallest current", testNumericallySmallCurrentTrigger},
        {"T8 sequence wrap overflow", testSequenceWrapOverflow},
        {"T9 timeout reset", testTimeoutReset},
        {"T10 external timeout ownership", testExternalTimeoutOwnership},
        {"T11 normal golden regression", testNormalGoldenRegression},
        {"T12 logical round boundary", testLogicalRoundBoundary},
        {"R1 identity residual hard barrier", testIdentityResidualHardBarrier},
        {"R2 CountBoundary final sync retention", testCountObserverDoesNotDropOldFinalSync},
        {"R3 stale round drop", testStaleRoundDrop},
        {"R4 generation gap/session floor", testGenerationGapAndSessionFloor},
        {"R10 measurement session restart", testMeasurementSessionRestart},
    };

    for (const auto &test : tests) {
        runner.test = test.first;
        const bool testOk = test.second(runner);
        if (testOk && runner.ok)
            std::cout << "PASS " << test.first << '\n';
    }
    if (!runner.ok) return 1;
    std::cout << "PASS all RingBlockAssembler tests (T1-T12,R1-R4,R10)\n";
    return 0;
}
