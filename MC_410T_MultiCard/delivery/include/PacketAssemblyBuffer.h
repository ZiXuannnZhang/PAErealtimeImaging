#pragma once
#include <cstdint>
#include <cstring>
#include <chrono>
#include <vector>
#include <algorithm>
#include "Constants.h"
#include "DataTypes.h"
#include "AcqConfig.h"

// ============================================================
// PacketAssemblyBuffer  单卡单次触发的包重组缓冲区
//
// 支持 16bit/32bit 数据格式，采样间隔可配置
// 固定数组改为 vector（堆分配），避免栈溢出
// B/A 交织解包：
//   16bit 模式：每 4 字节 = B(2B) + A(2B)
//   32bit 模式：每 8 字节 = B(4B) + A(4B)
// ============================================================
class PacketAssemblyBuffer {
public:
    enum class InsertResult {
        Accepted,
        Duplicate,
        OffsetOutOfRange
    };

    PacketAssemblyBuffer(int maxPackets = MAX_PKTS_PER_TRIG)
        : m_maxPackets(maxPackets)
        , m_receivedMask32(static_cast<size_t>((maxPackets + 31) / 32), 0)
        , m_dataSizes(maxPackets, 0)
        , m_payloads(static_cast<size_t>(maxPackets) * UDP_PAYLOAD_BYTES, 0)
    {
        reset();
    }

    // 插入一个数据包
    // pkt.packetSeq 是 FPGA 的全局包计数器（大端序 uint16）
    // 内部减 m_basePacketSeq 得到触发内连续序号（0, 1, 2, ...）
    InsertResult insertPacket(const DataPacket& pkt) {
        // 第一包：记录触发序号、基序号、时间戳
        if (m_receivedCount == 0) {
            m_triggerSeq     = pkt.triggerSeq;
            m_basePacketSeq  = pkt.packetSeq;
            m_firstArrivalMs = currentTimeMs();
            m_firstIngressId = pkt.ingressId;
            m_firstReceiveMonotonicNs = pkt.firstReceiveMonotonicNs;
            m_configVersion = pkt.configVersion;
        }

        // 计算触发内偏移序号（全局计数器 - 触发首包基序号 = 包内顺序号）
        int seqOffset = static_cast<int>(static_cast<uint16_t>(
            pkt.packetSeq - m_basePacketSeq));
        if (seqOffset < 0 || seqOffset >= m_maxPackets)
            return InsertResult::OffsetOutOfRange;

        // 位图去重
        size_t slot = static_cast<size_t>(seqOffset) >> 5;
        uint32_t bit = 1u << (seqOffset & 31);
        if (slot >= m_receivedMask32.size()) return InsertResult::OffsetOutOfRange;
        if (m_receivedMask32[slot] & bit) return InsertResult::Duplicate;  // 重复包
        m_receivedMask32[slot] |= bit;

        uint16_t sz = (pkt.dataSize > UDP_PAYLOAD_BYTES) ? UDP_PAYLOAD_BYTES : pkt.dataSize;
        uint8_t* dest = &m_payloads[static_cast<size_t>(seqOffset) * UDP_PAYLOAD_BYTES];
        std::memcpy(dest, pkt.data, sz);
        m_dataSizes[seqOffset] = sz;
        ++m_receivedCount;
        return InsertResult::Accepted;
    }

    bool isComplete(int expectedPackets) const {
        return m_receivedCount >= expectedPackets;
    }

