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
    stats.missingTriggerCount = 4;
    stats.packetsDropped = 7;
    stats.inputQueueDepth = 100;
    stats.saveQueueDepth = 5;
    stats.socketPacketsReceived = 1000;
    stats.processorPacketsDequeued = 900;
    stats.batchBoundaryDiscards = 0;
    stats.recvMbps = 123.45;
    stats.triggerHz = 99.5;

    const CardStatusFormatting::RoundDisplay round{4007, 7};
    const QString text = CardStatusFormatting::text(1, stats, round);
    const QString tooltip = CardStatusFormatting::tooltip(stats, round);

    // Session B 冻结主状态行：缺失=triggersPartial，跳号数=missingTriggerCount，
    // 已采集=全局物理轮 distinct physical trigger 数（含被启动过滤的）
    bool ok = check(text == QStringLiteral("卡1 | 缺失: 3 | 跳号数: 4 | 已采集: 4007"),
                    QStringLiteral("resident status text frozen semantics"));
    ok = check(!text.contains(QStringLiteral("已过滤"))
                   && !text.contains(QStringLiteral("处队"))
                   && !text.contains(QStringLiteral("存队"))
                   && !text.contains(QStringLiteral("丢弃")),
               QStringLiteral("resident status text is compact")) && ok;
    for (const QString& key : {QStringLiteral("已过滤: 7"), QStringLiteral("丢包: 7"),
                               QStringLiteral("触发完成: 12"), QStringLiteral("处队: 100"),
                               QStringLiteral("存队: 5"), QStringLiteral("Socket接收: 1000"),
                               QStringLiteral("Processor出队: 900"),
                               QStringLiteral("批边界丢弃: 0"),
                               QStringLiteral("触发丢弃"), QStringLiteral("存储队列丢弃")})
        ok = check(tooltip.contains(key), QStringLiteral("tooltip key: %1").arg(key)) && ok;
    // packetsDropped 含 full-gap 包当量，不得再叫“不完整触发缺包数”
    ok = check(!tooltip.contains(QStringLiteral("不完整触发缺包数")),
               QStringLiteral("tooltip drops misleading partial-only wording")) && ok;

    // current/lastCompleted presentation fallback（boundary 后 current 清零防闪 0；
    // 新一轮首枚 distinct physical trigger 出现即切回 current）
    paimage::PhysicalRoundNormalizer::Snapshot afterBoundary;
    afterBoundary.currentPhysicalDistinctCount = 0;
    afterBoundary.currentStartupFilteredCount = 0;
    afterBoundary.lastCompletedPhysicalDistinctCount = 4007;
    afterBoundary.lastCompletedStartupFilteredCount = 7;
    const auto fallback = CardStatusFormatting::roundDisplayForUi(afterBoundary);
    ok = check(fallback.collected == 4007 && fallback.filtered == 7,
               QStringLiteral("current==0 falls back to lastCompleted")) && ok;

    paimage::PhysicalRoundNormalizer::Snapshot collecting = afterBoundary;
    collecting.currentPhysicalDistinctCount = 12;
    collecting.currentStartupFilteredCount = 3;
    const auto current = CardStatusFormatting::roundDisplayForUi(collecting);
    ok = check(current.collected == 12 && current.filtered == 3,
               QStringLiteral("current>0 switches back to current counters")) && ok;

    if (ok)
        QTextStream(stdout) << "PASS card status formatting" << Qt::endl;
    return ok ? 0 : 1;
}
