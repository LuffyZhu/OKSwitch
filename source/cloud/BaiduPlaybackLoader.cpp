#include "okswitch/cloud/BaiduPlaybackLoader.hpp"

namespace okswitch::cloud {
BaiduPlaybackLoader::BaiduPlaybackLoader(QuarkProvider& quark,BaiduProvider& baidu):quark_(quark),baidu_(baidu){mutex_=SDL_CreateMutex();}
BaiduPlaybackLoader::~BaiduPlaybackLoader(){shutdown();}
bool BaiduPlaybackLoader::request(const ShareSession& session,const CloudEntry& file,bool baidu){
    if(!mutex_)return false;
    SDL_LockMutex(mutex_);const bool busy=running_;SDL_UnlockMutex(mutex_);if(busy)return false;
    if(thread_){SDL_WaitThread(thread_,nullptr);thread_=nullptr;}
    if(baidu)baidu_.resetPlaybackCancel();else quark_.resetPlaybackCancel();SDL_LockMutex(mutex_);session_=session;file_=file;useBaidu_=baidu;result_={};ready_=false;cancelled_=false;running_=true;SDL_UnlockMutex(mutex_);
    thread_=SDL_CreateThread(entry,"baidu-play-url",this);if(!thread_){SDL_LockMutex(mutex_);running_=false;SDL_UnlockMutex(mutex_);return false;}return true;
}
bool BaiduPlaybackLoader::poll(ProviderResult<PlaybackSource>& result){if(!mutex_)return false;SDL_LockMutex(mutex_);const bool ready=ready_;if(ready){result=std::move(result_);ready_=false;}SDL_UnlockMutex(mutex_);if(ready&&thread_){SDL_WaitThread(thread_,nullptr);thread_=nullptr;}return ready;}
void BaiduPlaybackLoader::cancel(){if(!mutex_)return;SDL_LockMutex(mutex_);cancelled_=true;const bool baidu=useBaidu_;SDL_UnlockMutex(mutex_);if(baidu)baidu_.cancelPlayback();else quark_.cancelPlayback();}
bool BaiduPlaybackLoader::running(){if(!mutex_)return false;SDL_LockMutex(mutex_);bool value=running_;SDL_UnlockMutex(mutex_);return value;}
void BaiduPlaybackLoader::shutdown(){cancel();if(thread_){SDL_WaitThread(thread_,nullptr);thread_=nullptr;}if(mutex_){SDL_DestroyMutex(mutex_);mutex_=nullptr;}}
int BaiduPlaybackLoader::entry(void* self){return static_cast<BaiduPlaybackLoader*>(self)->run();}
int BaiduPlaybackLoader::run(){auto result=useBaidu_?baidu_.getPlaybackSource(session_,file_):quark_.getPlaybackSource(session_,file_);SDL_LockMutex(mutex_);if(cancelled_){result.succeeded=false;result.error={ProviderErrorCode::NetworkError,"已取消播放地址解析"};}result_=std::move(result);running_=false;ready_=true;SDL_UnlockMutex(mutex_);return 0;}
}
