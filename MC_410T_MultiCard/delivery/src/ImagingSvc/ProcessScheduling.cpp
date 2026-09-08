#include "ProcessScheduling.h"

#include <QStringList>

#include <algorithm>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

constexpr quint64 kCpu0 = quint64(1) << 0;
constexpr quint64 kCpu2 = quint64(1) << 2;
constexpr quint64 kReservedMask = kCpu0 | kCpu2;

int bitCount(quint64 value)
{
    int count = 0;
    while (value != 0) {
        value &= value - 1;
        ++count;
    }
    return count;
}

QString maskText(quint64 mask)
{
    return QStringLiteral("0x%1")
        .arg(QString::number(static_cast<qulonglong>(mask), 16).toUpper());
}

#ifdef _WIN32

QString win32ErrorText(DWORD error)
{
    return QStringLiteral("error:%1").arg(error);
}

QString priorityName(DWORD priority)
{
    switch (priority) {
    case IDLE_PRIORITY_CLASS:
        return QStringLiteral("IDLE");
    case BELOW_NORMAL_PRIORITY_CLASS:
        return QStringLiteral("BELOW_NORMAL");
    case NORMAL_PRIORITY_CLASS:
        return QStringLiteral("NORMAL");
    case ABOVE_NORMAL_PRIORITY_CLASS:
        return QStringLiteral("ABOVE_NORMAL");
    case HIGH_PRIORITY_CLASS:
        return QStringLiteral("HIGH");
    case REALTIME_PRIORITY_CLASS:
        return QStringLiteral("REALTIME");
    default:
        return QStringLiteral("UNKNOWN(%1)").arg(priority);
    }
}

#endif

} // namespace

namespace ProcessScheduling {

AffinityDecision computeRequestedAffinity(quint64 processMask,
                                          quint64 systemMask)
{
    AffinityDecision decision;
    decision.originalMask = processMask;
    decision.requestedMask = processMask;
    decision.systemMask = systemMask;

    const quint64 usableMask = processMask & systemMask;
    if (bitCount(usableMask) < 4) {
        decision.reason = QStringLiteral("insufficient_logical_processors");
        return decision;
    }

    const quint64 requested = processMask & ~kReservedMask;
    if (requested == processMask) {
        decision.reason = QStringLiteral("reserved_cpus_not_present");
        return decision;
    }
    if (requested == 0) {
        decision.reason = QStringLiteral("zero_mask_prevented");
        return decision;
    }

    decision.requestedMask = requested;
    decision.shouldApply = true;
    decision.reason = QStringLiteral("excluded_cpu0_cpu2");
    return decision;
}

ApplyResult applyIngressProtectionScheduling()
{
    ApplyResult result;
    result.requestedPriority = QStringLiteral("BELOW_NORMAL");

#ifdef _WIN32
    HANDLE process = GetCurrentProcess();
    const BOOL prioritySet = SetPriorityClass(process, BELOW_NORMAL_PRIORITY_CLASS);
    if (prioritySet) {
        result.priorityApplied = true;
        result.priorityApply = QStringLiteral("ok");
    } else {
        result.priorityApply = win32ErrorText(GetLastError());
    }

    const DWORD effectivePriority = GetPriorityClass(process);
    if (effectivePriority != 0)
        result.effectivePriority = priorityName(effectivePriority);
    else
        result.effectivePriority = win32ErrorText(GetLastError());

    DWORD_PTR originalProcessMask = 0;
    DWORD_PTR systemMask = 0;
    if (!GetProcessAffinityMask(process, &originalProcessMask, &systemMask)) {
        const DWORD error = GetLastError();
        result.originalProcessMask = QStringLiteral("unknown");
        result.requestedProcessMask = QStringLiteral("unknown");
        result.affinityApply = win32ErrorText(error);
        result.effectiveProcessMask = QStringLiteral("unknown");
    } else {
        const auto decision = computeRequestedAffinity(
            static_cast<quint64>(originalProcessMask),
            static_cast<quint64>(systemMask));
        result.originalProcessMask = maskText(decision.originalMask);
        result.requestedProcessMask = maskText(decision.requestedMask);

        if (decision.shouldApply) {
            if (SetProcessAffinityMask(
                    process, static_cast<DWORD_PTR>(decision.requestedMask))) {
                result.affinityApplied = true;
                result.affinityApply = QStringLiteral("ok");
            } else {
                result.affinityApply = win32ErrorText(GetLastError());
            }
        } else {
            result.affinityApply = QStringLiteral("skipped:%1").arg(decision.reason);
        }

        DWORD_PTR effectiveProcessMask = 0;
        DWORD_PTR effectiveSystemMask = 0;
        if (GetProcessAffinityMask(process, &effectiveProcessMask,
                                    &effectiveSystemMask)) {
            result.effectiveProcessMask = maskText(
                static_cast<quint64>(effectiveProcessMask));
        } else {
            result.effectiveProcessMask = QStringLiteral("unknown");
            result.affinityApply += QStringLiteral(";effectiveQuery:%1")
                .arg(win32ErrorText(GetLastError()));
        }
    }
#else
    result.priorityApply = QStringLiteral("error:Windows_API_unavailable");
    result.effectivePriority = QStringLiteral("unknown");
    result.originalProcessMask = QStringLiteral("unknown");
    result.requestedProcessMask = QStringLiteral("unknown");
    result.affinityApply = QStringLiteral("error:Windows_API_unavailable");
    result.effectiveProcessMask = QStringLiteral("unknown");
#endif

    result.logLine = QStringLiteral(
        "[IngressProtection] requestedPriority=%1 priorityApply=%2 "
        "effectivePriority=%3 originalProcessMask=%4 "
        "requestedProcessMask=%5 affinityApply=%6 "
        "effectiveProcessMask=%7 reservedLogicalCpus=0,2")
        .arg(result.requestedPriority)
        .arg(result.priorityApply)
        .arg(result.effectivePriority)
        .arg(result.originalProcessMask)
        .arg(result.requestedProcessMask)
        .arg(result.affinityApply)
        .arg(result.effectiveProcessMask);
    return result;
}

} // namespace ProcessScheduling
