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
        [&trace, &startSends](int cardIndex) {
            trace.append(QStringLiteral("hardware_start%1").arg(cardIndex));
            ++startSends;
            return MeasurementSessionTransaction::SendResult{1, 0};
        },
        [&trace] { trace.append(QStringLiteral("rollback")); },
        {},
        [&trace](int cardIndex) {
            trace.append(QStringLiteral("fence%1").arg(cardIndex));
            return true;
        });
    bool ok = check(result.success && startSends == 4, QStringLiteral("all-card start succeeds"));
    const int hardware = trace.indexOf(QStringLiteral("hardware_start0"));
    const int lastArm = trace.lastIndexOf(QStringLiteral("arm3"));
    ok = check(hardware > lastArm, QStringLiteral("hardware start follows all processors armed")) && ok;
    ok = check(trace.indexOf(QStringLiteral("fence0")) > hardware
                   && trace.indexOf(QStringLiteral("hardware_start1"))
                          > trace.indexOf(QStringLiteral("fence0")),
               QStringLiteral("each card fence follows its local start")) && ok;
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
        [&startSends](int) {
            ++startSends;
            return MeasurementSessionTransaction::SendResult{1, 0};
        },
        [&rolledBack] { rolledBack = true; },
        {},
        [](int) { return true; });
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
        [](int index) {
            return index == 3
                ? MeasurementSessionTransaction::SendResult{0, 1}
                : MeasurementSessionTransaction::SendResult{1, 0};
        },
        [&stopRollbacks] { ++stopRollbacks; },
        {},
        [](int) { return true; });
    bool ok = check(!result.success && result.successCount == 3 && result.failCount == 1,
                    QStringLiteral("partial start is failure"));
    ok = check(stopRollbacks == 1 && result.reason == QStringLiteral("partial_start_send"),
               QStringLiteral("partial start performs one rollback")) && ok;
    return ok;
}

bool testFenceBarrierFailure()
{
    int startSends = 0;
    bool rolledBack = false;
    const auto result = MeasurementSessionTransaction::start(
        4, 4,
        [](int) { return true; },
        [](int) { return true; },
        [&startSends](int) {
            ++startSends;
            return MeasurementSessionTransaction::SendResult{1, 0};
        },
        [&rolledBack] { rolledBack = true; },
        {},
        [](int) { return false; });
    bool ok = check(!result.success && startSends == 1 && rolledBack,
                    QStringLiteral("fence failure rolls back after local hardware send"));
    ok = check(result.reason == QStringLiteral("start_fence_failed"),
               QStringLiteral("fence failure has explicit reason")) && ok;
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
    ok = testFenceBarrierFailure() && ok;
    ok = testTeardownAggregation() && ok;
    QTextStream(stdout) << (ok ? "PASS" : "FAIL")
                        << " measurement session transaction tests" << Qt::endl;
    return ok ? 0 : 1;
}
