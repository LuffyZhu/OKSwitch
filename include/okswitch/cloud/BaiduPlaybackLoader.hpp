#pragma once

#include <SDL2/SDL.h>
#include "okswitch/cloud/BaiduProvider.hpp"
#include "okswitch/cloud/QuarkProvider.hpp"

namespace okswitch::cloud {
class BaiduPlaybackLoader {
public:
    BaiduPlaybackLoader(QuarkProvider& quark,BaiduProvider& baidu);
    ~BaiduPlaybackLoader();
    bool request(const ShareSession& session,const CloudEntry& file,bool baidu);
    bool poll(ProviderResult<PlaybackSource>& result);
    void cancel();
    bool running();
    void shutdown();
private:
    static int entry(void* self);
    int run();
    QuarkProvider& quark_;
    BaiduProvider& baidu_;
    SDL_mutex* mutex_=nullptr;
    SDL_Thread* thread_=nullptr;
    ShareSession session_;
    CloudEntry file_;
    ProviderResult<PlaybackSource> result_;
    bool useBaidu_=false,running_=false,ready_=false,cancelled_=false;
};
}
