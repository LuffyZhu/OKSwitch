#pragma once
#include <SDL2/SDL.h>
#include <atomic>
#include <unordered_map>
#include "okswitch/media/MediaSource.hpp"

namespace okswitch::media {
// One pending request: rapid navigation replaces work that has not started.
class CatalogLoader {
public:
    explicit CatalogLoader(MediaSource& source);
    ~CatalogLoader();
    void request(const std::string& endpoint);
    bool poll(SearchResponse& result);
    void shutdown();
private:
    static int entry(void* self);
    int run();
    MediaSource& source_;
    SDL_mutex* mutex_=nullptr;
    SDL_cond* condition_=nullptr;
    SDL_Thread* thread_=nullptr;
    bool stopping_=false, pending_=false, ready_=false;
    std::atomic<bool> cancel_{false};
    unsigned serial_=0;
    std::string endpoint_;
    SearchResponse result_;
    std::unordered_map<std::string,SearchResponse> cache_;
};
}
