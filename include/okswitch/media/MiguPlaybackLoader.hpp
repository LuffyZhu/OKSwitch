#pragma once
#include <atomic>
#include <optional>
#include <SDL2/SDL.h>
#include "MiguSportsProvider.hpp"

namespace okswitch::media {
// Only the worker writes result_; the UI reads it after the release/acquire handoff.
class MiguPlaybackLoader {
public:
    explicit MiguPlaybackLoader(const MiguSportsProvider& source) : source_(source) {}
    ~MiguPlaybackLoader() { shutdown(); }
    bool start(MiguClip match) {
        if (thread_) return false;
        match_ = std::move(match);provider_.emplace(source_);
        discarded_ = false; done_ = false; cancelled_ = false;
        thread_ = SDL_CreateThread([](void* opaque) -> int {
            auto& self = *static_cast<MiguPlaybackLoader*>(opaque);
            self.result_ = self.provider_->playback(self.match_, &self.cancelled_);
            self.done_.store(true, std::memory_order_release); return 0;
        }, "migu-playback", this);
        return thread_ != nullptr;
    }
    bool poll(cloud::ProviderResult<cloud::PlaybackSource>& result) {
        if (!thread_ || !done_.load(std::memory_order_acquire)) return false;
        SDL_WaitThread(thread_, nullptr); thread_ = nullptr;
        if (discarded_) return false;
        result = std::move(result_); return true;
    }
    bool busy() const { return thread_ != nullptr; }
    void cancel() { discarded_ = true; cancelled_ = true; }
    void shutdown() { cancel(); if (thread_) SDL_WaitThread(thread_, nullptr); thread_ = nullptr; }
private:
    const MiguSportsProvider& source_;
    std::optional<MiguSportsProvider> provider_;
    SDL_Thread* thread_ = nullptr;
    std::atomic<bool> done_{false}, cancelled_{false};
    bool discarded_ = false;
    MiguClip match_;
    cloud::ProviderResult<cloud::PlaybackSource> result_;
};
}
