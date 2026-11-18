#pragma once

#include "okswitch/media/MediaSource.hpp"

namespace okswitch::media::huangguo {

SearchResponse catalog(net::HttpClient& http,const std::string& section,int page,
    const std::atomic<bool>* cancel=nullptr);
SearchResponse search(net::HttpClient& http,const std::string& keyword,
    const std::atomic<bool>* cancel=nullptr);
DetailResponse detail(net::HttpClient& http,const std::string& id,
    const std::atomic<bool>* cancel=nullptr);
cloud::ProviderResult<cloud::PlaybackSource> playback(net::HttpClient& http,
    const std::string& opaque,const std::atomic<bool>* cancel=nullptr);

}
