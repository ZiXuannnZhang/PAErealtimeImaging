#pragma once

#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

// Shared by the UI and console-only tests. Never infer this trial's readiness
// from another listener's state file, or from the state name alone.
namespace SystemCaptureStatus {
inline bool matches(const QJsonObject &state, const QString &trial, const QString &token,
                    const QString &run, const QString &listen, const QString &output)
{
    return !trial.isEmpty() && !token.isEmpty() && !run.isEmpty() && !listen.isEmpty()
        && state.value("trialId").toString() == trial
        && state.value("captureSessionToken").toString() == token
        && state.value("runId").toString() == run
        && state.value("listenId").toString() == listen
        && QDir::cleanPath(state.value("outputDirectory").toString()).compare(
               QDir::cleanPath(output), Qt::CaseInsensitive) == 0;
}

inline QString failureDetails(const QJsonObject &state)
{
    QStringList reasons;
    for (const auto &value : state.value("failureReasons").toArray()) {
        if (!value.toString().isEmpty()) reasons.append(value.toString());
    }
    return reasons.join(QStringLiteral("；"));
}

inline QString describe(const QJsonObject &state)
{
    const QString phase=state.value("status").toString();
    const bool simulated=state.value("simulation").toBool(false);
    if (phase=="failed") {
        const QString reason=failureDetails(state);
        if (reason.contains("administrator privileges")) return QStringLiteral("需要管理员权限（尚未启动）");
        return QStringLiteral("抓取失败：%1").arg(reason.isEmpty()?QStringLiteral("原因未记录，请查看状态文件"):reason);
    }
    if (phase=="ready" || phase=="collecting") {
        const auto ready=state.value("ready").toObject();
        const auto resources=state.value("resources").toObject();
        const bool confirmed=ready.value("pktmonConfirmed").toBool()
            && ready.value("wprConfirmed").toBool()
            && ready.value("applicationHandshakeMatched").toBool()
            && resources.value("pktmonStarted").toBool()
            && (state.value("noWpr").toBool() || resources.value("wprStarted").toBool());
        if (!confirmed) return QStringLiteral("状态异常（缺少启动确认，不能判为已就绪）");
        if (simulated) return QStringLiteral("模拟采集（不是真实系统抓取）");
        return phase=="ready" ? QStringLiteral("已就绪（可开始物理触发）")
                              : QStringLiteral("正在采集（已就绪，可开始物理触发）");
    }
    if (phase=="stopping" || phase=="validating") return QStringLiteral("正在停止／验证证据");
    if (phase=="complete") {
        if (!simulated && state.value("analysisReady").toBool() && state.value("targetPacketsPresent").toBool())
            return QStringLiteral("证据验证通过");
        return simulated ? QStringLiteral("模拟流程完成（非实机验证）") : QStringLiteral("已完成，但证据未验证");
    }
    if (phase=="no_target_packets") return QStringLiteral("已结束（没有目标数据包）");
    if (phase=="partial") return QStringLiteral("部分完成：%1").arg(failureDetails(state));
    if (phase=="idle" || phase=="preflight" || phase=="starting") return QStringLiteral("启动中（尚未就绪）");
    return QStringLiteral("状态未知（%1，尚未确认启动）").arg(phase);
}
}
