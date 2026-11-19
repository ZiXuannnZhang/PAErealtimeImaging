#include "TimeoutPresentation.h"
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>
void check(bool b,const char* message){if(!b)throw std::runtime_error(message);}
int main(){
    using namespace paimage;
    TimeoutPresentation presentation;
    presentation.configure(true,"new-ui-directory/recon_png","suffix");
    auto pixels=std::make_shared<int>(42);
    std::weak_ptr<int> lifetime=pixels;
    std::vector<int> order{1}; // TimeoutBoundary / binding already committed.
    presentation.publish(7,0,{sizeof(int), "_frame1", [pixels,&order] {
        check(*pixels==42,"C10 old pixels owned after live presentation destruction");
        order.push_back(3);
        return std::array<QImage, 2>{};
    }});
    pixels.reset();
    auto job=presentation.close(7,1,true,"old-directory",[&]{
        // close removed live writer, so ONLY the captured job can own pixels.
        check(!lifetime.expired(),"C10 payload captured BEFORE Ring/CUDA reset");
        check(!presentation.accepts(7,0)&&presentation.accepts(7,1),"C10 delayed old UI conversion rejected");
        order.push_back(2);
    });
    check(job && job.target.directory=="old-directory/recon_png" && job.target.suffix=="suffix",
          "C10 committed old directory");
    job.capture.render();
    check(order==std::vector<int>({1,2,3}),"C10 boundary -> capture -> reset -> render");
    job.capture.render={};
    check(lifetime.expired(),"C10 full-size payload released before asynchronous write");
    const TimeoutPresentation::Capture emptyPair{1, "_frame2", [] { return std::array<QImage, 2>{}; }};
    presentation.publish(7,1,emptyPair);
    job=presentation.close(7,2,false,{},[]{});
    check(job&&job.target.directory=="new-ui-directory/recon_png","C10 manual directory unchanged");
    presentation.publish(7,2,emptyPair);
    check(!presentation.close(7,3,true,{},[]{}),"C10 missing auto directory cannot use new UI target");
    RingRoundUiState ui;std::atomic<bool> blocked{false};
    ui.recordSubmitIndex(5);ui.onTimeoutBoundary();
    TimeoutPresentation::applyResetResult(true,5,ui,blocked);
    check(!blocked && !ui.admitSnapshot(5) && ui.admitSnapshot(6),"C12 success establishes producer cutoff");
    ui.onTimeoutBoundary();TimeoutPresentation::applyResetResult(false,0,ui,blocked);
    check(blocked && !ui.snapshot().hasStaleCutoff,"C12 failed reset blocks snapshot admission");
    TimeoutPresentation::applyResetResult(true,9,ui,blocked);
    check(blocked,"C12 later success cannot clear lifecycle fail-closed latch");
    std::cout<<"PASS C10 capture-before-reset/old-directory/lifetime; C12 reset success/failure\n";
}
