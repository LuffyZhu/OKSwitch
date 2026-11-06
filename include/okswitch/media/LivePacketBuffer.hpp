#pragma once

#include <SDL2/SDL.h>
#include <algorithm>
#include <atomic>
#include <deque>
extern "C" {
#include <libavformat/avformat.h>
}

namespace okswitch::media {
// One demux owner reads ahead independently of rendering and paused playback.
// Store compressed packets, never a minute of decoded video frames.
class LivePacketBuffer {
    struct Entry { AVPacket* packet; double seconds; };
    std::deque<Entry> packets_;
    SDL_mutex* mutex_ = nullptr;
    SDL_Thread* thread_ = nullptr;
    std::atomic<bool> stopping_{false};
    AVFormatContext* format_ = nullptr;
    std::atomic<bool>* interrupt_ = nullptr;
    int video_ = -1, audio_ = -1, terminal_ = 0;
    double timeBase_ = 0, frameStep_ = 1.0/30, seconds_ = 0;
    size_t bytes_ = 0, limit_ = 0;
    bool filling_ = true;
    Uint64 fillingSince_ = 0;
    bool full() const { return seconds_ >= 60 || bytes_ >= limit_ || packets_.size() >= 16000; }
    static int entry(void* self) { return static_cast<LivePacketBuffer*>(self)->run(); }
    int run() {
        while (!stopping_.load()) {
            SDL_LockMutex(mutex_); const bool wait = full(); SDL_UnlockMutex(mutex_);
            if (wait) { SDL_Delay(10); continue; }
            AVPacket* packet = av_packet_alloc();
            int rc = packet ? av_read_frame(format_, packet) : AVERROR(ENOMEM);
            if (rc == AVERROR(EAGAIN)) { av_packet_free(&packet); SDL_Delay(10); continue; }
            if (rc < 0) {
                av_packet_free(&packet);
                SDL_LockMutex(mutex_); terminal_ = rc; SDL_UnlockMutex(mutex_);
                break;
            }
            if (packet->stream_index != video_ && packet->stream_index != audio_) {
                av_packet_free(&packet); continue;
            }
            double duration = 0;
            if (packet->stream_index == video_) {
                duration = packet->duration > 0 ? packet->duration * timeBase_ : frameStep_;
                // Bad timestamps must not claim that a single packet fills the cache.
                duration = std::clamp(duration, 0.001, 1.0);
            }
            SDL_LockMutex(mutex_);
            packets_.push_back({packet, duration});
            bytes_ += static_cast<size_t>(std::max(0, packet->size)); seconds_ += duration;
            SDL_UnlockMutex(mutex_);
        }
        return 0;
    }
public:
    ~LivePacketBuffer() { stop(); }
    bool start(AVFormatContext* format, int video, int audio, double frameStep,
               size_t byteLimit, std::atomic<bool>& interrupt) {
        format_ = format; video_ = video; audio_ = audio; frameStep_ = frameStep;
        timeBase_ = av_q2d(format->streams[video]->time_base);
        limit_ = byteLimit; interrupt_ = &interrupt;
        mutex_ = SDL_CreateMutex();
        if (!mutex_) return false;
        fillingSince_ = SDL_GetTicks64();
        thread_ = SDL_CreateThread(entry, "live-read-ahead", this);
        return thread_ != nullptr;
    }
    int read(AVPacket* output) {
        SDL_LockMutex(mutex_);
        if (filling_ && !terminal_ && !full() && seconds_ < 5
            && SDL_GetTicks64() - fillingSince_ < 10000) {
            SDL_UnlockMutex(mutex_); return AVERROR(EAGAIN);
        }
        if (packets_.empty()) {
            if (!filling_) { filling_ = true; fillingSince_ = SDL_GetTicks64(); }
            const int rc = terminal_ ? terminal_ : AVERROR(EAGAIN);
            SDL_UnlockMutex(mutex_); return rc;
        }
        filling_ = false;
        auto item = packets_.front(); packets_.pop_front();
        bytes_ -= static_cast<size_t>(std::max(0, item.packet->size));
        seconds_ = std::max(0.0, seconds_ - item.seconds);
        av_packet_move_ref(output, item.packet); av_packet_free(&item.packet);
        SDL_UnlockMutex(mutex_); return 0;
    }
    void stop() {
        stopping_.store(true);
        if (thread_) {
            // Interrupt an in-flight read before waiting, and before closing format_.
            interrupt_->store(true);
            SDL_WaitThread(thread_, nullptr); thread_ = nullptr;
        }
        for (auto& item : packets_) av_packet_free(&item.packet);
        packets_.clear();
        if (mutex_) { SDL_DestroyMutex(mutex_); mutex_ = nullptr; }
    }
};
}
