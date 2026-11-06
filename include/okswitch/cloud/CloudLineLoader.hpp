#pragma once

#include <SDL2/SDL.h>
#include <string>
#include <vector>

#include "okswitch/cloud/BaiduProvider.hpp"
#include "okswitch/cloud/QuarkProvider.hpp"

namespace okswitch::cloud {
struct CloudLineResult {
    bool succeeded=false;
    bool baidu=false;
    size_t index=0;
    std::string error;
    ShareSession session;
    std::vector<CloudEntry> items;
    std::vector<std::string> directories;
};

class CloudLineLoader {
public:
    CloudLineLoader(QuarkProvider& quark,BaiduProvider& baidu);
    ~CloudLineLoader();
    bool request(const std::string& url,size_t index);
    bool poll(CloudLineResult& result);
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
    std::string url_;
    size_t index_=0;
    CloudLineResult result_;
    bool running_=false,ready_=false,cancelled_=false;
};
}
