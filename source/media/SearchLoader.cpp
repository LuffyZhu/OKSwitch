#include "okswitch/media/SearchLoader.hpp"

#include <algorithm>

namespace okswitch::media {

SearchLoader::SearchLoader(MediaSource& source):mediaSource_(source),mutex_(SDL_CreateMutex()){}
SearchLoader::~SearchLoader(){shutdown();}

void SearchLoader::joinCompleted(){
    for(auto& worker:workers_){
        if(worker->thread){SDL_WaitThread(worker->thread,nullptr);worker->thread=nullptr;}
    }
    workers_.clear();
}

bool SearchLoader::request(const std::vector<SourceDefinition>& sources,const std::string& keyword){
    if(!mutex_||sources.empty()||keyword.empty())return false;
    SDL_LockMutex(mutex_);const bool busy=active_;SDL_UnlockMutex(mutex_);
    if(busy)return false;
    joinCompleted();
    SDL_LockMutex(mutex_);
    cancel_.store(false);finished_=0;nextSource_=0;activeWorkers_=0;resultCount_=0;ready_=false;active_=true;progress_={};progress_.total=sources.size();
    queuedSources_=sources;keyword_=keyword;
    // Real hardware has enough network/thread capacity for several independent
    // providers. Four workers cut the 96-source scan substantially without
    // opening an unbounded number of TLS sessions.
    const size_t workerCount=std::min<size_t>(4,sources.size());
    for(size_t i=0;i<workerCount;++i){
        auto worker=std::make_unique<Worker>();worker->owner=this;
        workers_.push_back(std::move(worker));
    }
    for(auto& worker:workers_){
        worker->thread=SDL_CreateThread(entry,"source-search",worker.get());
        if(worker->thread)++activeWorkers_;
        else progress_.error+=(progress_.error.empty()?"":" | ")+std::string("无法启动搜索线程");
    }
    progress_.finished=finished_;
    if(activeWorkers_==0){active_=false;ready_=true;progress_.done=true;}
    SDL_UnlockMutex(mutex_);return true;
}

int SearchLoader::entry(void* context){
    auto& worker=*static_cast<Worker*>(context);return worker.owner->run(worker);
}

int SearchLoader::run(Worker& worker){
    (void)worker;
    for(;;){
        SDL_LockMutex(mutex_);
        if(stopping_||cancel_.load()||nextSource_>=queuedSources_.size()){SDL_UnlockMutex(mutex_);break;}
        SourceDefinition source=queuedSources_[nextSource_++];
        const std::string keyword=keyword_;
        SDL_UnlockMutex(mutex_);

        auto response=mediaSource_.search(source,keyword,&cancel_);
        SDL_LockMutex(mutex_);
        if(!stopping_&&!cancel_.load()){
            if(response.succeeded){
                constexpr size_t maxAggregateResults=384;
                const size_t available=resultCount_<maxAggregateResults
                    ? maxAggregateResults-resultCount_:0;
                const size_t take=std::min(available,response.items.size());
                progress_.items.insert(progress_.items.end(),response.items.begin(),response.items.begin()+take);
                resultCount_+=take;
                progress_.sourceStatus.emplace_back(source.name,response.items.empty()?"网站没有返回搜索结果":"找到 "+std::to_string(response.items.size())+" 个资源");
            }
            else {
                progress_.error+=(progress_.error.empty()?"":" | ")+response.error;
                progress_.sourceStatus.emplace_back(source.name,response.error);
            }
            ++finished_;progress_.finished=finished_;ready_=true;
        }
        SDL_UnlockMutex(mutex_);
    }
    SDL_LockMutex(mutex_);
    if(activeWorkers_>0)--activeWorkers_;
    if(activeWorkers_==0){active_=false;progress_.done=true;ready_=true;}
    SDL_UnlockMutex(mutex_);return 0;
}

bool SearchLoader::poll(SearchProgress& progress){
    if(!mutex_)return false;
    SDL_LockMutex(mutex_);const bool ready=ready_;
    if(ready){
        progress.items.swap(progress_.items);
        progress.sourceStatus.swap(progress_.sourceStatus);
        progress.error=progress_.error;
        progress.finished=progress_.finished;
        progress.total=progress_.total;
        progress.done=progress_.done;
        ready_=false;
    }
    SDL_UnlockMutex(mutex_);return ready;
}

void SearchLoader::cancel(){
    // Detail and cloud requests must not overlap an unfinished provider TLS
    // handshake on Horizon/Ryujinx.  Signal curl first, then reclaim every
    // worker before the caller starts the next network stage.
    cancel_.store(true);
    joinCompleted();
    if(!mutex_)return;
    SDL_LockMutex(mutex_);
    active_=false;activeWorkers_=0;ready_=false;
    progress_.items.clear();queuedSources_.clear();keyword_.clear();
    SDL_UnlockMutex(mutex_);
}

void SearchLoader::shutdown(){
    if(!mutex_)return;
    cancel_.store(true);SDL_LockMutex(mutex_);stopping_=true;SDL_UnlockMutex(mutex_);
    joinCompleted();SDL_DestroyMutex(mutex_);mutex_=nullptr;
}
}
