#include "MeasurementSession.h"

#include <QTextStream>

#include <functional>

namespace {
bool check(bool condition, const QString &message)
{
    if (!condition) QTextStream(stderr) << "FAIL " << message << Qt::endl;
    return condition;
}

bool testOrdering()
{
    QStringList trace;
    int startSends = 0;
    const auto result = MeasurementSessionTransaction::start(
        4, 4,
        [&trace](int index) { trace.append(QStringLiteral("prepare%1").arg(index)); return true; },
        [&trace](int index) { trace.append(QStringLiteral("arm%1").arg(index)); return true; },
        [&trace, &startSends] {
            trace.append(QStringLiteral("hardware_start"));
            ++startSends;
            return MeasurementSessionTransaction::SendResult{4, 0};
        },
        [&trace] { trace.append(QStringLiteral("rollback")); });
    bool ok = check(result.success && startSends == 1, QStringLiteral("all-card start succeeds"));
    const int hardware = trace.indexOf(QStringLiteral("hardware_start"));
    const int lastArm = trace.lastIndexOf(QStringLiteral("arm3"));
    ok = check(hardware > lastArm, QStringLiteral("hardware start follows all processors armed")) && ok;
    return ok;
}

bool testPrepareFailureBlocksHardware()
{
    int startSends = 0;
    bool rolledBack = false;
    const auto result = MeasurementSessionTransaction::start(
        4, 4,
        [](int index) { return index != 2; },
        [](int) { return true; },
        [&startSends] {
            ++startSends;
            return MeasurementSessionTransaction::SendResult{4, 0};
        },
        [&rolledBack] { rolledBack = true; });
    bool ok = check(!result.success && startSends == 0,
                    QStringLiteral("prepare failure sends zero hardware starts"));
    ok = check(rolledBack && result.reason == QStringLiteral("processor_prepare_failed"),
               QStringLiteral("prepare failure rolls back local session")) && ok;
    return ok;
}

bool testPartialStartRollback()
{
    int stopRollbacks = 0;
    const auto result = MeasurementSessionTransaction::start(
        4, 4,
        [](int) { return true; },
        [](int) { return true; },
        [] { return MeasurementSessionTransaction::SendResult{3, 1}; },
        [&stopRollbacks] { ++stopRollbacks; });
    bool ok = check(!result.success && result.successCount == 3 && result.failCount == 1,
                    QStringLiteral("partial start is failure"));
    ok = check(stopRollbacks == 1 && result.reason == QStringLiteral("partial_start_send"),
               QStringLiteral("partial start performs one rollback")) && ok;
    return ok;
}

bool testCommitBarrierFailure()
{
    int startSends = 0;
    bool rolledBack = false;
    const auto result = MeasurementSessionTransaction::start(
        4, 4,
        [](int) { return true; },
        [](int) { return true; },
        [&startSends] {
            ++startSends;
            return MeasurementSessionTransaction::SendResult{4, 0};
        },
        [&rolledBack] { rolledBack = true; },
        {},
        [] { return false; });
    bool ok = check(!result.success && startSends == 1 && rolledBack,
                    QStringLiteral("admission commit failure rolls back after send"));
    ok = check(result.reason == QStringLiteral("receiver_commit_failed"),
               QStringLiteral("commit failure has explicit reason")) && ok;
    return ok;
}

bool testTeardownAggregation()
{
    const auto hardwareFailure = MeasurementSessionTransaction::teardown(
        2, 4, false,
        [](int index) { return index == 0; },
        [](int index) { return index != 3; });
    bool ok = check(!hardwareFailure.success
                        && hardwareFailure.receiverSuccessCount == 1
                        && hardwareFailure.receiverFailCount == 1
                        && hardwareFailure.processorSuccessCount == 3
                        && hardwareFailure.processorFailCount == 1
                        && hardwareFailure.reason == QStringLiteral("hardware_stop_send_failed"),
                    QStringLiteral("teardown aggregates hardware and barrier failures"));
    const auto processorFailure = MeasurementSessionTransaction::teardown(
        1, 2, true,
        [](int) { return true; },
        [](int index) { return index == 0; });
    ok = check(!processorFailure.success
                   && processorFailure.reason == QStringLiteral("processor_disarm_failed"),
               QStringLiteral("processor disarm failure is propagated")) && ok;
    return ok;
}
}

int main()
{
    bool ok = testOrdering();
    ok = testPrepareFailureBlocksHardware() && ok;
    ok = testPartialStartRollback() && ok;
    ok = testCommitBarrierFailure() && ok;
    ok = testTeardownAggregation() && ok;
    QTextStream(stdout) << (ok ? "PASS" : "FAIL")
                        << " measurement session transaction tests" << Qt::endl;
    return ok ? 0 : 1;
}
