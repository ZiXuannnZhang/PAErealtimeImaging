#pragma once
#include "FrameConverter.h"
#include "OutputWorkers.h"
#include "TraceWriter.h"
#include "TimingWriter.h"
#include "DataProcessor.h"
#include "FileSaver.h"
namespace paimage {
// Lifetimes: processors/savers and trace outlive this adapter. Their QThreads
// remain unstarted; only the recovered two output workers invoke delivery.
class HostOutput {
public:
    HostOutput(int bits,int blockSize,std::vector<DataProcessor*>,std::vector<FileSaver*>,TraceWriter*,TimingWriter* = nullptr);
    ~HostOutput();
    void start(){workers_.start();}
    void stop(){workers_.stop();}
    void requestStop(){workers_.requestStop();}
    void beginSession(std::uint64_t s){workers_.beginSession(s);}
    void card(Frame f);
    void sync(std::uint16_t,const std::vector<Frame>&,bool startup);
    std::uint64_t startSaving(const QString&,int,const QString&);
    std::uint64_t stopSaving();
    void prepareConfigurationRestart(){configurationRestart_=true;}
    std::uint64_t resumeSaving();
    bool savingApplied(std::uint64_t g)const{return workers_.savingConfigurationApplied(g);}
    void requestClose();
    unsigned cardDepth(int card){return workers_.cardDepth(card);}
    OutputWorkers::Snapshot stats() const noexcept{return workers_.snapshot();}
private:
    void consumeCard(Frame);void consumeSync(const SyncFrame&);
    void observe(Frame,std::uint8_t stage,std::uint8_t reason,std::uint32_t value=0);
    std::vector<DataProcessor*> processors_;std::vector<FileSaver*> savers_;
    TraceWriter* trace_;TimingWriter* timing_;FrameConverter converter_;OutputWorkers workers_;
    std::atomic<bool> configurationRestart_{false};
};
}
