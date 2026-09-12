#include "RingBlockAssembler.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool value, const char *message)
{
    if (!value) throw std::runtime_error(message);
}

FrameQuality validQuality()
{
    FrameQuality quality;
    quality.qualityUnknown = false;
    quality.assemblyComplete = true;
    quality.packetCoverageComplete = true;
    quality.packetLengthValid = true;
    quality.sampleLengthValid = true;
    return quality;
}

void push(RingBlockAssembler &assembler, int channel, std::uint16_t trigger,
          float value, int wavelength = -1, bool valid = true)
{
    const float line[2] = {value, value + 0.5f};
    FrameIdentity identity;
    identity.wireTrigger = trigger;
    FrameQuality quality = valid ? validQuality() : FrameQuality{};
    assembler.pushChannelLine(channel, trigger, line, 2, identity, quality, wavelength);
}

void testPositionOwnedGapAndExplicitWavelength()
{
    RingBlockAssembler assembler;
    int enabled[8] = {1, 1, 0, 0, 0, 0, 0, 0};
    std::vector<RingBlock> blocks;
    assembler.setIdentityContext(7, 3, 11, PositionConfidence::RelativeOnly);
    assembler.setRingBlockCallback([&](RingBlock &&block) { blocks.push_back(std::move(block)); });
    assembler.configure(enabled, 3, 2, 10.0, 20.0, 1.0, 8, 1, 0.0);
    // Position 0 is complete. Position 2 arrives before position 1; the
    // forced round close seals position 1 as invalid instead of compressing it.
    push(assembler, 1, 100, 100.0f, 1);
    push(assembler, 0, 100, 10.0f, 1);
    push(assembler, 0, 102, 12.0f, 0);
    push(assembler, 1, 102, 102.0f, 0);
    assembler.finishRound();
    require(blocks.size() == 1, "gap tail must be one block");
    const RingBlock &block = blocks.front();
    require(block.roundId == 3 && block.serviceGeneration == 7 && block.configVersion == 11,
            "block identity");
    require(block.startPosition == 0 && block.positionCount == 3,
            "gap positions must retain span");
    require((block.validPositionBits & 0x5u) == 0x5u,
            "only positions 0 and 2 should be valid");
    require(block.wavelengthAssumed == false, "explicit wavelength must be preserved");
    require(block.wavelengths[0] == 1 && block.wavelengths[4] == 0,
            "explicit wavelength vector");
    require(!block.quality.qualityUnknown && !block.quality.assemblyComplete &&
            !block.quality.packetCoverageComplete,
            "block quality must retain known gap without claiming complete");
    require(block.raw[0] == 10.0f && block.raw[8] == 12.0f,
            "position raw layout");
    require(std::fabs(block.anglesDeg[0] - 11.0f) < 1e-6f &&
            std::fabs(block.anglesDeg[4] - 11.0f) < 1e-6f,
            "angles use position and explicit wavelength");
}

void testTailAndNoCrossRoundPacking()
{
    RingBlockAssembler assembler;
    int enabled[8] = {1, 0, 0, 0, 0, 0, 0, 0};
    std::vector<RingBlock> blocks;
    assembler.setRingBlockCallback([&](RingBlock &&block) { blocks.push_back(std::move(block)); });
    assembler.configure(enabled, 50, 2, 0.0, 0.0, 1.0, 50, 1, 0.0);
    for (std::uint16_t trigger = 0; trigger < 51; ++trigger)
        push(assembler, 0, trigger, static_cast<float>(trigger), trigger % 2);
    require(blocks.size() == 1 && blocks[0].positionCount == 50,
            "first full block must contain exactly 50 positions");
    assembler.finishRound();
    require(blocks.size() == 2 && blocks[1].positionCount == 1,
            "tail of one must be emitted separately");
    require(blocks[1].startPosition == 50 && blocks[1].blockSeq == 1,
            "tail identity");
    push(assembler, 0, 1000, 1000.0f, 0);
    assembler.finishRound();
    require(blocks.size() == 3 && blocks[2].roundId == blocks[1].roundId + 1,
            "new round must not reuse prior tail");
    require(blocks[2].startPosition == 0 && blocks[2].blockSeq == 0,
            "new round starts at relative zero");
}

void testLegacyCallbackAndWrap()
{
    RingBlockAssembler assembler;
    int enabled[8] = {1, 1, 0, 0, 0, 0, 0, 0};
    std::vector<std::vector<float>> raw;
    assembler.setBlockCallback([&](std::vector<float> &&values,
                                   std::vector<float> &&,
                                   std::vector<std::uint8_t> &&,
                                   int) { raw.push_back(std::move(values)); });
    assembler.configure(enabled, 2, 1, 0.0, 10.0, 1.0, 8, 1, 0.0);
    const float a = 1.0f, b = 2.0f;
    assembler.pushChannelLine(0, 65534, &a, 1);
    assembler.pushChannelLine(1, 65534, &b, 1);
    assembler.pushChannelLine(0, 0, &a, 1);
    assembler.pushChannelLine(1, 0, &b, 1);
    assembler.pushChannelLine(0, 65535, &a, 1);
    assembler.pushChannelLine(1, 65535, &b, 1);
    require(raw.size() == 1, "legacy callback full block");
    require(raw[0].size() == 4 && raw[0][0] == 1.0f && raw[0][2] == 1.0f,
            "legacy fixed raw layout");
}
}

int main()
{
    try {
        testPositionOwnedGapAndExplicitWavelength();
        testTailAndNoCrossRoundPacking();
        testLegacyCallbackAndWrap();
        std::cout << "PASS RingBlockAssembler: position gaps, explicit wavelength, tails, wrap, legacy layout\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL RingBlockAssembler: " << error.what() << '\n';
        return 1;
    }
}
