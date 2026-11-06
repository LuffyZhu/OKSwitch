#pragma once
#include <atomic>
#include <string>
#include <SDL2/SDL.h>

namespace okswitch::net {
// One-use local HTTP form. The listener exists only while the QR screen is open.
class LanTextInput {
public:
    explicit LanTextInput(bool oauthConfig=false):oauthConfig_(oauthConfig){}
    ~LanTextInput(){stop();}
    bool start(std::string& error);
    void stop();
    bool completed() const{return completed_.load(std::memory_order_acquire);}
    const std::string& value() const{return value_;}
    const std::string& url() const{return url_;}
private:
    static int entry(void* context);
    void serve(int client);
    int listener_=-1;
    SDL_Thread* thread_=nullptr;
    std::atomic<bool> stopping_{false},completed_{false};
    std::string token_,url_,value_;
    bool oauthConfig_=false;
};
}
