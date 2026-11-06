#pragma once
#include "okswitch/media/MiguSportsProvider.hpp"
#include <atomic>
#include <SDL2/SDL.h>

namespace okswitch::media {
struct MiguLoginView {
    std::string message;
    std::vector<unsigned char> qrJpeg;
    unsigned qrRevision=0;
    MiguSession session;
    bool running=false, authenticated=false;
};
class MiguLogin {
public:
    explicit MiguLogin(net::HttpClient& http);
    ~MiguLogin();
    void start();
    bool logout();
    void cancel();
    MiguLoginView snapshot() const;
    MiguSession savedSession() const { return saved_; }
private:
    static int entry(void* self);
    int run();
    void publish(const std::string& message, bool finished=false);
    net::HttpClient& http_;
    mutable SDL_mutex* mutex_=nullptr;
    SDL_Thread* thread_=nullptr;
    std::atomic<bool> cancel_{false};
    MiguLoginView view_;
    MiguSession saved_;
};
}
