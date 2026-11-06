#pragma once

#include "okswitch/media/MediaSource.hpp"

namespace okswitch::media::guazi
{
SearchResponse search(net::HttpClient& http, const std::string& keyword,
                      const std::atomic<bool>* cancel);
DetailResponse detail(net::HttpClient& http, const std::string& id,
                      const std::atomic<bool>* cancel);
cloud::ProviderResult<cloud::PlaybackSource> playback(
    net::HttpClient& http, const std::string& opaque,
    const std::atomic<bool>* cancel);
}
