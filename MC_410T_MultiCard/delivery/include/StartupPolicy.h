#pragma once

#include <string>

// Startup admission policy selected on the command line. The diagnostic
// default is bypass: SourceCore keeps startupIdleMs=0, so confirmed_ is set
// immediately and the first trigger follows the normal ingestion path.
// Legacy restores the recovered 1000 ms startup cache/idle-filter setting
// and exists only for regression comparison of this experiment variable.
enum class StartupPolicy { Bypass, Legacy };

constexpr int startupIdleMsFor(StartupPolicy policy)
{
    return policy == StartupPolicy::Legacy ? 1000 : 0;
}

constexpr const char* startupPolicyName(StartupPolicy policy)
{
    return policy == StartupPolicy::Legacy ? "legacy" : "bypass";
}

inline bool parseStartupPolicy(const std::string& value, StartupPolicy* out)
{
    if (value == "bypass") { if (out) *out = StartupPolicy::Bypass; return true; }
    if (value == "legacy") { if (out) *out = StartupPolicy::Legacy; return true; }
    return false;
}

// Process-wide values parsed once in main(). Implemented in main.cpp so both
// the UI configuration path and the diagnostic identity read one source.
StartupPolicy        startupPolicy();
const std::string&   startupTrialId();
const std::string&   systemCaptureChannelDir();
void configureStartupDiagnostics(StartupPolicy,std::string trialId,std::string captureChannel);
