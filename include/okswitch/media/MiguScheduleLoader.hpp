#pragma once
#include <atomic>
#include <SDL2/SDL.h>
#include "MiguSportsProvider.hpp"

namespace okswitch::media {
// Only the worker writes result_; the UI reads it after the release/acquire handoff.
class MiguScheduleLoader {
public:
    explicit MiguScheduleLoader(net::HttpClient& http) : provider_(http) {}
    ~MiguScheduleLoader() { shutdown(); }
    bool start(std::string anchor = {}, std::string direction = {}) {
        if (thread_) return false;
        anchor_ = std::move(anchor); direction_ = std::move(direction);
        discarded_ = false; done_ = false; cancelled_ = false;
        thread_ = SDL_CreateThread([](void* opaque) -> int {
            auto& self = *static_cast<MiguScheduleLoader*>(opaque);
            self.result_ = self.provider_.matches(self.anchor_, self.direction_, &self.cancelled_);
            self.done_.store(true, std::memory_order_release); return 0;
        }, "migu-schedule", this);
        return thread_ != nullptr;
    }
    bool poll(MiguResult<MiguMatch>& result, std::string& direction) {
        if (!thread_ || !done_.load(std::memory_order_acquire)) return false;
        SDL_WaitThread(thread_, nullptr); thread_ = nullptr;
        if (discarded_) return false;
        result = std::move(result_); direction = direction_; return true;
    }
    bool busy() const { return thread_ != nullptr; }
    void cancel() { discarded_ = true; cancelled_ = true; }
    void shutdown() { cancel(); if (thread_) SDL_WaitThread(thread_, nullptr); thread_ = nullptr; }
private:
    MiguSportsProvider provider_;
    SDL_Thread* thread_ = nullptr;
    std::atomic<bool> done_{false}, cancelled_{false};
    bool discarded_ = false;
    std::string anchor_, direction_;
    MiguResult<MiguMatch> result_;
};
}
