#pragma once

#include <SDL2/SDL.h>
#include <atomic>
#include <string>
#include "okswitch/net/HttpClient.hpp"

namespace okswitch::net {
class OnlineUpdater {
public:
    OnlineUpdater(HttpClient& http,std::string proxy,std::string installedPath);
    ~OnlineUpdater();
    bool start();
    void cancel();
    bool done() const { return done_.load(); }
    int percent() const { return percent_.load(); }
    std::string message();
    void join();
private:
    static int entry(void* self);
    int run();
    void setMessage(const std::string& value);
    HttpClient& http_;
    std::string proxy_,installedPath_,message_;
    SDL_mutex* mutex_=nullptr;
    SDL_Thread* thread_=nullptr;
    std::atomic<bool> cancel_{false},done_{false};
    std::atomic<int> percent_{0};
};
}
