#pragma once
#include "okswitch/net/HttpClient.hpp"
#include "okswitch/cloud/QuarkSession.hpp"
#include <atomic>
namespace okswitch::cloud {
struct LoginView {
    std::string message="准备扫码登录";
    std::vector<unsigned char> modules;
    int size=0;
    bool running=false, authenticated=false;
    QuarkSession session;
};
class QuarkLogin {
public:
    explicit QuarkLogin(net::HttpClient& http);
    ~QuarkLogin();
    void start();
    bool logout();
    void cancel();
    LoginView snapshot();
    void shutdown();
private:
    static int entry(void* self);
    int run();
    void publish(const std::string& message,bool finished=false);
    bool store(const QuarkSession& session);
    net::HttpClient& http_;
    SDL_mutex* mutex_=nullptr;
    SDL_Thread* thread_=nullptr;
    std::atomic<bool> cancel_{false};
    LoginView view_;
    QuarkSession saved_;
};
}
