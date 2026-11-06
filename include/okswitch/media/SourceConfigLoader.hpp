#pragma once

#include <SDL2/SDL.h>
#include <atomic>
#include <string>
#include <vector>

#include "okswitch/media/MediaSource.hpp"
#include "okswitch/net/HttpClient.hpp"

namespace okswitch::media {

struct SourceConfigResult {
    bool succeeded=false;
    std::string error;
    std::vector<SourceDefinition> sources;
    std::vector<std::string> footballSources;
    size_t totalSites=0;
};

// Loads the online TVBox-style configuration without blocking the UI. Only
// entries with a registered native implementation are returned.
class SourceConfigLoader {
public:
    explicit SourceConfigLoader(net::HttpClient& httpClient);
    ~SourceConfigLoader();
    void request(const std::string& url);
    bool poll(SourceConfigResult& result);
    void shutdown();

private:
    static int entry(void* self);
    int run();
    SourceConfigResult load(const std::string& url);

    net::HttpClient& httpClient_;
    SDL_mutex* mutex_=nullptr;
    SDL_cond* condition_=nullptr;
    SDL_Thread* thread_=nullptr;
    bool stopping_=false, pending_=false, ready_=false;
    size_t requestId_=0;
    std::atomic<bool> cancel_{false};
    std::string url_;
    SourceConfigResult result_;
};

}
