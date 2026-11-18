#include "PaimageAcquisition/LoopLog.h"
#include "StartupPolicy.h"
#include <filesystem>
#include <future>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace paimage;

namespace {
void check(bool ok,const char* label){if(!ok)throw std::runtime_error(label);}

bool containsKind(const std::filesystem::path& root,LoopKind kind){
    for(const auto& entry:std::filesystem::directory_iterator(root)){
        if(!entry.is_regular_file()||entry.path().extension()!=".bin")continue;
        std::ifstream input(entry.path(),std::ios::binary);LoopRecord record;
        while(input.read(reinterpret_cast<char*>(&record),sizeof(record)))
            if(record.kind==std::uint16_t(kind))return true;
    }
    return false;
}
}

int main(){try{
    StartupPolicy policy{};
    check(parseStartupPolicy("bypass",&policy)&&policy==StartupPolicy::Bypass,"parse bypass");
    check(startupIdleMsFor(policy)==0,"bypass uses zero startup idle");
    check(parseStartupPolicy("legacy",&policy)&&policy==StartupPolicy::Legacy,"parse legacy");
    check(startupIdleMsFor(policy)==1000,"legacy restores 1000 ms");
    check(!parseStartupPolicy("unknown",&policy),"reject unknown startup policy");

    const auto root=std::filesystem::current_path()/"looplog-check-output";
    std::filesystem::remove_all(root);
    {
        LoopLog log(root/"burst");
        constexpr std::uint64_t start=1000000000ull;
        log.noteThreadStart(start);
        log.noteSampleData(start+1000000ull);
        check(log.freezeEpoch()==0,"short idle is not a burst");
        log.noteSampleData(start+2100000000ull);
        check(log.freezeEpoch()==1,"two-second idle creates burst mark");
        log.noteSampleData(start+2101000000ull);
        check(log.freezeEpoch()==1,"continuous input does not create a second mark");
        log.stop();
        check(!log.incomplete(),"normal loop log remains complete");
    }
    check(std::filesystem::exists(root/"burst"/"looplog-summary.json"),"loop summary written");
    check(containsKind(root/"burst",LoopKind::BurstMark),"burst mark persisted");

    {
        std::promise<void> drainEntered, releaseDrain;
        auto entered=drainEntered.get_future();
        auto release=releaseDrain.get_future();
        LoopLog::Budget budget;
        budget.records=8192;
        budget.beforeDrain=[&]{
            if (release.valid()) {
                drainEntered.set_value();
                release.get();
            }
        };
        const auto backlog=root/"export-backlog";
        LoopLog log(backlog,budget);
        entered.wait();
        LoopRecord record;
        record.kind=std::uint16_t(LoopKind::Loop);
        record.timeNs=1000000;
        log.push(record);
        const auto windowStart=record.timeNs+5000000000ull;
        auto burst=record;
        burst.timeNs=windowStart;
        burst.kind=std::uint16_t(LoopKind::BurstMark);
        log.push(burst);
        record.timeNs=windowStart+1000000;
        for (int i=2;i<4096;++i) log.push(record);
        const auto cut=LoopLog::captureCuts().front();
        auto outside=record;
        outside.timeNs=windowStart+5000000001ull;
        log.push(outside);
        record.value0=0x12345678;
        const auto tailSequence=log.push(record);
        releaseDrain.set_value();
        check(cut.sequence==4096&&tailSequence==4098&&cut.flush(),"export acknowledges exactly the queued click cut");
        const auto frozen=backlog/"looplog-window-1.bin";
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while (!std::filesystem::exists(frozen)&&std::chrono::steady_clock::now()<deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        log.stop();
        check(std::filesystem::file_size(frozen)==4097*sizeof(LoopRecord),
              "expired window retains queued in-window record beyond the export cut");
        std::ifstream input(frozen,std::ios::binary);
        input.seekg(4096*sizeof(LoopRecord));
        input.read(reinterpret_cast<char*>(&record),sizeof(record));
        check(input&&record.sequence==tailSequence&&record.value0==0x12345678,"frozen file preserves the queued tail");
        check(cut.windowInventory().find("\"postWindowTruncated\":false")!=std::string::npos,
              "drained window does not report a truncated tail");
    }

    {
        LoopLog::Budget budget;budget.records=64;budget.segmentBytes=320;budget.totalBytes=640;
        LoopLog log(root/"budget",budget);
        for(int i=0;i<4096;++i){LoopRecord record;record.kind=std::uint16_t(LoopKind::Loop);record.timeNs=std::uint64_t(i+1);log.push(record);}
        log.stop();
        check(log.incomplete(),"budget or queue exhaustion is explicit");
    }
    check(std::filesystem::exists(root/"budget"/"looplog-summary.json"),"incomplete summary written");
    std::filesystem::remove_all(root);
    std::cout<<"PASS: startup policy and bounded loop log checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
