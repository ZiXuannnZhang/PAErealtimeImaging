#include "MeasurementSession.h"

MeasurementSessionTransaction::Result MeasurementSessionTransaction::start(
    int processorCount,
    int targetCount,
    const ProcessorStep &prepare,
    const ProcessorStep &arm,
    const SendStep &sendStart,
    const RollbackStep &rollback,
    const StepObserver &observe,
    const FenceBeginStep &beginStartFence,
    const FenceCompleteStep &completeStartFence)
{
    Result result;
    const auto step = [&result, &observe](const QString &name) {
        result.steps.append(name);
        if (observe) observe(name);
    };
    if (processorCount <= 0 || targetCount <= 0 || !prepare || !arm || !sendStart
        || !beginStartFence || !completeStartFence) {
        result.reason = QStringLiteral("invalid_transaction_inputs");
        if (rollback) rollback();
        return result;
    }

    step(QStringLiteral("measurement_session_prepare"));
    for (int index = 0; index < processorCount; ++index) {
        if (prepare(index)) {
            ++result.prepareSuccessCount;
            continue;
        }
        ++result.prepareFailCount;
        result.reason = QStringLiteral("processor_prepare_failed");
        step(QStringLiteral("measurement_start_failed"));
        if (rollback) rollback();
        return result;
    }

    for (int index = 0; index < processorCount; ++index) {
        if (arm(index)) {
            ++result.armSuccessCount;
            continue;
        }
        ++result.armFailCount;
        result.reason = QStringLiteral("processor_arm_failed");
        step(QStringLiteral("measurement_start_failed"));
        if (rollback) rollback();
        return result;
    }
    step(QStringLiteral("measurement_session_armed"));

    step(QStringLiteral("measurement_start_command"));
    for (int cardIndex = 0; cardIndex < targetCount; ++cardIndex) {
        step(QStringLiteral("measurement_start_fence_begin"));
        if (!beginStartFence(cardIndex)) {
            ++result.startFenceBeginFailCount;
            result.reason = QStringLiteral("start_fence_begin_failed");
            step(QStringLiteral("measurement_start_fence_begin_failed"));
            step(QStringLiteral("measurement_start_failed"));
            step(QStringLiteral("measurement_start_rollback"));
            if (rollback) rollback();
            return result;
        }
        ++result.startFenceBeginSuccessCount;
        step(QStringLiteral("measurement_start_fence_begin_succeeded"));

        const SendResult sent = sendStart(cardIndex);
        result.successCount += sent.successCount;
        result.failCount += sent.failCount;
        if (sent.successCount == 1 && sent.failCount == 0) {
            step(QStringLiteral("measurement_start_fence_send_succeeded"));
            if (!completeStartFence(cardIndex, true)) {
                ++result.startFenceCompleteFailCount;
                result.reason = QStringLiteral("start_fence_complete_failed");
                step(QStringLiteral("measurement_start_fence_complete_failed"));
                step(QStringLiteral("measurement_start_failed"));
                step(QStringLiteral("measurement_start_rollback"));
                if (rollback) rollback();
                return result;
            }
            ++result.startFenceCompleteSuccessCount;
            step(QStringLiteral("measurement_start_fence_complete_succeeded"));
            continue;
        }

        step(QStringLiteral("measurement_start_fence_send_failed"));
        const bool failureCleanupSucceeded = completeStartFence(cardIndex, false);
        if (failureCleanupSucceeded) {
            ++result.startFenceCompleteSuccessCount;
            step(QStringLiteral("measurement_start_fence_failure_cleanup_succeeded"));
        } else {
            ++result.startFenceCompleteFailCount;
            step(QStringLiteral("measurement_start_fence_failure_cleanup_failed"));
        }
        result.reason = result.successCount > 0
            ? QStringLiteral("partial_start_send")
            : QStringLiteral("start_send_failed");
        step(QStringLiteral("measurement_start_failed"));
        step(QStringLiteral("measurement_start_rollback"));
        if (rollback) rollback();
        return result;
    }

    result.success = true;
    step(QStringLiteral("measurement_started"));
    result.reason = QStringLiteral("all_targets_sent");
    return result;
}

MeasurementSessionTransaction::TeardownResult
MeasurementSessionTransaction::teardown(
    int receiverCount,
    int processorCount,
    bool hardwareStopSucceeded,
    const ProcessorStep &disarmReceiver,
    const ProcessorStep &disarmProcessor)
{
    TeardownResult result;
    result.hardwareStopSucceeded = hardwareStopSucceeded;
    if (receiverCount < 0 || processorCount < 0 || !disarmReceiver || !disarmProcessor) {
        result.reason = QStringLiteral("invalid_teardown_inputs");
        return result;
    }
    for (int index = 0; index < receiverCount; ++index) {
        if (disarmReceiver(index)) ++result.receiverSuccessCount;
        else ++result.receiverFailCount;
    }
    for (int index = 0; index < processorCount; ++index) {
        if (disarmProcessor(index)) ++result.processorSuccessCount;
        else ++result.processorFailCount;
    }
    if (!hardwareStopSucceeded) {
        result.reason = QStringLiteral("hardware_stop_send_failed");
    } else if (result.receiverFailCount != 0) {
        result.reason = QStringLiteral("receiver_disarm_failed");
    } else if (result.processorFailCount != 0) {
        result.reason = QStringLiteral("processor_disarm_failed");
    } else {
        result.success = true;
        result.reason = QStringLiteral("teardown_complete");
    }
    return result;
}
