#include "CardStatusFormatting.h"

#include <QCoreApplication>
#include <QTextStream>

namespace {
bool check(bool condition, const QString& message)
{
    if (!condition)
        QTextStream(stderr) << "FAIL " << message << Qt::endl;
    return condition;
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    CardStats::Snapshot stats;
    stats.cardId = 0;
    stats.triggersComplete = 12;
    stats.triggersPartial = 3;
    stats.packetsDropped = 7;
    stats.inputQueueDepth = 100;
    stats.saveQueueDepth = 5;
    stats.socketPacketsReceived = 1000;
    stats.processorPacketsDequeued = 900;
    stats.batchBoundaryDiscards = 0;
    stats.recvMbps = 123.45;
    stats.triggerHz = 99.5;

    const QString text = CardStatusFormatting::text(1, stats);
    const QString tooltip = CardStatusFormatting::tooltip(stats);
    bool ok = check(text == QStringLiteral("卡1 | 丢失: 3"),
                    QStringLiteral("resident status text contains only loss"));
    ok = check(!text.contains(QStringLiteral("缺失"))
                   && !text.contains(QStringLiteral("处队"))
                   && !text.contains(QStringLiteral("存队"))
                   && !text.contains(QStringLiteral("丢弃")),
               QStringLiteral("resident status text is compact")) && ok;
    for (const QString& key : {QStringLiteral("缺失: 7"), QStringLiteral("处队: 100"),
                               QStringLiteral("存队: 5"), QStringLiteral("Socket接收: 1000"),
                               QStringLiteral("Processor出队: 900"),
                               QStringLiteral("批边界丢弃: 0")})
        ok = check(tooltip.contains(key), QStringLiteral("tooltip key: %1").arg(key)) && ok;

    if (ok)
        QTextStream(stdout) << "PASS card status formatting" << Qt::endl;
    return ok ? 0 : 1;
}
