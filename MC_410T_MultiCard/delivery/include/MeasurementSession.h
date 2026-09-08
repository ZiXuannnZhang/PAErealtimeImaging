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
        QString reason;
        QList<QString> steps;
    };

    using ProcessorStep = std::function<bool(int)>;
    using SendStep = std::function<SendResult()>;
    using RollbackStep = std::function<void()>;
    using StepObserver = std::function<void(const QString&)>;

    static Result start(int processorCount,
                        int targetCount,
                        const ProcessorStep &prepare,
                        const ProcessorStep &arm,
                        const SendStep &sendStart,
                        const RollbackStep &rollback,
                        const StepObserver &observe = {});
};
