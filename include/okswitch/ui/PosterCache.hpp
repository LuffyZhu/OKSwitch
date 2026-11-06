#pragma once

#include <SDL2/SDL.h>
#include <string>
#include <deque>
#include <atomic>
#include <memory>
#include <unordered_map>

#include "okswitch/net/HttpClient.hpp"

namespace okswitch::ui
{
class PosterCache
{
public:
    PosterCache(net::HttpClient& httpClient, SDL_Renderer* renderer);
    ~PosterCache();
    void beginFrame(int loadBudget = 2);
    SDL_Texture* get(const std::string& url,const std::string& proxyUrl={});
    void clear();
    void setRenderer(SDL_Renderer* renderer);
    void shutdown();
    std::string error(const std::string& url) const;

private:
    struct Job { std::string url; unsigned generation; std::string proxyUrl;
        std::shared_ptr<std::atomic<bool>> cancel; };
    struct Image {
        Job job;
        int width=0, height=0;
        std::vector<unsigned char> pixels;
        std::string error;
    };
    bool load(const std::string& url, Image& image);
    static int workerEntry(void* self);
    int workerLoop();
    SDL_mutex* mutex_=nullptr;
    SDL_mutex* diskMutex_=nullptr;
    SDL_cond* condition_=nullptr;
    std::vector<SDL_Thread*> workers_;
    std::atomic<bool> stopping_{false};
    unsigned generation_=0;
    std::shared_ptr<std::atomic<bool>> generationCancel_=std::make_shared<std::atomic<bool>>(false);
    std::deque<Job> jobs_;
    std::deque<Image> completed_;
    net::HttpClient& httpClient_;
    SDL_Renderer* renderer_;
    std::unordered_map<std::string, SDL_Texture*> textures_;
    std::unordered_map<std::string, std::string> errors_;
    std::unordered_map<std::string, Uint32> retryAt_;
    int loadBudget_ = 0;
};
}
