#include "PaimageAcquisition/OutputWorkers.h"
#include <chrono>
#include <iostream>
#include <stdexcept>
using namespace paimage;
using namespace std::chrono_literals;
void require(bool p){if(!p)throw std::runtime_error("worker boundary check");}
template<class F> bool until(F f){auto end=std::chrono::steady_clock::now()+3s;while(!f()&&std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(1ms);return f();}
Frame frame(int c,int t){auto f=std::make_shared<CardFrame>();f->card=c;f->trigger=t;return f;}
int main(){
    {std::atomic<int> configured{0},consumed{0},closed{0};
        std::thread::id owner;bool sameThread=true;
        OutputWorkers outputs(1,2,[&](Frame){sameThread= sameThread && owner==std::this_thread::get_id();++consumed;},[](const SyncFrame&){});
        outputs.setSavingHooks({},[&]{sameThread= sameThread && owner==std::this_thread::get_id();++closed;});
        auto first=outputs.configureSaving(true,[&]{owner=std::this_thread::get_id();++configured;});
        outputs.start();
        require(until([&]{return outputs.savingConfigurationApplied(first);}));
        outputs.pushCard(frame(0,1));require(until([&]{return consumed.load()==1;}));
        auto second=outputs.configureSaving(false,[&]{sameThread= sameThread && owner==std::this_thread::get_id();++configured;});
        require(until([&]{return outputs.savingConfigurationApplied(second);}));
        outputs.pushCard(frame(0,2));outputs.stop();
        require(configured==2 && consumed==1 && closed==1 && sameThread);
    }
    {std::mutex mutex;std::condition_variable cv;bool release=false;
        std::atomic<unsigned> entered{0},consumed{0},full{0};
        OutputWorkers outputs(2,2,[&](Frame){++entered;std::unique_lock<std::mutex> lock(mutex);cv.wait(lock,[&]{return release;});++consumed;},
            [](const SyncFrame&){},[&](auto result,Frame){if(result==OutputWorkers::Result::CardFull)++full;});
        outputs.start();outputs.beginSession(1);outputs.setSavingEnabled(true);outputs.pushCard(frame(0,0));
        bool started=until([&]{return entered.load()==1;});
        if(started){for(int i=0;i<401;++i)outputs.pushCard(frame(0,i+1));outputs.pushCard(frame(1,999));outputs.beginSession(2);}
        {std::lock_guard<std::mutex> lock(mutex);release=true;}cv.notify_all();
        bool delivered=until([&]{return consumed.load()==402;});outputs.stop();
        require(started&&delivered&&full==1);require(outputs.isCurrentSession(2)&&!outputs.isCurrentSession(1));
    }
    {std::mutex mutex;std::condition_variable cv;bool release=false;
        std::atomic<unsigned> entered{0},consumed{0},evicted{0};std::vector<int> order;
        OutputWorkers outputs(1,2,[](Frame){},[&](const SyncFrame& f){
            ++entered;std::unique_lock<std::mutex> lock(mutex);cv.wait(lock,[&]{return release;});order.push_back(f.trigger);++consumed;
        },[&](auto r,Frame){if(r==OutputWorkers::Result::SyncEvicted)++evicted;});
        outputs.beginSession(1);outputs.start();outputs.pushSync(0,{frame(0,0)},false);
        bool started=until([&]{return entered.load()==1;});
        if(started){for(int t=1;t<=5;++t)outputs.pushSync(t,{frame(0,t)},false);outputs.pushSync(99,{frame(0,99)},true);}
        {std::lock_guard<std::mutex> lock(mutex);release=true;}cv.notify_all();
        bool delivered=until([&]{return consumed.load()==6;});outputs.stop();
        require(started&&delivered&&evicted==1);require(order==std::vector<int>({0,99,2,3,4,5}));
        require(outputs.syncPriorityError()==0);
    }
    std::cout<<"PASS worker boundaries: per-card saturation, measurement restart preservation, startup priority, whole-block eviction\n";
}
