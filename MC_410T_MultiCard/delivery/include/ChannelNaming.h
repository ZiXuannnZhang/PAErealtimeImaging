#pragma once

#include <QString>

// ============================================================
// ChannelNaming  通道下标 ↔ (卡序号, 通道 A/B) 的唯一换算
//
// 下标 c = cardId * 2 + (ch == 'B' ? 1 : 0)，即 0..7 = 4 张卡 × 2 通道。
// 该约定的源头是 ImagingBypass::tryPush()：
//     const int channelA = frame->cardId * 2;
//     const int channelB = channelA + 1;
// 也就是 RingBlockAssembler / RingReconCudaConfig::enabledChannels 用的物理通道号。
//
// 本头文件是两种显示命名的唯一入口，两个界面不得各自再写一套换算：
//   * 主程序时域/频域信号的选项卡  -> channelFullName()  "卡1-通道A"
//   * 环形扫描参数设定的启用通道勾选框 -> channelShortName()    "1-A"
// 两者必须逐位对应（同一个 c 指向同一物理通道），由 card_status_formatting_test
// 的 C 系列断言锁定。
// ============================================================

namespace ChannelNaming {

//  该下标属于第几张卡（1-based）
inline int channelCardNumber(int index) noexcept
{
    return index / 2 + 1;
}

//  该下标是该卡的哪个通道
inline QChar channelLetter(int index) noexcept
{
    return index % 2 == 0 ? QLatin1Char('A') : QLatin1Char('B');
}

//  主程序时域/频域信号选项卡用的完整命名
inline QString channelFullName(int index)
{
    return QStringLiteral("卡%1-通道%2")
        .arg(channelCardNumber(index))
        .arg(channelLetter(index));
}

//  环形扫描参数设定弹窗用的简要命名（前端选项卡「卡1-通道A」⇒「1-A」）
inline QString channelShortName(int index)
{
    return QStringLiteral("%1-%2")
        .arg(channelCardNumber(index))
        .arg(channelLetter(index));
}

}  // namespace ChannelNaming
