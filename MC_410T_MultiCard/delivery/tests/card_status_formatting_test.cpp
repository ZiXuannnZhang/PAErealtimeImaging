#include "CardStatusFormatting.h"
#include "ChannelNaming.h"

#include <QCoreApplication>
#include <QStringList>
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
    stats.packetLossRate = 0.000125;
    stats.inputQueueDepth = 100;
    stats.saveQueueDepth = 5;
    stats.socketPacketsReceived = 1000;
    stats.processorPacketsDequeued = 900;
    stats.batchBoundaryDiscards = 0;
    stats.triggersDiscarded = 5;
    stats.saveQueueDiscards = 3;
    stats.recvMbps = 123.45;
    stats.triggerHz = 99.5;

    const CardStatusFormatting::RoundDisplay round{4007, 7};
    const QString text = CardStatusFormatting::text(1, stats, round);
    const QString tooltip = CardStatusFormatting::tooltip(stats, round);

    // A1/A2 常驻栏冻结语义：缺失=triggersPartial，已采集=全局物理轮 distinct
    // physical trigger 数（含被启动过滤的）。跳号数已移入 tooltip。
    bool ok = check(text == QStringLiteral("卡1 | 缺失: 3 | 已采集: 4007"),
                    QStringLiteral("A1 resident status text is 缺失/已采集 only"));
    ok = check(!text.contains(QStringLiteral("跳号数")),
               QStringLiteral("A1 跳号数 is no longer resident")) && ok;
    ok = check(!text.contains(QStringLiteral("已过滤"))
                   && !text.contains(QStringLiteral("处队"))
                   && !text.contains(QStringLiteral("存队"))
                   && !text.contains(QStringLiteral("丢弃")),
               QStringLiteral("A2 resident status text is compact")) && ok;

    // A3 跳号数自常驻栏移入悬停表（missingTriggerCount：完全 0 包到达的 trigger 数）
    ok = check(tooltip.contains(QStringLiteral("跳号数: 4")),
               QStringLiteral("A3 跳号数 moved into the tooltip")) && ok;

    // A4/A5 两处真重复已合并：被并入项不再单独成行，原名以括注保留，术语不丢
    //   * 丢失包率 = packetsDropped 的相对值（同一量的另一种表达）
    //   * 存储队列丢弃与触发丢弃同步递增（前者是后者的成因细分）
    ok = check(tooltip.contains(QStringLiteral("丢包: 7（丢失包率: 0.000125）")),
               QStringLiteral("A4 丢失包率 folded into 丢包")) && ok;
    ok = check(tooltip.contains(QStringLiteral("触发丢弃: 5（存储队列丢弃: 3）")),
               QStringLiteral("A4 存储队列丢弃 folded into 触发丢弃")) && ok;
    ok = check(!tooltip.contains(QStringLiteral("\n丢失包率:"))
                   && !tooltip.startsWith(QStringLiteral("丢失包率:")),
               QStringLiteral("A5 丢失包率 no longer occupies its own line")) && ok;
    ok = check(!tooltip.contains(QStringLiteral("\n存储队列丢弃:")),
               QStringLiteral("A5 存储队列丢弃 no longer occupies its own line")) && ok;

    // A6 其余项逐项仍在，位置不动
    for (const QString& key : {QStringLiteral("触发完成: 12"), QStringLiteral("处队: 100"),
                               QStringLiteral("存队: 5"), QStringLiteral("Socket接收: 1000"),
                               QStringLiteral("Processor出队: 900"),
                               QStringLiteral("批边界丢弃: 0"),
                               QStringLiteral("速率: 123.45 Mb/s"),
                               QStringLiteral("触发率: 99.50 Hz"),
                               QStringLiteral("已过滤: 7")})
        ok = check(tooltip.contains(key), QStringLiteral("A6 tooltip key: %1").arg(key)) && ok;
    // 13 项 -> 12 项（移入 1、合并 2），行数同步收窄
    ok = check(tooltip.count(QLatin1Char('\n')) == 11,
               QStringLiteral("A6 tooltip is 12 lines")) && ok;

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

    // C 系列：通道命名 ↔ 主程序时域/频域信号选项卡命名的逐位对应。
    // 下标 c = cardId*2 + (ch=='B'?1:0)，源头是 ImagingBypass::tryPush 的
    // channelA = frame->cardId * 2（即 RingBlockAssembler 的物理通道号）。
    // 主窗口选项卡用完整名「卡1-通道A」，环形弹窗勾选框用简要名「1-A」。
    {
        const QStringList expectedShort = {
            QStringLiteral("1-A"), QStringLiteral("1-B"),
            QStringLiteral("2-A"), QStringLiteral("2-B"),
            QStringLiteral("3-A"), QStringLiteral("3-B"),
            QStringLiteral("4-A"), QStringLiteral("4-B")};
        const QStringList expectedFull = {
            QStringLiteral("卡1-通道A"), QStringLiteral("卡1-通道B"),
            QStringLiteral("卡2-通道A"), QStringLiteral("卡2-通道B"),
            QStringLiteral("卡3-通道A"), QStringLiteral("卡3-通道B"),
            QStringLiteral("卡4-通道A"), QStringLiteral("卡4-通道B")};
        for (int i = 0; i < 8; ++i) {
            ok = check(ChannelNaming::channelShortName(i) == expectedShort.at(i),
                       QStringLiteral("C1 channelShortName(%1)").arg(i)) && ok;
            ok = check(ChannelNaming::channelFullName(i) == expectedFull.at(i),
                       QStringLiteral("C2 channelFullName(%1)").arg(i)) && ok;
            // C3 两种命名必须由同一 (卡序号, 通道字母) 生成，否则勾选框会与
            // 前端选项卡错位——这是"与选项卡对应"这条要求的可执行形式。
            const int card = ChannelNaming::channelCardNumber(i);
            const QChar letter = ChannelNaming::channelLetter(i);
            ok = check(card == i / 2 + 1 && letter == (i % 2 == 0 ? QLatin1Char('A')
                                                                 : QLatin1Char('B')),
                       QStringLiteral("C3 derivation(%1)").arg(i)) && ok;
            ok = check(ChannelNaming::channelFullName(i) ==
                           QStringLiteral("卡%1-通道%2").arg(card).arg(letter),
                       QStringLiteral("C3 full name from shared pair(%1)").arg(i)) && ok;
            ok = check(ChannelNaming::channelShortName(i) ==
                           QStringLiteral("%1-%2").arg(card).arg(letter),
                       QStringLiteral("C3 short name from shared pair(%1)").arg(i)) && ok;
        }
    }

    if (ok)
        QTextStream(stdout) << "PASS card status formatting" << Qt::endl;
    return ok ? 0 : 1;
}
