#pragma once

#include <SDL2/SDL.h>
#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "okswitch/media/MediaSource.hpp"

namespace okswitch::media {

struct SearchProgress {
    std::vector<SearchResult> items;
    std::vector<std::pair<std::string,std::string>> sourceStatus;
    std::string error;
    size_t finished=0;
    size_t total=0;
    bool done=false;
};

// Searches all enabled native providers concurrently. Workers publish an
// incremental batch whenever one provider finishes, so large result sets are
// moved to the UI instead of repeatedly copied between threads.
class SearchLoader {
public:
    explicit SearchLoader(MediaSource& source);
    ~SearchLoader();
    bool request(const std::vector<SourceDefinition>& sources,const std::string& keyword);
    bool poll(SearchProgress& progress);
    void cancel();
    void shutdown();

private:
    struct Worker {
        SearchLoader* owner=nullptr;
        SDL_Thread* thread=nullptr;
    };
    static int entry(void* context);
    int run(Worker& worker);
    void joinCompleted();

    MediaSource& mediaSource_;
    SDL_mutex* mutex_=nullptr;
    std::vector<std::unique_ptr<Worker>> workers_;
    std::atomic<bool> cancel_{false};
    bool stopping_=false,active_=false,ready_=false;
    size_t finished_=0,nextSource_=0,activeWorkers_=0,resultCount_=0;
    std::vector<SourceDefinition> queuedSources_;
    std::string keyword_;
    SearchProgress progress_;
};

}
