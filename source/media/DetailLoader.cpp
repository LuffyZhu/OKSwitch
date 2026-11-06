#include "okswitch/media/DetailLoader.hpp"

namespace okswitch::media
{
DetailLoader::DetailLoader(MediaSource& source) : source_(source)
{
    mutex_ = SDL_CreateMutex();
    condition_ = SDL_CreateCond();
    if (mutex_ && condition_)
        thread_ = SDL_CreateThread(entry, "detail-loader", this);
}

DetailLoader::~DetailLoader() { shutdown(); }

bool DetailLoader::request(const SearchResult& item)
{
    if (!mutex_ || !thread_ || item.detailUrl.empty()) return false;
    SDL_LockMutex(mutex_);
    cancel_.store(true);
    ++serial_;
    requested_ = item;
    pending_ = true;
    ready_ = false;
    SDL_CondSignal(condition_);
    SDL_UnlockMutex(mutex_);
    return true;
}

bool DetailLoader::poll(SearchResult& item, DetailResponse& result)
{
    if (!mutex_) return false;
    SDL_LockMutex(mutex_);
    const bool ready = ready_;
    if (ready)
    {
        item = std::move(completed_);
        result = std::move(result_);
        ready_ = false;
    }
    SDL_UnlockMutex(mutex_);
    return ready;
}

void DetailLoader::cancel()
{
    if (!mutex_) return;
    SDL_LockMutex(mutex_);
    cancel_.store(true);
    ++serial_;
    pending_ = false;
    ready_ = false;
    SDL_UnlockMutex(mutex_);
}

void DetailLoader::shutdown()
{
    if (mutex_)
    {
        SDL_LockMutex(mutex_);
        stopping_ = true;
        cancel_.store(true);
        if (condition_) SDL_CondSignal(condition_);
        SDL_UnlockMutex(mutex_);
    }
    if (thread_) { SDL_WaitThread(thread_, nullptr); thread_ = nullptr; }
    if (condition_) { SDL_DestroyCond(condition_); condition_ = nullptr; }
    if (mutex_) { SDL_DestroyMutex(mutex_); mutex_ = nullptr; }
}

int DetailLoader::entry(void* self)
{
    return static_cast<DetailLoader*>(self)->run();
}

int DetailLoader::run()
{
    for (;;)
    {
        SDL_LockMutex(mutex_);
        while (!pending_ && !stopping_) SDL_CondWait(condition_, mutex_);
        if (stopping_) { SDL_UnlockMutex(mutex_); return 0; }
        SearchResult item = requested_;
        const unsigned serial = serial_;
        pending_ = false;
        working_ = true;
        cancel_.store(false);
        SDL_UnlockMutex(mutex_);

        DetailResponse result = source_.detail(item.detailUrl, &cancel_);

        SDL_LockMutex(mutex_);
        working_ = false;
        if (!stopping_ && serial == serial_)
        {
            completed_ = std::move(item);
            result_ = std::move(result);
            ready_ = true;
        }
        SDL_UnlockMutex(mutex_);
    }
}
}
