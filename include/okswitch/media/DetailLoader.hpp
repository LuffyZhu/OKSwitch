#pragma once

#include <atomic>
#include <SDL2/SDL.h>
#include "okswitch/media/MediaSource.hpp"

namespace okswitch::media
{
class DetailLoader
{
public:
    explicit DetailLoader(MediaSource& source);
    ~DetailLoader();
    bool request(const SearchResult& item);
    bool poll(SearchResult& item, DetailResponse& result);
    void cancel();
    void shutdown();

private:
    static int entry(void* self);
    int run();
    MediaSource& source_;
    SDL_mutex* mutex_ = nullptr;
    SDL_cond* condition_ = nullptr;
    SDL_Thread* thread_ = nullptr;
    bool stopping_ = false, pending_ = false, ready_ = false, working_ = false;
    std::atomic<bool> cancel_{false};
    unsigned serial_ = 0;
    SearchResult requested_, completed_;
    DetailResponse result_;
};
}
