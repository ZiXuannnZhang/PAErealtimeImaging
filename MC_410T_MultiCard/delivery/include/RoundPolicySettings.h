#pragma once

#include <QSettings>
#include <QString>

#include "PaimageAcquisition/SettingsPath.h"

// 物理轮次启动策略（startupFilterTriggerCount / disableCountBoundary）的
// 单一持久化源。RingConfigDialog 的“设为默认/恢复默认”与 MainWindow 在
// backend 创建前的启动读取共用这一组 helper，避免双 QSettings 源失步。
//
// 存储位置：现有 RingConfigDialog/Defaults group 内的两个独立 key；
// 无历史 key 时回退出厂默认 1 / false。两个参数完全独立持久化，
// 互不推导（startupFilterTriggerCount==0 不影响 disableCountBoundary，反之亦然）。
namespace RoundPolicySettings {

struct Policy {
    quint64 startupFilterTriggerCount = 1;   // 出厂默认：过滤每物理轮首枚 distinct trigger
    bool     disableCountBoundary = false;   // 出厂默认：保留计数边界
};

inline QString groupName() { return QStringLiteral("RingConfigDialog/Defaults"); }
inline QString keyStartupFilterTriggerCount() { return QStringLiteral("startupFilterTriggerCount"); }
inline QString keyDisableCountBoundary() { return QStringLiteral("disableCountBoundary"); }

// 从调用方提供的 QSettings 读取；调用时不得处于任何 group 内。
// 缺 key（无“设为默认”历史）时对应字段保持出厂默认。
inline Policy load(QSettings &settings)
{
    Policy policy;
    settings.beginGroup(groupName());
    if (settings.contains(keyStartupFilterTriggerCount()))
        policy.startupFilterTriggerCount =
            settings.value(keyStartupFilterTriggerCount()).toULongLong();
    if (settings.contains(keyDisableCountBoundary()))
        policy.disableCountBoundary =
            settings.value(keyDisableCountBoundary()).toBool();
    settings.endGroup();
    return policy;
}

// 写入调用方提供的 QSettings；两个 key 独立写入，不触碰 group 内其他参数。
inline void save(QSettings &settings, const Policy &policy)
{
    settings.beginGroup(groupName());
    settings.setValue(keyStartupFilterTriggerCount(), policy.startupFilterTriggerCount);
    settings.setValue(keyDisableCountBoundary(), policy.disableCountBoundary);
    settings.endGroup();
    settings.sync();
}

// 生产入口：程序正式 INI（PAimageReceiverDiagnostics.ini）。
// MainWindow 在 RingConfigDialog 尚未 lazy-create 时也经由此处读取已保存策略。
inline Policy loadDefault()
{
    QSettings settings(paimageSettingsPath(), QSettings::IniFormat);
    return load(settings);
}

} // namespace RoundPolicySettings
