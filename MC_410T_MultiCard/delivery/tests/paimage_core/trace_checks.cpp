#include "PaimageAcquisition/TraceWriter.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace paimage;
int main(){try{
    std::filesystem::path root="trace-fault-checks";std::filesystem::create_directories(root);
    {TraceWriter w(root/"queue-and-budget",{8,256,2048});for(int i=0;i<1000000;++i){TraceRecord r;r.value=i;w.push(r);}w.stop();if(!w.incomplete()||!w.dropped())throw std::runtime_error("queue/budget fault not observed");}
    {std::ofstream(root/"regular-file")<<"this is not a directory";TraceWriter w(root/"regular-file"/"child");TraceRecord r;w.push(r);w.stop();if(!w.incomplete())throw std::runtime_error("write error hidden");}
    {TraceWriter w(root/"rotation",{1024,256,65536});for(int i=0;i<100;++i){TraceRecord r;r.value=i;w.push(r);}w.stop();if(w.incomplete())throw std::runtime_error("unexpected rotation loss");}
    std::cout<<"PASS trace queue saturation, budget exhaustion, write failure and rotation\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
