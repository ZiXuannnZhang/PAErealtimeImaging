#pragma once

#include <QString>
#include "DataTypes.h"

namespace CardStatusFormatting {

inline QString text(int cardNumber, const CardStats::Snapshot& stats)
{
    // triggersPartial counts triggers that arrived partially (closed by
    // trigger switch or timeout), not triggers the card never received.
    return QStringLiteral("卡%1 | 报文不完整触发: %2")
        .arg(cardNumber)
        .arg(QString::number(static_cast<qulonglong>(stats.triggersPartial)));
}

inline QString tooltip(const CardStats::Snapshot& stats)
{
    return QStringLiteral(
        "触发完成: %1\n"
        "不完整触发缺包数: %2\n"
        "处队: %3\n"
        "存队: %4\n"
        "Socket接收: %5\n"
        "Processor出队: %6\n"
        "批边界丢弃: %7\n"
        "速率: %8 Mb/s\n"
        "触发率: %9 Hz\n"
        "丢失包率: %10\n"
        "触发丢弃: %11\n"
        "存储队列丢弃: %12")
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
        .arg(QString::number(static_cast<qulonglong>(stats.saveQueueDiscards)));
}

} // namespace CardStatusFormatting
