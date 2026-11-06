#pragma once
#include "okswitch/cloud/CloudDriveProvider.hpp"
#include <curl/curl.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <climits>
#include <vector>
#include <functional>
#include <future>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <chrono>
#include <array>
#include <atomic>
#include <string>
extern "C" {
#include <libavformat/avio.h>
#include <libavutil/error.h>
}

namespace okswitch::media {
// Bounded, seekable HTTP input for FFmpeg builds without an HTTPS protocol.
class CurlVideoInput {
    static constexpr size_t blockSize = 1 * 1024 * 1024;
    static constexpr size_t maxParallelFetches = 4;
    static constexpr size_t segmentCacheBlocks = 8;
    static constexpr size_t segmentReadAhead = 5;
    static constexpr int64_t variableSegmentSlot = 32LL * 1024 * 1024;
    struct Block { int64_t begin = -1; std::vector<unsigned char> data; uint64_t used = 0; };
    struct Segment { std::string url; int64_t begin = 0, size = 0; double startSeconds = 0, durationSeconds = 0; };
    struct SegmentDownload {
        int64_t begin = -1;
        std::vector<unsigned char> data;
        std::string error;
        bool succeeded = false;
    };
    cloud::PlaybackSource rangeSource_;
    std::future<void> rangeWorker_;
    std::mutex rangeMutex_;
    std::condition_variable rangeReady_;
    std::deque<Block> readyRanges_;
    std::atomic<unsigned> rangeGeneration_{0};
    int64_t rangeTarget_ = 0, publishedSize_ = -1;
    int rangeFailure_ = 0;
    std::string rangeError_;
    bool backgroundWorker_ = false;
    void startRangeWorker() {
        if (rangeWorker_.valid()) return;
        int64_t initialPosition=position_;
        for(const auto& cached:blocks_){
            if(position_>=cached.begin&&position_<cached.begin+(int64_t)cached.data.size()){
                const size_t offset=(size_t)(position_-cached.begin);
                Block block;block.begin=position_;
                block.data.assign(cached.data.begin()+offset,cached.data.end());
                initialPosition=block.begin+block.data.size();
                readyRanges_.push_back(std::move(block));
                break;
            }
        }
        rangeTarget_ = initialPosition;
        const int64_t initialSize = size_;
        const unsigned initialGeneration = rangeGeneration_.load();
        rangeWorker_ = std::async(std::launch::async, [this, initialPosition, initialSize, initialGeneration]() {
            unsigned generation = initialGeneration;
            CurlVideoInput worker(rangeSource_, [this, &generation]() {
                return stopping_.load() || generation != rangeGeneration_.load()
                    || (interrupted_ && interrupted_());
            });
            worker.backgroundWorker_ = true;
            worker.position_ = initialPosition;
            worker.size_ = initialSize;
            worker.rangeMulti_ = rangeMulti_; rangeMulti_ = nullptr;
            // A continuous media response can last for hours. Bound idle time,
            // not total transfer time, and resume from the last published byte.
            curl_easy_setopt(worker.curl_,CURLOPT_TIMEOUT,0L);
            curl_easy_setopt(worker.curl_,CURLOPT_LOW_SPEED_LIMIT,1024L);
            curl_easy_setopt(worker.curl_,CURLOPT_LOW_SPEED_TIME,10L);
            struct StreamState {
                CurlVideoInput* owner;
                unsigned generation;
                int64_t begin;
                int64_t requestBegin;
                long status = 0;
                long long rangeBegin = -1, rangeEnd = -1, rangeSize = -1;
                std::vector<unsigned char> pending;
                bool received = false;
            };
            auto publish = [](StreamState& state) -> bool {
                if(state.pending.empty())return true;
                auto& owner=*state.owner;
                std::unique_lock<std::mutex> lock(owner.rangeMutex_);
                while(owner.readyRanges_.size()>=32
                    &&!owner.stopping_.load()&&state.generation==owner.rangeGeneration_.load()
                    &&!(owner.interrupted_&&owner.interrupted_()))
                    owner.rangeReady_.wait_for(lock,std::chrono::milliseconds(20));
                if(owner.stopping_.load()||state.generation!=owner.rangeGeneration_.load()
                    ||(owner.interrupted_&&owner.interrupted_()))return false;
                Block block;block.begin=state.begin;block.data=std::move(state.pending);
                state.begin+=block.data.size();owner.received_+=block.data.size();
                owner.readyRanges_.push_back(std::move(block));owner.rangeReady_.notify_all();
                return true;
            };
            int retries=0;
            for (;;) {
                {
                    std::unique_lock<std::mutex> lock(rangeMutex_);
                    rangeReady_.wait(lock, [&]() { return stopping_.load()
                        || generation != rangeGeneration_.load()
                        || (readyRanges_.size() < 32 && !rangeFailure_); });
                    if (stopping_.load()) break;
                    if (generation != rangeGeneration_.load()) {
                        generation = rangeGeneration_.load();
                        worker.position_ = rangeTarget_;
                        retries=0;
                    }
                }
                if(worker.size_>=0&&worker.position_>=worker.size_){
                    std::lock_guard<std::mutex> lock(rangeMutex_);
                    if(generation==rangeGeneration_.load())rangeFailure_=AVERROR_EOF;
                    rangeReady_.notify_all();continue;
                }
                StreamState state{this,generation,worker.position_,worker.position_};
                const std::string range=std::to_string(worker.position_)+"-";
                curl_easy_setopt(worker.curl_,CURLOPT_RANGE,range.c_str());
                curl_easy_setopt(worker.curl_,CURLOPT_WRITEFUNCTION,
                    +[](char* data,size_t size,size_t count,void* opaque)->size_t{
                        auto& state=*static_cast<StreamState*>(opaque);
                        const size_t bytes=size*count;
                        if(state.status!=206&&(state.status!=200||state.requestBegin!=0))return 0;
                        if(state.status==206&&state.rangeBegin!=state.requestBegin)return 0;
                        state.received=true;
                        state.pending.insert(state.pending.end(),data,data+bytes);
                        if(state.pending.size()>=64*1024){
                            auto& owner=*state.owner;
                            std::unique_lock<std::mutex> lock(owner.rangeMutex_);
                            while(owner.readyRanges_.size()>=32&&!owner.stopping_.load()
                                &&state.generation==owner.rangeGeneration_.load()
                                &&!(owner.interrupted_&&owner.interrupted_()))
                                owner.rangeReady_.wait_for(lock,std::chrono::milliseconds(20));
                            if(owner.stopping_.load()||state.generation!=owner.rangeGeneration_.load()
                                ||(owner.interrupted_&&owner.interrupted_()))return 0;
                            Block block;block.begin=state.begin;block.data=std::move(state.pending);
                            state.begin+=block.data.size();owner.received_+=block.data.size();
                            owner.readyRanges_.push_back(std::move(block));owner.rangeReady_.notify_all();
                        }
                        return bytes;
                    });
                curl_easy_setopt(worker.curl_,CURLOPT_WRITEDATA,&state);
                curl_easy_setopt(worker.curl_,CURLOPT_HEADERFUNCTION,
                    +[](char* data,size_t size,size_t count,void* opaque)->size_t{
                        auto& state=*static_cast<StreamState*>(opaque);
                        const size_t bytes=size*count;
                        if(bytes>=5&&curl_strnequal(data,"HTTP/",5)){
                            long code=0;std::sscanf(data,"HTTP/%*s %ld",&code);state.status=code;
                            state.rangeBegin=state.rangeEnd=state.rangeSize=-1;
                        }else if(bytes>=14&&curl_strnequal(data,"Content-Range:",14)){
                            std::sscanf(data+14," bytes %lld-%lld/%lld",
                                &state.rangeBegin,&state.rangeEnd,&state.rangeSize);
                            if(state.rangeSize>0){
                                std::lock_guard<std::mutex> lock(state.owner->rangeMutex_);
                                state.owner->publishedSize_=state.rangeSize;
                            }
                        }
                        return bytes;
                    });
                curl_easy_setopt(worker.curl_,CURLOPT_HEADERDATA,&state);
                curl_easy_setopt(worker.curl_,CURLOPT_XFERINFODATA,&worker);
                ++requests_;
                const CURLcode rc=curl_easy_perform(worker.curl_);
                long status=0;curl_easy_getinfo(worker.curl_,CURLINFO_RESPONSE_CODE,&status);
                if(rc==CURLE_OK&&state.received)publish(state);
                bool retry=false;
                {
                    std::lock_guard<std::mutex> lock(rangeMutex_);
                    if(generation!=rangeGeneration_.load())continue;
                    const bool complete=rc==CURLE_OK&&state.received
                        &&(state.rangeSize<=0||state.begin>=state.rangeSize);
                    if(complete)rangeFailure_=AVERROR_EOF;
                    else if(!(interrupted_&&interrupted_())&&status!=403&&status!=404
                        &&status!=416&&(state.begin>state.requestBegin||retries<4)){
                        // Proxy connections can close after delivering some
                        // data. Continue at the first unpublished byte, and
                        // reset the retry budget when the stream makes progress.
                        if(state.begin>state.requestBegin)retries=0;
                        else ++retries;
                        worker.position_=state.begin;
                        retry=true;
                    }else{
                        rangeFailure_=(interrupted_&&interrupted_())?AVERROR_EXIT:AVERROR(EIO);
                        rangeError_=status>=400?"HTTP "+std::to_string(status):curl_easy_strerror(rc);
                    }
                    rangeReady_.notify_all();
                }
                if(retry){SDL_Delay(150*std::max(1,retries));continue;}
            }
        });
    }
    int readBuffered(uint8_t* output, int capacity) {
        startRangeWorker();
        std::unique_lock<std::mutex> lock(rangeMutex_);
        // A pause/menu press can interrupt an in-flight request. It is not a
        // permanent stream failure; restart from the consumer position.
        if (rangeFailure_ == AVERROR_EXIT) {
            rangeTarget_ = position_; readyRanges_.clear(); rangeFailure_ = 0;
            ++rangeGeneration_; rangeReady_.notify_all();
        }
        while (readyRanges_.empty() && !rangeFailure_) {
            if (stopping_.load() || (interrupted_ && interrupted_())) return AVERROR_EXIT;
            rangeReady_.wait_for(lock, std::chrono::milliseconds(20));
        }
        if (publishedSize_ >= 0) size_ = publishedSize_;
        while (!readyRanges_.empty()) {
            auto& block = readyRanges_.front();
            if (position_ >= block.begin + static_cast<int64_t>(block.data.size())) {
                readyRanges_.pop_front(); rangeReady_.notify_all(); continue;
            }
            if (position_ < block.begin) break;
            const size_t offset = static_cast<size_t>(position_ - block.begin);
            const size_t count = std::min<size_t>(capacity, block.data.size() - offset);
            std::memcpy(output, block.data.data() + offset, count);
            position_ += count;
            if (offset + count == block.data.size()) {
                readyRanges_.pop_front(); rangeReady_.notify_all();
            }
            return static_cast<int>(count);
        }
        if (rangeFailure_) { error = rangeError_; return rangeFailure_; }
        return AVERROR(EIO);
    }
    CURL* curl_ = nullptr;
    CURLM* rangeMulti_ = nullptr;
    bool reuseRangeConnections_ = false;
    curl_slist* headers_ = nullptr;
    std::array<Block, 16> blocks_;
    int64_t position_ = 0, size_ = -1;
    int64_t lastFetchBegin_ = -1;
    int consecutiveReadErrors_ = 0;
    size_t parallelFetches_ = maxParallelFetches;
    uint64_t clock_ = 0;
    std::atomic<unsigned> requests_{0}, hits_{0};
    std::atomic<uint64_t> downloaded_{0};
    std::atomic<uint64_t> received_{0};
    std::function<bool()> interrupted_;
    std::vector<Segment> segments_;
    std::function<std::string(const std::string&)> refreshSegmentUrl_;
    bool segmented_ = false;
    bool variableSegmentSizes_ = false;
    bool nonBlocking_ = false;
    std::future<SegmentDownload> prefetch_;
    int64_t prefetchBegin_ = -1;
    std::atomic<bool> cancelPrefetch_{false};
    std::atomic<bool> stopping_{false};
    struct Transfer {
        CurlVideoInput* owner = nullptr;
        CURL* easy = nullptr;
        int64_t begin = 0;
        std::string range;
        std::vector<unsigned char> data;
        long long rangeBegin = -1, rangeEnd = -1, rangeSize = -1;
        long status = 0;
        CURLcode result = CURLE_OK;
        bool done = false;
        size_t maxBytes = blockSize;
        bool stopWhenFull = false;
    };
    static int progress(void* opaque, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
        auto& s = *static_cast<CurlVideoInput*>(opaque);
        return s.stopping_.load(std::memory_order_relaxed)
            || s.cancelPrefetch_.load(std::memory_order_relaxed)
            || (s.interrupted_ && s.interrupted_()) ? 1 : 0;
    }
    static size_t write(char* data, size_t size, size_t count, void* opaque) {
        auto& transfer = *static_cast<Transfer*>(opaque);
        size_t bytes = size * count;
        if (transfer.data.size() > transfer.maxBytes
            || bytes > transfer.maxBytes - transfer.data.size()) return 0;
        transfer.data.insert(transfer.data.end(), data, data + bytes);
        if (transfer.owner) transfer.owner->received_ += bytes;
        if (transfer.stopWhenFull && transfer.data.size() == transfer.maxBytes) return 0;
        return bytes;
    }
    static size_t header(char* data, size_t size, size_t count, void* opaque) {
        auto& transfer = *static_cast<Transfer*>(opaque);
        std::string line(data, size * count);
        if (line.compare(0, 5, "HTTP/") == 0) {
            transfer.data.clear();
            transfer.rangeBegin = transfer.rangeEnd = transfer.rangeSize = -1;
        }
        if (line.size() >= 14 && curl_strnequal(line.c_str(), "Content-Range:", 14))
            std::sscanf(line.c_str() + 14, " bytes %lld-%lld/%lld",
                &transfer.rangeBegin, &transfer.rangeEnd, &transfer.rangeSize);
        return size * count;
    }
    static std::string percentDecode(const std::string& value) {
        std::string decoded;
        decoded.reserve(value.size());
        auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        for (size_t i = 0; i < value.size(); ++i) {
            if (value[i] == '%' && i + 2 < value.size()) {
                const int high = hex(value[i + 1]), low = hex(value[i + 2]);
                if (high >= 0 && low >= 0) {
                    decoded.push_back(static_cast<char>((high << 4) | low));
                    i += 2;
                    continue;
                }
            }
            decoded.push_back(value[i] == '+' ? ' ' : value[i]);
        }
        return decoded;
    }
    static std::vector<std::string> segmentCandidates(const std::string& url) {
        std::vector<std::string> candidates{url};
        const size_t scheme = url.find("://");
        if (scheme == std::string::npos) return candidates;
        const size_t hostBegin = scheme + 3;
        const size_t hostEnd = url.find('/', hostBegin);
        if (hostEnd == std::string::npos) return candidates;
        const std::string marker = "backhost=";
        size_t valueBegin = url.find(marker, hostEnd);
        if (valueBegin == std::string::npos) return candidates;
        valueBegin += marker.size();
        const size_t valueEnd = url.find('&', valueBegin);
        const std::string decoded = percentDecode(url.substr(valueBegin, valueEnd - valueBegin));
        size_t cursor = 0;
        while (candidates.size() < 3) {
            const size_t quoteBegin = decoded.find('"', cursor);
            if (quoteBegin == std::string::npos) break;
            const size_t quoteEnd = decoded.find('"', quoteBegin + 1);
            if (quoteEnd == std::string::npos) break;
            const std::string host = decoded.substr(quoteBegin + 1, quoteEnd - quoteBegin - 1);
            cursor = quoteEnd + 1;
            if (host.empty()) continue;
            const std::string candidate = url.substr(0, hostBegin) + host + url.substr(hostEnd);
            if (std::find(candidates.begin(), candidates.end(), candidate) == candidates.end())
                candidates.push_back(candidate);
        }
        return candidates;
    }
    static SegmentDownload downloadSegment(
        CurlVideoInput& s, const Segment& segment, CURL* templateHandle) {
        SegmentDownload result;
        result.begin = segment.begin;
        const auto urls = segmentCandidates(segment.url);
        const size_t expected = s.variableSegmentSizes_ ? 0 : static_cast<size_t>(segment.size);
        result.data.reserve(expected ? expected : 2 * 1024 * 1024);
        result.error = "HLS segment download stalled";
        bool variableComplete = false;
        const size_t maxAttempts = std::max<size_t>(3, urls.size() * 2);
        for (size_t attempt = 0;
             attempt < maxAttempts && (expected == 0 || result.data.size() < expected); ++attempt) {
            if (s.stopping_.load(std::memory_order_relaxed)
                || s.cancelPrefetch_.load(std::memory_order_relaxed)
                || (s.interrupted_ && s.interrupted_())) {
                result.error = "HLS segment download cancelled";
                return result;
            }
            const size_t offset = result.data.size();
            Transfer transfer;
            transfer.owner = &s;
            transfer.begin = segment.begin;
            transfer.maxBytes = expected ? expected : static_cast<size_t>(variableSegmentSlot);
            transfer.stopWhenFull = expected != 0;
            transfer.easy = curl_easy_duphandle(templateHandle);
            if (!transfer.easy) continue;
            const std::string url = s.refreshSegmentUrl_
                ? s.refreshSegmentUrl_(urls[attempt % urls.size()])
                : urls[attempt % urls.size()];
            std::string range;
            curl_easy_setopt(transfer.easy, CURLOPT_URL, url.c_str());
            if (offset > 0) {
                range = std::to_string(offset) + "-";
                curl_easy_setopt(transfer.easy, CURLOPT_RANGE, range.c_str());
            } else {
                curl_easy_setopt(transfer.easy, CURLOPT_RANGE, nullptr);
            }
            curl_easy_setopt(transfer.easy, CURLOPT_FRESH_CONNECT, 1L);
            curl_easy_setopt(transfer.easy, CURLOPT_FORBID_REUSE, 1L);
            curl_easy_setopt(transfer.easy, CURLOPT_CONNECTTIMEOUT, 6L);
            curl_easy_setopt(transfer.easy, CURLOPT_TIMEOUT, 12L);
            curl_easy_setopt(transfer.easy, CURLOPT_LOW_SPEED_LIMIT, 1024L);
            curl_easy_setopt(transfer.easy, CURLOPT_LOW_SPEED_TIME, 4L);
            curl_easy_setopt(transfer.easy, CURLOPT_WRITEFUNCTION, write);
            curl_easy_setopt(transfer.easy, CURLOPT_WRITEDATA, &transfer);
            curl_easy_setopt(transfer.easy, CURLOPT_HEADERFUNCTION, header);
            curl_easy_setopt(transfer.easy, CURLOPT_HEADERDATA, &transfer);
            ++s.requests_;
            transfer.result = curl_easy_perform(transfer.easy);
            curl_easy_getinfo(transfer.easy, CURLINFO_RESPONSE_CODE, &transfer.status);
            curl_easy_cleanup(transfer.easy);
            transfer.easy = nullptr;

            const bool statusOk = transfer.status == 200 || transfer.status == 206;
            if (statusOk && transfer.result == CURLE_OK && transfer.status == 200
                && ((expected && transfer.data.size() == expected)
                    || (!expected && !transfer.data.empty()))) {
                result.data = std::move(transfer.data);
                variableComplete = !expected;
                break;
            }
            if (statusOk && transfer.status == 206 && !transfer.data.empty()
                && transfer.data.size() <= expected - offset) {
                result.data.insert(result.data.end(),
                    transfer.data.begin(), transfer.data.end());
                if (!expected && transfer.rangeSize > 0
                    && transfer.rangeEnd + 1 == transfer.rangeSize) {
                    variableComplete = true;
                    break;
                }
            } else if (statusOk && transfer.status == 200 && !transfer.data.empty()
                && transfer.data.size() > result.data.size()) {
                result.data = std::move(transfer.data);
            }
            if (transfer.result != CURLE_OK && transfer.result != CURLE_WRITE_ERROR)
                result.error = curl_easy_strerror(transfer.result);
            else if (!statusOk)
                result.error = "HTTP " + std::to_string(transfer.status);
        }
        result.succeeded = expected ? result.data.size() == expected : variableComplete;
        if (!result.succeeded)
            result.error += " / received " + std::to_string(result.data.size())
                + (expected ? " of " + std::to_string(expected) : std::string()) + " bytes";
        return result;
    }
    static Block* findSegmentBlock(CurlVideoInput& s, int64_t begin, bool countHit) {
        for (auto& candidate : s.blocks_)
            if (candidate.begin == begin && !candidate.data.empty()) {
                if (countHit) ++s.hits_;
                return &candidate;
            }
        return nullptr;
    }
    static Block* cacheSegment(CurlVideoInput& s, SegmentDownload&& download) {
        auto cacheEnd = s.blocks_.begin() + segmentCacheBlocks;
        auto destinationIt = std::find_if(s.blocks_.begin(), cacheEnd,
            [](const Block& block) { return block.begin < 0 || block.data.empty(); });
        if (destinationIt == cacheEnd) {
            // Read-ahead blocks have an older LRU timestamp than the current
            // block because they were filled before playback began. A plain
            // LRU eviction therefore discards the next unplayed segment and
            // turns a healthy 60-second buffer into a network stall. Reuse a
            // fully consumed block first and keep every future segment intact.
            auto oldestConsumed = cacheEnd;
            for (auto it = s.blocks_.begin(); it != cacheEnd; ++it) {
                const int64_t blockEnd = it->begin
                    + static_cast<int64_t>(it->data.size());
                if (blockEnd > s.position_) continue;
                if (oldestConsumed == cacheEnd || it->used < oldestConsumed->used)
                    oldestConsumed = it;
            }
            destinationIt = oldestConsumed != cacheEnd ? oldestConsumed
                : std::min_element(s.blocks_.begin(), cacheEnd,
                    [](const Block& a, const Block& b) { return a.used < b.used; });
        }
        Block* destination = &*destinationIt;
        destination->begin = download.begin;
        destination->data = std::move(download.data);
        destination->used = ++s.clock_;
        s.downloaded_ += destination->data.size();
        return destination;
    }
    static void collectPrefetch(CurlVideoInput& s, bool wait) {
        if (!s.prefetch_.valid()) return;
        if (!wait && s.prefetch_.wait_for(std::chrono::seconds(0))
            != std::future_status::ready) return;
        SegmentDownload download = s.prefetch_.get();
        s.prefetchBegin_ = -1;
        if (download.succeeded) cacheSegment(s, std::move(download));
    }
    static void startPrefetch(CurlVideoInput& s, size_t index) {
        if (s.prefetch_.valid() || index >= s.segments_.size()
            || s.stopping_.load(std::memory_order_relaxed)) return;
        CURL* templateHandle = curl_easy_duphandle(s.curl_);
        if (!templateHandle) return;
        const Segment segment = s.segments_[index];
        s.cancelPrefetch_.store(false, std::memory_order_relaxed);
        s.prefetchBegin_ = segment.begin;
        s.prefetch_ = std::async(std::launch::async,
            [&s, segment, templateHandle]() {
                SegmentDownload result = downloadSegment(s, segment, templateHandle);
                curl_easy_cleanup(templateHandle);
                return result;
            });
    }
    static int readSegmented(CurlVideoInput& s, uint8_t* output, int capacity) {
        if (s.position_ >= s.size_) return AVERROR_EOF;
        auto segment = std::upper_bound(s.segments_.begin(), s.segments_.end(), s.position_,
            [](int64_t position, const Segment& item) { return position < item.begin; });
        if (segment == s.segments_.begin()) return AVERROR(EIO);
        --segment;
        if (s.position_ >= segment->begin + segment->size) return AVERROR(EIO);
        const size_t segmentIndex = static_cast<size_t>(segment - s.segments_.begin());

        collectPrefetch(s, false);
        Block* block = findSegmentBlock(s, segment->begin, true);
        if (!block && s.prefetch_.valid()) {
            if (segment->begin != s.prefetchBegin_)
                s.cancelPrefetch_.store(true, std::memory_order_relaxed);
            if (s.nonBlocking_ && s.prefetch_.wait_for(std::chrono::seconds(0))
                != std::future_status::ready) return AVERROR(EAGAIN);
            collectPrefetch(s, !s.nonBlocking_);
            s.cancelPrefetch_.store(false, std::memory_order_relaxed);
            block = findSegmentBlock(s, segment->begin, true);
        }
        if (!block) {
            if (s.nonBlocking_) {
                startPrefetch(s, segmentIndex);
                return AVERROR(EAGAIN);
            }
            SegmentDownload download = downloadSegment(s, *segment, s.curl_);
            if (!download.succeeded) {
                s.error = download.error;
                return (s.interrupted_ && s.interrupted_()) ? AVERROR_EXIT : AVERROR(EIO);
            }
            block = cacheSegment(s, std::move(download));
        }

        block->used = ++s.clock_;
        const size_t offset = static_cast<size_t>(s.position_ - block->begin);
        if (s.variableSegmentSizes_ && offset >= block->data.size()) {
            if (segmentIndex + 1 >= s.segments_.size()) return AVERROR_EOF;
            s.position_ = s.segments_[segmentIndex + 1].begin;
            return readSegmented(s, output, capacity);
        }
        const size_t bytes = std::min<size_t>(capacity, block->data.size() - offset);
        std::memcpy(output, block->data.data() + offset, bytes);
        s.position_ += bytes;

        collectPrefetch(s, false);
        if (!s.prefetch_.valid()) {
            const size_t windowEnd = std::min(s.segments_.size(),
                segmentIndex + 1 + segmentReadAhead);
            for (size_t index = segmentIndex + 1; index < windowEnd; ++index) {
                if (findSegmentBlock(s, s.segments_[index].begin, false)) continue;
                startPrefetch(s, index);
                break;
            }
        }
        return static_cast<int>(bytes);
    }
public:
    bool isSegmented() const { return segmented_; }
    void enableNonBlocking() {
        nonBlocking_ = true;
        if (reuseRangeConnections_ && !segmented_ && !backgroundWorker_) startRangeWorker();
    }
    bool prebufferAtSeconds(double seconds, size_t count) {
        if (!segmented_) return true;
        cancelPrefetch_.store(true, std::memory_order_relaxed);
        collectPrefetch(*this, true);
        cancelPrefetch_.store(false, std::memory_order_relaxed);
        size_t first = 0;
        if (seconds > 0) {
            first = segments_.size() - 1;
            for (size_t index = 0; index < segments_.size(); ++index)
                if (seconds < segments_[index].startSeconds
                        + segments_[index].durationSeconds) {
                    first = index;
                    break;
                }
        }
        const size_t limit = std::min(first + count, segments_.size());
        for (size_t batch = first; batch < limit; batch += parallelFetches_) {
            const size_t end = std::min(limit, batch + parallelFetches_);
            std::vector<std::future<SegmentDownload>> jobs;
            for (size_t index = batch; index < end; ++index) {
                if (findSegmentBlock(*this, segments_[index].begin, false)) continue;
                jobs.push_back(std::async(std::launch::async, [this,index]() {
                    CURL* easy=curl_easy_duphandle(curl_);
                    if(!easy){SegmentDownload failed;failed.error="无法创建分片连接";return failed;}
                    SegmentDownload result=downloadSegment(*this,segments_[index],easy);
                    curl_easy_cleanup(easy);return result;
                }));
            }
            for(auto& job:jobs){SegmentDownload download=job.get();if(!download.succeeded){error=download.error;return false;}cacheSegment(*this,std::move(download));}
        }
        return true;
    }
    void pumpReadAhead(double seconds, size_t count) {
        if (!segmented_ || segments_.empty() || count == 0) return;
        collectPrefetch(*this, false);
        if (prefetch_.valid()) return;
        size_t current = segments_.size() - 1;
        for (size_t index = 0; index < segments_.size(); ++index) {
            if (seconds < segments_[index].startSeconds
                    + segments_[index].durationSeconds) {
                current = index;
                break;
            }
        }
        const size_t end = std::min(segments_.size(), current + 1 + count);
        for (size_t index = current + 1; index < end; ++index) {
            if (findSegmentBlock(*this, segments_[index].begin, false)) continue;
            startPrefetch(*this, index);
            break;
        }
    }
    double bufferedUntilSeconds(double seconds) const {
        if (!segmented_ || segments_.empty()) return seconds;
        size_t current = segments_.size() - 1;
        for (size_t index = 0; index < segments_.size(); ++index) {
            if (seconds < segments_[index].startSeconds
                    + segments_[index].durationSeconds) {
                current = index;
                break;
            }
        }
        double buffered = std::max(seconds, segments_[current].startSeconds
            + segments_[current].durationSeconds);
        for (size_t index = current + 1; index < segments_.size(); ++index) {
            bool cached = false;
            for (const auto& block : blocks_)
                if (block.begin == segments_[index].begin && !block.data.empty()) {
                    cached = true;
                    break;
                }
            if (!cached) break;
            buffered = segments_[index].startSeconds
                + segments_[index].durationSeconds;
        }
        return buffered;
    }
    int64_t byteOffsetForSeconds(double seconds) const {
        if (!segmented_ || segments_.empty()) return -1;
        const Segment* chosen = &segments_.back();
        for (const auto& segment : segments_) {
            if (seconds < segment.startSeconds + segment.durationSeconds) {
                chosen = &segment; break;
            }
        }
        return chosen->begin;
    }
    void cancelReadAhead() {
        cancelPrefetch_.store(true, std::memory_order_relaxed);
        collectPrefetch(*this, true);
        cancelPrefetch_.store(false, std::memory_order_relaxed);
    }
    std::string error;
    std::string stats() const {
        const uint64_t received = received_.load();
        const std::string amount = received < 1024 * 1024
            ? std::to_string(received / 1024) + " KB"
            : std::to_string(received / 1024 / 1024) + " MB";
        return "网络请求 " + std::to_string(requests_.load())
            + " · 缓存命中 " + std::to_string(hits_.load())
            + " · 已接收 " + amount;
    }
    uint64_t downloadedBytes() const { return received_.load(); }
    explicit CurlVideoInput(const cloud::PlaybackSource& source, std::function<bool()> interrupted)
        : rangeSource_(source), parallelFetches_(std::max<size_t>(1,
              std::min<size_t>(maxParallelFetches,source.parallelFetches))),
          interrupted_(std::move(interrupted)),refreshSegmentUrl_(source.refreshSegmentUrl) {
        const size_t hostStart=source.url.find("://");
        if(hostStart!=std::string::npos){
            const size_t start=hostStart+3;
            const std::string host=source.url.substr(start,source.url.find_first_of("/:?#",start)-start);
            const std::string suffix=".googlevideo.com";
            reuseRangeConnections_=host=="googlevideo.com"||(host.size()>suffix.size()
                &&host.compare(host.size()-suffix.size(),suffix.size(),suffix)==0);
        }
        if (!source.segmentUrls.empty()
            && source.segmentUrls.size() == source.segmentSizes.size()
            && source.segmentUrls.size() == source.segmentDurations.size()) {
            variableSegmentSizes_ = std::all_of(source.segmentSizes.begin(),
                source.segmentSizes.end(), [](std::uint64_t size) { return size == 0; });
            int64_t begin = 0;double startSeconds = 0;
            for (size_t i = 0; i < source.segmentUrls.size(); ++i) {
                const std::uint64_t bytes = variableSegmentSizes_
                    ? static_cast<std::uint64_t>(variableSegmentSlot)
                    : source.segmentSizes[i];
                const double seconds = source.segmentDurations[i];
                if (bytes == 0 || seconds <= 0
                    || bytes > static_cast<std::uint64_t>(INT64_MAX - begin)) {
                    segments_.clear(); break;
                }
                segments_.push_back({source.segmentUrls[i], begin,
                    static_cast<int64_t>(bytes), startSeconds, seconds});
                begin += static_cast<int64_t>(bytes);startSeconds += seconds;
            }
            segmented_ = !segments_.empty() && segments_.size() == source.segmentUrls.size();
            if (segmented_) size_ = begin;
        }
        curl_ = curl_easy_init();
        if (!curl_) return;
        for (const auto& h : source.headers) {
            std::string line = h.name + ": " + h.value;
            headers_ = curl_slist_append(headers_, line.c_str());
        }
        if (!segmented_&&!reuseRangeConnections_) headers_ = curl_slist_append(headers_, "Connection: close");
        curl_easy_setopt(curl_, CURLOPT_URL, source.url.c_str());
        curl_easy_setopt(curl_, CURLOPT_PROXY, source.proxyUrl.c_str());
        if (!source.proxyUrl.empty()) curl_easy_setopt(curl_, CURLOPT_NOPROXY, "");
        curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers_);
        curl_easy_setopt(curl_, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
        curl_easy_setopt(curl_, CURLOPT_REDIR_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
        curl_easy_setopt(curl_, CURLOPT_CAINFO, "romfs:/cacert.pem");
        curl_easy_setopt(curl_, CURLOPT_SSLVERSION,
            (long)(CURL_SSLVERSION_TLSv1_2 | CURL_SSLVERSION_MAX_TLSv1_2));
        curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl_, CURLOPT_MAXREDIRS, 5L);
        curl_easy_setopt(curl_, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
        curl_easy_setopt(curl_, CURLOPT_FORBID_REUSE, segmented_||reuseRangeConnections_ ? 0L : 1L);
        curl_easy_setopt(curl_, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(curl_, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl_, CURLOPT_TIMEOUT, segmented_ ? 30L : 20L);
        curl_easy_setopt(curl_, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl_, CURLOPT_XFERINFOFUNCTION, progress);
        curl_easy_setopt(curl_, CURLOPT_XFERINFODATA, this);
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, write);
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, this);
        curl_easy_setopt(curl_, CURLOPT_HEADERFUNCTION, header);
        curl_easy_setopt(curl_, CURLOPT_HEADERDATA, this);
    }
    ~CurlVideoInput() {
        stopping_.store(true, std::memory_order_relaxed);
        cancelPrefetch_.store(true, std::memory_order_relaxed);
        rangeReady_.notify_all();
        if (rangeWorker_.valid()) rangeWorker_.wait();
        if (prefetch_.valid()) prefetch_.wait();
        if (rangeMulti_) curl_multi_cleanup(rangeMulti_);
        if (curl_) curl_easy_cleanup(curl_);
        curl_slist_free_all(headers_);
    }
    static int read(void* opaque, uint8_t* output, int capacity) {
        auto& s = *static_cast<CurlVideoInput*>(opaque);
        if (s.interrupted_ && s.interrupted_()) return AVERROR_EXIT;
        if (!s.curl_) return AVERROR(ENOMEM);
        if (capacity <= 0) return AVERROR(EINVAL);
        if (s.size_ >= 0 && s.position_ >= s.size_) return AVERROR_EOF;
        if (s.segmented_) return readSegmented(s, output, capacity);
        if (s.reuseRangeConnections_ && s.nonBlocking_ && !s.backgroundWorker_)
            return s.readBuffered(output, capacity);
        Block* block = nullptr;
        for (auto& candidate : s.blocks_)
            if (s.position_ >= candidate.begin
                && s.position_ < candidate.begin + (int64_t)candidate.data.size()) {
                block = &candidate; ++s.hits_; break;
            }
        if (!block) {
            s.error.clear();
            // Container probing needs small ranges at the head/tail, not a full
            // media block. Return to large blocks once playback begins.
            // Googlevideo startup previously needed four separate 128 KiB
            // requests to reach 512 KiB. One larger range avoids those extra
            // proxy/TLS round trips; playback still switches to streaming.
            const size_t fetchSize=s.backgroundWorker_?256*1024:
                s.reuseRangeConnections_&&!s.nonBlocking_?512*1024:blockSize;
            const int64_t fetchBegin = (s.position_ / (int64_t)fetchSize) * (int64_t)fetchSize;
            const bool readAhead = s.lastFetchBegin_ >= 0
                && fetchBegin > s.lastFetchBegin_
                && fetchBegin <= s.lastFetchBegin_
                    + static_cast<int64_t>(s.parallelFetches_ * fetchSize);
            s.lastFetchBegin_ = fetchBegin;
            std::vector<Transfer> transfers;
            // Header probing jumps between container metadata locations. Only
            // fetch the requested block for those random seeks; starting four
            // speculative TLS transfers here congests the emulator SSL service.
            // Once FFmpeg reads forward, use the provider's full read-ahead.
            const size_t fetchCount = readAhead && !s.reuseRangeConnections_ ? s.parallelFetches_ : 1;
            transfers.reserve(fetchCount);
            for (size_t i = 0; i < fetchCount; ++i) {
                const int64_t begin = fetchBegin + static_cast<int64_t>(i * fetchSize);
                if (begin < fetchBegin || (s.size_ >= 0 && begin >= s.size_)) break;
                const int64_t wanted=s.size_>=0?std::min<int64_t>(fetchSize,s.size_-begin):fetchSize;
                bool cached = std::any_of(s.blocks_.begin(), s.blocks_.end(),
                    [&](const Block& candidate) { return candidate.begin == begin
                        &&static_cast<int64_t>(candidate.data.size())>=wanted; });
                if (cached) continue;
                transfers.emplace_back();
                auto& transfer = transfers.back();
                transfer.owner = &s;
                transfer.begin = begin;
                transfer.maxBytes=fetchSize;
                const int64_t end = std::min<int64_t>(INT64_MAX - wanted, begin)+wanted-1;
                transfer.range = std::to_string(begin) + "-" + std::to_string(end);
            }
            CURLM* multi = s.reuseRangeConnections_ ? s.rangeMulti_ : nullptr;
            if (!multi) { multi = curl_multi_init(); if (s.reuseRangeConnections_) s.rangeMulti_ = multi; }
            if (!multi) return AVERROR(ENOMEM);
            for (auto& transfer : transfers) {
                transfer.easy = curl_easy_duphandle(s.curl_);
                if (!transfer.easy) continue;
                curl_easy_setopt(transfer.easy, CURLOPT_RANGE, transfer.range.c_str());
                curl_easy_setopt(transfer.easy, CURLOPT_WRITEFUNCTION, write);
                curl_easy_setopt(transfer.easy, CURLOPT_WRITEDATA, &transfer);
                curl_easy_setopt(transfer.easy, CURLOPT_HEADERFUNCTION, header);
                curl_easy_setopt(transfer.easy, CURLOPT_HEADERDATA, &transfer);
                curl_multi_add_handle(multi, transfer.easy);
                ++s.requests_;
            }
            {
                int running = 0;
                curl_multi_perform(multi, &running);
                auto collectCompleted = [&]() {
                    int remaining = 0;
                    while (CURLMsg* message = curl_multi_info_read(multi, &remaining)) {
                        if (message->msg != CURLMSG_DONE) continue;
                        for (auto& transfer : transfers)
                            if (transfer.easy == message->easy_handle) {
                                transfer.result = message->data.result;
                                transfer.done = true;
                                break;
                            }
                    }
                };
                collectCompleted();
                while (running && !(s.interrupted_ && s.interrupted_())) {
                    if (!readAhead && !transfers.empty() && transfers.front().done) break;
                    int activity = 0;
                    curl_multi_wait(multi, nullptr, 0, 100, &activity);
                    curl_multi_perform(multi, &running);
                    collectCompleted();
                }
                collectCompleted();
            }
            for (auto& transfer : transfers) {
                if (!transfer.easy) continue;
                curl_easy_getinfo(transfer.easy, CURLINFO_RESPONSE_CODE, &transfer.status);
                curl_multi_remove_handle(multi, transfer.easy);
                curl_easy_cleanup(transfer.easy);
                transfer.easy = nullptr;
            }
            if (multi && !s.reuseRangeConnections_) curl_multi_cleanup(multi);
            if (s.interrupted_ && s.interrupted_()) return AVERROR_EXIT;

            for (auto& transfer : transfers) {
                const bool partial = transfer.done && transfer.result == CURLE_OK
                    && transfer.status == 206 && transfer.rangeBegin == transfer.begin
                    && transfer.rangeEnd >= transfer.rangeBegin
                    && transfer.rangeEnd - transfer.rangeBegin + 1
                        == static_cast<int64_t>(transfer.data.size())
                    && transfer.rangeSize >= transfer.rangeEnd + 1;
                const bool whole = transfer.done && transfer.result == CURLE_OK
                    && transfer.status == 200 && transfer.begin == 0
                    && !transfer.data.empty();
                if (!partial && !whole) {
                    if (transfer.begin == fetchBegin)
                        s.error = transfer.result != CURLE_OK
                            ? curl_easy_strerror(transfer.result)
                            : "HTTP " + std::to_string(transfer.status)
                                + " / invalid byte range";
                    continue;
                }
                s.size_ = partial ? transfer.rangeSize
                    : static_cast<int64_t>(transfer.data.size());
                s.downloaded_ += transfer.data.size();
                Block* destination = &*std::min_element(s.blocks_.begin(), s.blocks_.end(),
                    [](const Block& a, const Block& b) { return a.used < b.used; });
                destination->begin = transfer.begin;
                destination->data = std::move(transfer.data);
                destination->used = ++s.clock_;
            }
            for (auto& candidate : s.blocks_)
                if (s.position_ >= candidate.begin
                    && s.position_ < candidate.begin + static_cast<int64_t>(candidate.data.size())) {
                    block = &candidate; break;
                }
            if (!block) {
                const bool permanent=s.error.rfind("HTTP 403",0)==0||s.error.rfind("HTTP 404",0)==0
                    ||s.error.rfind("HTTP 416",0)==0;
                if(!permanent&&s.consecutiveReadErrors_++<2){
                    // A failed range can leave a stale connection in the
                    // multi handle. Retry with a new proxy/TLS connection.
                    if(s.rangeMulti_){curl_multi_cleanup(s.rangeMulti_);s.rangeMulti_=nullptr;}
                    SDL_Delay(150);return read(opaque,output,capacity);
                }
                s.consecutiveReadErrors_=0;
                return AVERROR(EIO);
            }
        }
        s.consecutiveReadErrors_=0;
        block->used = ++s.clock_;
        size_t offset = (size_t)(s.position_ - block->begin);
        size_t bytes = std::min<size_t>(capacity, block->data.size() - offset);
        std::memcpy(output, block->data.data() + offset, bytes);
        s.position_ += bytes;
        return (int)bytes;
    }
    static int64_t seek(void* opaque, int64_t offset, int whence) {
        auto& s = *static_cast<CurlVideoInput*>(opaque);
        if (whence == AVSEEK_SIZE) return s.size_ >= 0 ? s.size_ : AVERROR(ENOSYS);
        whence &= ~AVSEEK_FORCE;
        int64_t base = 0;
        if (whence == SEEK_CUR) base = s.position_;
        else if (whence == SEEK_END && s.size_ >= 0) base = s.size_;
        else if (whence != SEEK_SET) return AVERROR(EINVAL);
        if (offset < -base || (offset > 0 && base > INT64_MAX - offset)) return AVERROR(EINVAL);
        const int64_t target = base + offset;
        if (target != s.position_ && s.rangeWorker_.valid()) {
            std::lock_guard<std::mutex> lock(s.rangeMutex_);
            s.rangeTarget_ = target;
            s.readyRanges_.clear();
            s.rangeFailure_ = 0;
            s.rangeError_.clear();
            ++s.rangeGeneration_;
            s.rangeReady_.notify_all();
        }
        s.position_ = target;
        return s.position_;
    }
};
}
