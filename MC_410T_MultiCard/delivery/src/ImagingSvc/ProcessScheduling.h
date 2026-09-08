#pragma once

#include <QString>
#include <QtGlobal>

#include <cstdint>

namespace ProcessScheduling {

// Pure mask decision used by deterministic tests. processMask is the
// current process affinity, systemMask is the machine's usable mask.
struct AffinityDecision {
    quint64 originalMask = 0;
    quint64 requestedMask = 0;
    quint64 systemMask = 0;
    bool shouldApply = false;
    QString reason;
};

AffinityDecision computeRequestedAffinity(quint64 processMask,
                                          quint64 systemMask);

struct ApplyResult {
    QString logLine;
    QString requestedPriority;
    QString priorityApply;
    QString effectivePriority;
    QString originalProcessMask;
    QString requestedProcessMask;
    QString affinityApply;
    QString effectiveProcessMask;
    bool priorityApplied = false;
    bool affinityApplied = false;
};

// Applies only the experiment's process-level scheduling policy. A failure
// is returned as structured status and never prevents ImagingSvc startup.
ApplyResult applyIngressProtectionScheduling();

} // namespace ProcessScheduling
