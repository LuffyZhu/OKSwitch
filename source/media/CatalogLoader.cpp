#include "okswitch/media/CatalogLoader.hpp"
namespace okswitch::media {
CatalogLoader::CatalogLoader(MediaSource& source):source_(source) {
    mutex_=SDL_CreateMutex();condition_=SDL_CreateCond();
    if(mutex_&&condition_)thread_=SDL_CreateThread(entry,"catalog",this);
}
CatalogLoader::~CatalogLoader(){shutdown();}
void CatalogLoader::shutdown(){
    if(mutex_){
        SDL_LockMutex(mutex_);stopping_=true;cancel_.store(true);
        if(condition_)SDL_CondSignal(condition_);
        SDL_UnlockMutex(mutex_);
    }
    if(thread_){SDL_WaitThread(thread_,nullptr);thread_=nullptr;}
    if(condition_){SDL_DestroyCond(condition_);condition_=nullptr;}
    if(mutex_){SDL_DestroyMutex(mutex_);mutex_=nullptr;}
}
void CatalogLoader::request(const std::string& endpoint){
    if(!mutex_)return;
    SDL_LockMutex(mutex_);
    ++serial_;endpoint_=endpoint;cancel_.store(true);ready_=false;pending_=false;
    const auto found=cache_.find(endpoint);
    if(found!=cache_.end()){result_=found->second;ready_=true;}
    else if(!thread_){result_={};result_.error="Background catalog unavailable";ready_=true;}
    else{pending_=true;SDL_CondSignal(condition_);}
    SDL_UnlockMutex(mutex_);
}
bool CatalogLoader::poll(SearchResponse& result){
    if(!mutex_){result={};result.error="Catalog initialization failed";return true;}
    SDL_LockMutex(mutex_);
    bool ready=ready_;
    if(ready){result=std::move(result_);ready_=false;}
    SDL_UnlockMutex(mutex_);return ready;
}
int CatalogLoader::entry(void* self){return static_cast<CatalogLoader*>(self)->run();}
int CatalogLoader::run(){
    for(;;){
        SDL_LockMutex(mutex_);
        while(!pending_&&!stopping_)SDL_CondWait(condition_,mutex_);
        if(stopping_){SDL_UnlockMutex(mutex_);return 0;}
        const auto endpoint=endpoint_;const unsigned serial=serial_;
        pending_=false;cancel_.store(false);SDL_UnlockMutex(mutex_);
        auto result=source_.catalog(endpoint,&cancel_);
        SDL_LockMutex(mutex_);
        if(result.succeeded){
            if(cache_.size()>=16)cache_.erase(cache_.begin());
            cache_[endpoint]=result;
        }
        if(!stopping_&&serial==serial_){result_=std::move(result);ready_=true;}
        SDL_UnlockMutex(mutex_);
    }
}
}
