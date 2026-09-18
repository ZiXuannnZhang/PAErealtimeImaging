#include "RoundPolicySettings.h"

#include <QCoreApplication>
#include <QTemporaryDir>
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
    QTemporaryDir tempDir;
    if (!check(tempDir.isValid(), QStringLiteral("temp settings dir")))
        return 1;
    const QString iniPath = tempDir.filePath(QStringLiteral("session-b-policy.ini"));

    // 无历史 -> 出厂 1 / false
    {
        QSettings s(iniPath, QSettings::IniFormat);
        const auto policy = RoundPolicySettings::load(s);
        if (!check(policy.startupFilterTriggerCount == 1 && !policy.disableCountBoundary,
                   QStringLiteral("no history -> factory 1/false")))
            return 1;
    }

    // 保存 default 7 / true -> 重新读取（=“恢复默认”读取路径）-> 7 / true
    {
        QSettings s(iniPath, QSettings::IniFormat);
        RoundPolicySettings::save(s, RoundPolicySettings::Policy{7, true});
    }
    {
        QSettings s(iniPath, QSettings::IniFormat);
        const auto reloaded = RoundPolicySettings::load(s);
        if (!check(reloaded.startupFilterTriggerCount == 7 && reloaded.disableCountBoundary,
                   QStringLiteral("saved default 7/true reload")))
            return 1;
        const auto restored = RoundPolicySettings::load(s);
        if (!check(restored.startupFilterTriggerCount == 7 && restored.disableCountBoundary,
                   QStringLiteral("restore returns saved 7/true")))
            return 1;
        // 单一持久化源：两个 key 必须落在现有 RingConfigDialog/Defaults group
        if (!check(s.contains(QStringLiteral("RingConfigDialog/Defaults/startupFilterTriggerCount"))
                       && s.contains(QStringLiteral("RingConfigDialog/Defaults/disableCountBoundary")),
                   QStringLiteral("keys live in RingConfigDialog/Defaults")))
            return 1;
    }

    // 参数独立性：X=0 + disable=false
    {
        QSettings s(iniPath, QSettings::IniFormat);
        RoundPolicySettings::save(s, RoundPolicySettings::Policy{0, false});
    }
    {
        QSettings s(iniPath, QSettings::IniFormat);
        const auto policy = RoundPolicySettings::load(s);
        if (!check(policy.startupFilterTriggerCount == 0 && !policy.disableCountBoundary,
                   QStringLiteral("X=0 does not imply/alter disable=false")))
            return 1;
    }

    // key 级独立：只写 disable=true，startup 回退出厂 1；只写 startup=7，disable 回退 false
    const QString isolatedPath = tempDir.filePath(QStringLiteral("session-b-policy-isolated.ini"));
    {
        QSettings s(isolatedPath, QSettings::IniFormat);
        s.beginGroup(QStringLiteral("RingConfigDialog/Defaults"));
        s.setValue(QStringLiteral("disableCountBoundary"), true);
        s.endGroup();
    }
    {
        QSettings s(isolatedPath, QSettings::IniFormat);
        const auto policy = RoundPolicySettings::load(s);
        if (!check(policy.startupFilterTriggerCount == 1 && policy.disableCountBoundary,
                   QStringLiteral("only disable saved -> startup factory 1")))
            return 1;
    }
    {
        QSettings s(isolatedPath, QSettings::IniFormat);
        RoundPolicySettings::save(s, RoundPolicySettings::Policy{7, false});
        const auto policy = RoundPolicySettings::load(s);
        if (!check(policy.startupFilterTriggerCount == 7 && !policy.disableCountBoundary,
                   QStringLiteral("X=7 with disable=false stays independent")))
            return 1;
    }

    QTextStream(stdout) << "PASS round policy settings" << Qt::endl;
    return 0;
}
