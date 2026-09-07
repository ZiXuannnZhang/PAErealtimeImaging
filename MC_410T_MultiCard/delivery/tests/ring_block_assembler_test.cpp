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
                                          int blockSeq) {
            blocks.push_back({std::move(raw), std::move(angles),
                              std::move(channelIds), blockSeq});
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
        {"T10 normal golden regression", testNormalGoldenRegression},
    };

    for (const auto &test : tests) {
        runner.test = test.first;
        const bool testOk = test.second(runner);
        if (testOk && runner.ok)
            std::cout << "PASS " << test.first << '\n';
    }
    if (!runner.ok) return 1;
    std::cout << "PASS all RingBlockAssembler tests (T1-T10)\n";
    return 0;
}
