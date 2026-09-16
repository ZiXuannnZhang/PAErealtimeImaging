#include "StartupPolicy.h"
#include <utility>

namespace {
StartupPolicy currentPolicy=StartupPolicy::Bypass;
std::string currentTrialId;
std::string currentCaptureChannel;
}

StartupPolicy startupPolicy(){return currentPolicy;}
const std::string& startupTrialId(){return currentTrialId;}
const std::string& systemCaptureChannelDir(){return currentCaptureChannel;}

void configureStartupDiagnostics(StartupPolicy policy,std::string trialId,std::string captureChannel){
    currentPolicy=policy;currentTrialId=std::move(trialId);currentCaptureChannel=std::move(captureChannel);
}
