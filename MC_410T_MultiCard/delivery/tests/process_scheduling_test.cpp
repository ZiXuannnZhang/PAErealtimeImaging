#include "ProcessScheduling.h"

#include <QCoreApplication>
#include <QTextStream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

bool check(bool condition, const QString& message)
{
    if (!condition)
        QTextStream(stderr) << "FAIL " << message << Qt::endl;
    return condition;
}

bool testMaskDecision()
{
    using ProcessScheduling::computeRequestedAffinity;

    const quint64 highMask = (quint64(1) << 63) | (quint64(1) << 6)
        | (quint64(1) << 4) | (quint64(1) << 2) | (quint64(1) << 0);
    const auto high = computeRequestedAffinity(highMask, highMask);
    bool ok = check(high.shouldApply
                        && high.requestedMask == ((quint64(1) << 63)
                                                   | (quint64(1) << 6)
                                                   | (quint64(1) << 4)),
                    QStringLiteral("CPU0/CPU2 excluded while high bits remain"));

    const quint64 noCpu2Mask = (quint64(1) << 8) | (quint64(1) << 6)
        | (quint64(1) << 4) | (quint64(1) << 0);
    const auto noCpu2 = computeRequestedAffinity(noCpu2Mask, noCpu2Mask);
    ok = check(noCpu2.shouldApply && noCpu2.requestedMask == (noCpu2Mask & ~quint64(1)),
               QStringLiteral("missing CPU2 does not create an invalid bit")) && ok;

    const quint64 smallMask = (quint64(1) << 2) | (quint64(1) << 1)
        | (quint64(1) << 0);
    const auto small = computeRequestedAffinity(smallMask, smallMask);
    ok = check(!small.shouldApply && small.requestedMask == smallMask
                   && small.reason == QStringLiteral("insufficient_logical_processors"),
               QStringLiteral("small mask keeps a runnable fallback")) && ok;

    const quint64 unavailableReserved = (quint64(1) << 7) | (quint64(1) << 6)
        | (quint64(1) << 5) | (quint64(1) << 4);
    const auto absent = computeRequestedAffinity(unavailableReserved, unavailableReserved);
    return check(!absent.shouldApply && absent.requestedMask == unavailableReserved
                     && absent.reason == QStringLiteral("reserved_cpus_not_present"),
                 QStringLiteral("absent reserved CPUs are reported without changing mask")) && ok;
}

#ifdef _WIN32
bool testApplyAndRestore()
{
    HANDLE process = GetCurrentProcess();
    DWORD_PTR originalMask = 0;
    DWORD_PTR systemMask = 0;
    if (!check(GetProcessAffinityMask(process, &originalMask, &systemMask),
               QStringLiteral("read original process affinity")))
        return false;
    const DWORD originalPriority = GetPriorityClass(process);
    if (!check(originalPriority != 0, QStringLiteral("read original process priority")))
        return false;

    const auto result = ProcessScheduling::applyIngressProtectionScheduling();
    QTextStream(stdout) << result.logLine << Qt::endl;
    bool ok = check(!result.originalProcessMask.isEmpty()
                        && !result.requestedProcessMask.isEmpty()
                        && !result.effectiveProcessMask.isEmpty(),
                    QStringLiteral("scheduling result contains masks"));
    ok = check(!result.priorityApply.isEmpty() && !result.effectivePriority.isEmpty(),
               QStringLiteral("scheduling result contains priority status")) && ok;
    if (result.priorityApply == QStringLiteral("ok")) {
        ok = check(result.effectivePriority == QStringLiteral("BELOW_NORMAL"),
                   QStringLiteral("effective priority is BELOW_NORMAL")) && ok;
    }
    ok = check(!result.affinityApply.isEmpty(),
               QStringLiteral("scheduling result contains affinity status")) && ok;
    if (result.affinityApply == QStringLiteral("ok")) {
        ok = check(result.effectiveProcessMask == result.requestedProcessMask,
                   QStringLiteral("effective affinity equals requested mask")) && ok;
    }

    const BOOL restoredAffinity = SetProcessAffinityMask(process, originalMask);
    const BOOL restoredPriority = SetPriorityClass(process, originalPriority);
    ok = check(restoredAffinity && restoredPriority,
               QStringLiteral("restore test process scheduling state")) && ok;
    return ok;
}
#endif

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    bool ok = testMaskDecision();
#ifdef _WIN32
    ok = testApplyAndRestore() && ok;
#endif
    if (ok)
        QTextStream(stdout) << "PASS process scheduling tests" << Qt::endl;
    return ok ? 0 : 1;
}
