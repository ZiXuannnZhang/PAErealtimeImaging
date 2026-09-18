#pragma once

#include <QString>
#include "DataTypes.h"

namespace CardStatusFormatting {

// 全局物理轮次显示态（Session B）：来自 PhysicalRoundNormalizer::Snapshot，
// 不是 per-card CardStats；各卡行显示相同值是预期行为。
struct RoundDisplay {
    quint64 collected = 0;   // 已采集：当前物理轮 distinct physical trigger 数（含被启动过滤的）
    quint64 filtered  = 0;   // 已过滤：当前物理轮实际被启动过滤的 distinct physical trigger 数
};

// boundary 后 Normalizer 将 current counters 清零并 latch 到 lastCompleted*；
// 为避免 UI 在轮次边界闪 0，current 为 0 时回退显示上一轮 completed 值；
// 新一轮首枚 distinct physical trigger 出现（current > 0）即切回新轮 current。
// 仅 presentation fallback，不改变 Normalizer 真实 counters。
inline RoundDisplay roundDisplayForUi(const paimage::PhysicalRoundNormalizer::Snapshot& round)
{
    if (round.currentPhysicalDistinctCount > 0)
        return {round.currentPhysicalDistinctCount, round.currentStartupFilteredCount};
    return {round.lastCompletedPhysicalDistinctCount, round.lastCompletedStartupFilteredCount};
}

inline QString text(int cardNumber, const CardStats::Snapshot& stats,
                    const RoundDisplay& round)
{
    // 冻结口径：缺失 = 部分到达但未完整组装的 trigger 数（triggersPartial）；
    // 跳号数 = 完全 0 包到达的 missing trigger 数（missingTriggerCount）；
    // 已采集 = 全局物理轮 distinct physical trigger 数（含被启动过滤的）。
    return QStringLiteral("卡%1 | 缺失: %2 | 跳号数: %3 | 已采集: %4")
        .arg(cardNumber)
        .arg(QString::number(static_cast<qulonglong>(stats.triggersPartial)))
        .arg(QString::number(static_cast<qulonglong>(stats.missingTriggerCount)))
        .arg(QString::number(static_cast<qulonglong>(round.collected)));
}

inline QString tooltip(const CardStats::Snapshot& stats, const RoundDisplay& round)
{
    // 丢包 = packetsDropped 原语义：partial trigger 内缺失包 + 完整 missing
    // trigger 的 gap * expectedPackets 包当量（不是 trigger 数）。
    return QStringLiteral(
        "触发完成: %1\n"
        "丢包: %2\n"
        "处队: %3\n"
        "存队: %4\n"
        "Socket接收: %5\n"
        "Processor出队: %6\n"
        "批边界丢弃: %7\n"
        "速率: %8 Mb/s\n"
        "触发率: %9 Hz\n"
        "丢失包率: %10\n"
        "触发丢弃: %11\n"
        "存储队列丢弃: %12\n"
        "已过滤: %13")
        .arg(QString::number(static_cast<qulonglong>(stats.triggersComplete)))
        .arg(QString::number(static_cast<qulonglong>(stats.packetsDropped)))
        .arg(stats.inputQueueDepth)
        .arg(stats.saveQueueDepth)
        .arg(QString::number(static_cast<qulonglong>(stats.socketPacketsReceived)))
        .arg(QString::number(static_cast<qulonglong>(stats.processorPacketsDequeued)))
        .arg(QString::number(static_cast<qulonglong>(stats.batchBoundaryDiscards)))
        .arg(stats.recvMbps, 0, 'f', 2)
        .arg(stats.triggerHz, 0, 'f', 2)
        .arg(stats.packetLossRate, 0, 'f', 6)
        .arg(QString::number(static_cast<qulonglong>(stats.triggersDiscarded)))
        .arg(QString::number(static_cast<qulonglong>(stats.saveQueueDiscards)))
        .arg(QString::number(static_cast<qulonglong>(round.filtered)));
}

} // namespace CardStatusFormatting