    // 导出到 TriggerGroup
    void exportTo(TriggerGroup& group, const AcqConfig& config) {
        int expected      = config.packetsPerTrig();
        int samplesPerTrig = config.samplesPerTrig();
        int bytesPerPair   = config.bytesPerSamplePair();

        group.triggerSeq   = m_triggerSeq;
        group.timestamp_ms = m_firstArrivalMs;
        group.isComplete   = (m_receivedCount >= expected);
        group.identity.wireTrigger = m_triggerSeq;
        group.identity.ingressId = m_firstIngressId;
        group.identity.firstReceiveMonotonicNs = m_firstReceiveMonotonicNs;
        group.identity.configVersion = m_configVersion;
        group.quality.schemaVersion = 1;
        group.quality.qualityUnknown = false;
        group.quality.assemblyComplete = group.isComplete;
        group.quality.expectedPacketCount = static_cast<uint32_t>(std::max(0, expected));
        group.quality.receivedPacketCount = static_cast<uint32_t>(std::max(0, m_receivedCount));
        group.quality.expectedPayloadBytes = static_cast<uint64_t>(std::max(0, samplesPerTrig)) *
                                             static_cast<uint64_t>(std::max(0, bytesPerPair));
        group.quality.actualPayloadBytes = 0;
        group.quality.packetCoverage.assign(static_cast<size_t>(std::max(0, expected)), 0);
        const uint64_t expectedBytes = group.quality.expectedPayloadBytes;
        bool lengthsValid = expected > 0 && bytesPerPair > 0;
        for (int p = 0; p < expected; ++p) {
            const int dataBytes = p < m_maxPackets ? static_cast<int>(m_dataSizes[p]) : 0;
            if (dataBytes > 0 && p < static_cast<int>(group.quality.packetCoverage.size()))
                group.quality.packetCoverage[static_cast<size_t>(p)] = 1;
            group.quality.actualPayloadBytes += static_cast<uint64_t>(std::max(0, dataBytes));
            const uint64_t offset = static_cast<uint64_t>(p) * UDP_PAYLOAD_BYTES;
            const uint64_t expectedLength = expectedBytes > offset
                ? std::min<uint64_t>(UDP_PAYLOAD_BYTES, expectedBytes - offset) : 0;
            if (expectedLength == 0 || dataBytes != static_cast<int>(expectedLength))
                lengthsValid = false;
        }
        // Count equality is not enough: a duplicate/out-of-range packet can
        // replace a missing slot while keeping the historical isComplete
        // counter true.  The downstream quality contract requires every
        // expected slot to be present.
        bool coverageComplete = expected > 0;
        for (int p = 0; p < expected; ++p) {
            if (p >= static_cast<int>(group.quality.packetCoverage.size()) ||
                group.quality.packetCoverage[static_cast<size_t>(p)] == 0) {
                coverageComplete = false;
                break;
            }
        }
        group.quality.packetCoverageComplete = coverageComplete;
        group.quality.packetLengthValid = lengthsValid;
        group.quality.sampleLengthValid = coverageComplete && lengthsValid &&
                                          group.quality.actualPayloadBytes == expectedBytes;
        group.quality.sampleOriginKnown = false;
        if (!coverageComplete) group.quality.missingReason = "packet-coverage-incomplete";
        else if (!lengthsValid) group.quality.missingReason = "packet-length-invalid";
        else if (!group.quality.sampleLengthValid) group.quality.missingReason = "sample-length-invalid";
        group.allocate(samplesPerTrig, config.displayPoints);

        int sampleIdx = 0;
        int pairsPerPkt = UDP_PAYLOAD_BYTES / bytesPerPair;

        for (int p = 0; p < expected && sampleIdx < samplesPerTrig; ++p) {
            int dataBytes = (p < m_maxPackets) ? m_dataSizes[p] : 0;
            if (dataBytes == 0) {
                int fill = std::min(pairsPerPkt, samplesPerTrig - sampleIdx);
                for (int s = 0; s < fill; ++s) {
                    group.freqA[sampleIdx + s] = 0.0f;
                    group.freqB[sampleIdx + s] = 0.0f;
                }
                sampleIdx += fill;
                continue;
            }
            int pairs = dataBytes / bytesPerPair;
            const uint8_t* buf = &m_payloads[static_cast<size_t>(p) * UDP_PAYLOAD_BYTES];
            for (int s = 0; s < pairs && sampleIdx < samplesPerTrig; ++s, ++sampleIdx) {
                if (bytesPerPair == 4) {
                    int16_t rawB, rawA;
                    std::memcpy(&rawB, buf + s * 4 + 0, 2);
                    std::memcpy(&rawA, buf + s * 4 + 2, 2);
                    group.freqB[sampleIdx] = static_cast<float>(rawB);
                    group.freqA[sampleIdx] = static_cast<float>(rawA);
                } else {
                    int32_t rawB, rawA;
                    std::memcpy(&rawB, buf + s * 8 + 0, 4);
                    std::memcpy(&rawA, buf + s * 8 + 4, 4);
                    group.freqB[sampleIdx] = static_cast<float>(rawB);
                    group.freqA[sampleIdx] = static_cast<float>(rawA);
                }
            }
        }
        group.sampleCount = sampleIdx;
    }

    void reset() {
        m_receivedCount  = 0;
        m_triggerSeq     = 0;
        m_basePacketSeq  = 0;
        m_firstArrivalMs = 0;
        m_firstIngressId = 0;
        m_firstReceiveMonotonicNs = 0;
        m_configVersion = 0;
        std::fill(m_receivedMask32.begin(), m_receivedMask32.end(), 0);
        std::fill(m_dataSizes.begin(), m_dataSizes.end(), 0);
    }

    uint16_t triggerSeq()    const { return m_triggerSeq; }
    int      receivedCount() const { return m_receivedCount; }

private:
    static uint64_t currentTimeMs() {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
    }

    int        m_maxPackets;
    uint16_t   m_triggerSeq        = 0;
    uint16_t   m_basePacketSeq     = 0;  // 当前触发第一包的全局计数器（减此值得触发内序号）
    uint64_t   m_firstArrivalMs    = 0;
    uint64_t   m_firstIngressId = 0;
    uint64_t   m_firstReceiveMonotonicNs = 0;
    uint64_t   m_configVersion = 0;
    int        m_receivedCount     = 0;
    std::vector<uint32_t> m_receivedMask32;
    std::vector<uint16_t> m_dataSizes;
    std::vector<uint8_t>  m_payloads;  // 平坦数组：m_payloads[seqOffset * UDP_PAYLOAD_BYTES + offset]
};
