#include "build_task.hpp"
BuildTask::BuildTask(QObject* parent):QObject(parent){
    timer_.setInterval(25);connect(&timer_,&QTimer::timeout,this,[this]{
        if(!busy())return;std::string phase;{std::lock_guard lock(shared_->mutex);phase=shared_->phase;}
        if(updated)updated(phase);
        if(future_.wait_for(std::chrono::milliseconds(0))==std::future_status::ready){timer_.stop();auto outcome=future_.get();if(finished)finished(outcome);}
    });
}
BuildTask::~BuildTask(){timer_.stop();if(busy()){cancel();future_.wait();}}
bool BuildTask::start(std::filesystem::path root,std::filesystem::path database,codeguard::BuildOptions options){
    if(busy())return false;
    auto shared=std::make_shared<Shared>();options.control=shared->control;
    const auto observer=options.progress;
    options.progress=[shared,observer](const auto& phase){{std::lock_guard lock(shared->mutex);shared->phase=phase;}if(observer)observer(phase);};
    future_=std::async(std::launch::async,[root,database,options,shared]{
        BuildOutcome outcome;try{outcome.result=codeguard::build_and_test(root,database,options);}catch(const std::exception&e){outcome.error=e.what();}
        shared->control->finish();return outcome;
    });
    shared_=shared;timer_.start();if(updated)updated("prepare");return true;
}
bool BuildTask::cancel(){return busy()&&shared_->control->request_cancel();}
