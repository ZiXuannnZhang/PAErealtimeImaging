#include "StartupPolicy.h"
#include <utility>

namespace {
StartupPolicy currentPolicy=StartupPolicy::Bypass;
std::string currentTrialId;
}

StartupPolicy startupPolicy(){return currentPolicy;}
const std::string& startupTrialId(){return currentTrialId;}

void configureStartupDiagnostics(StartupPolicy policy,std::string trialId){
    currentPolicy=policy;currentTrialId=std::move(trialId);
}
