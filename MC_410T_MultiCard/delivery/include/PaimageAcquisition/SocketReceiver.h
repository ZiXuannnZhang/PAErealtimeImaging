#pragma once
#include "SourceCore.h"
#include "TraceWriter.h"
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
namespace paimage {
// Windows-only recovered receive scheduling and feedback demultiplexing.
// Control transaction ownership is deliberately separate from the raw socket loop.
class SocketReceiver {
public:
    struct Endpoint {std::uint16_t port=0;std::string bindIp;};
    SocketReceiver(Config,std::vector<Endpoint>,Endpoint feedback,std::vector<std::string> targets,
        TraceWriter*,SourceCore::CardSink,SourceCore::SyncSink);
    ~SocketReceiver();
    bool start(std::string& error);
    void stop();
    void requestStop(){running_=false;}
    void observeShutdown(){std::lock_guard<std::mutex> lock(coreMutex_);core_.observeShutdown(now());}
    void prepareStart(std::uint64_t session);
    void completeStart(bool success);
    void prepareStop();
    void completeStop(bool success);
    std::vector<int> receiveBuffers() const{return receiveBuffers_;}
    int feedbackReceiveBuffer()const{return feedbackReceiveBuffer_;}
    std::uint64_t ingress() const{return ingress_.load();}
    std::uint64_t hardErrors() const{return hardErrors_.load();}
    bool isRunning()const{return running_.load();}
    int lastSocketError()const{return lastSocketError_.load();}
    int priorityResult()const{return priorityResult_.load();}
    int actualPriority()const{return actualPriority_.load();}
    std::function<void(int,int)> feedbackSink;
    SourceCore::Observer observationSink;
    std::function<void(int,const TraceRecord&)> ingressSink;
    Counters counters()const{std::lock_guard<std::mutex> lock(coreMutex_);return core_.counters();}
    static Time now();
private:
    void run();void closeSockets();
    Config config_;std::vector<Endpoint> endpoints_;Endpoint feedback_;
    std::vector<std::string> targets_;std::vector<std::uint32_t> targetAddresses_;
    std::vector<std::uintptr_t> sockets_;std::vector<int> receiveBuffers_;
    int feedbackReceiveBuffer_=-1;
    std::uintptr_t feedbackSocket_=~std::uintptr_t(0);
    TraceWriter* trace_;SourceCore core_;mutable std::mutex coreMutex_;
    std::atomic<bool> running_{false};std::atomic<std::uint64_t> session_{0},ingress_{0},hardErrors_{0};
    std::atomic<int> priorityResult_{-1},actualPriority_{-1};std::thread worker_;
    std::atomic<int> lastSocketError_{0};
    std::uint64_t correlation_=0;bool wsa_=false;
};
}
