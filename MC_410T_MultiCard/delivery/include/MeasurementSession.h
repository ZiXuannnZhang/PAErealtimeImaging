#pragma once

#include <QList>
#include <QString>

#include <functional>

// Small, dependency-free transaction engine shared by NetworkController and
// deterministic tests.  The engine owns ordering and rollback decisions; the
// controller supplies processor and socket operations.
class MeasurementSessionTransaction final
{
public:
    struct SendResult {
        int successCount = 0;
        int failCount = 0;
    };

    struct Result {
        bool success = false;
        int successCount = 0;
        int failCount = 0;
        int prepareSuccessCount = 0;
        int prepareFailCount = 0;
        int armSuccessCount = 0;
        int armFailCount = 0;
        QString reason;
        QList<QString> steps;
    };

    struct TeardownResult {
        bool success = false;
        bool hardwareStopSucceeded = false;
        int receiverSuccessCount = 0;
        int receiverFailCount = 0;
        int processorSuccessCount = 0;
        int processorFailCount = 0;
        QString reason;
    };

    using ProcessorStep = std::function<bool(int)>;
    using SendStep = std::function<SendResult()>;
    using RollbackStep = std::function<void()>;
    using StepObserver = std::function<void(const QString&)>;
    using AdmissionStep = std::function<bool()>;

    static Result start(int processorCount,
                        int targetCount,
                        const ProcessorStep &prepare,
                        const ProcessorStep &arm,
                        const SendStep &sendStart,
                        const RollbackStep &rollback,
                        const StepObserver &observe = {},
                        const AdmissionStep &commitAdmission = {});

    // Aggregates the bounded receiver/processor disarm barriers.  Every
    // callback is attempted so the caller receives a complete teardown
    // result even when one component has already failed.
    static TeardownResult teardown(int receiverCount,
                                   int processorCount,
                                   bool hardwareStopSucceeded,
                                   const ProcessorStep &disarmReceiver,
                                   const ProcessorStep &disarmProcessor);
};
