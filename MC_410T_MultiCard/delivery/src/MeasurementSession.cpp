#include "MeasurementSession.h"

MeasurementSessionTransaction::Result MeasurementSessionTransaction::start(
    int processorCount,
    int targetCount,
    const ProcessorStep &prepare,
    const ProcessorStep &arm,
    const SendStep &sendStart,
    const RollbackStep &rollback,
    const StepObserver &observe)
{
    Result result;
    const auto step = [&result, &observe](const QString &name) {
        result.steps.append(name);
        if (observe) observe(name);
    };
    if (processorCount <= 0 || targetCount <= 0 || !prepare || !arm || !sendStart) {
        result.reason = QStringLiteral("invalid_transaction_inputs");
        if (rollback) rollback();
        return result;
    }

    step(QStringLiteral("measurement_session_prepare"));
    for (int index = 0; index < processorCount; ++index) {
        if (prepare(index)) continue;
        result.reason = QStringLiteral("processor_prepare_failed");
        step(QStringLiteral("measurement_start_failed"));
        if (rollback) rollback();
        return result;
    }

    for (int index = 0; index < processorCount; ++index) {
        if (arm(index)) continue;
        result.reason = QStringLiteral("processor_arm_failed");
        step(QStringLiteral("measurement_start_failed"));
        if (rollback) rollback();
        return result;
    }
    step(QStringLiteral("measurement_session_armed"));

    step(QStringLiteral("measurement_start_command"));
    const SendResult sent = sendStart();
    result.successCount = sent.successCount;
    result.failCount = sent.failCount;
    if (sent.successCount == targetCount && sent.failCount == 0) {
        result.success = true;
        step(QStringLiteral("measurement_started"));
        result.reason = QStringLiteral("all_targets_sent");
        return result;
    }

    result.reason = sent.successCount > 0
        ? QStringLiteral("partial_start_send")
        : QStringLiteral("start_send_failed");
    step(QStringLiteral("measurement_start_failed"));
    step(QStringLiteral("measurement_start_rollback"));
    if (rollback) rollback();
    return result;
}
