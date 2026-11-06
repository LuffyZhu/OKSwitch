#pragma once

#include <string>
#include <vector>

#include "okswitch/cloud/CloudDriveProvider.hpp"
#include "okswitch/net/HttpClient.hpp"

namespace okswitch::media
{
struct CctvChannel
{
    std::string id;
    std::string name;
};

class CctvLiveProvider
{
public:
    explicit CctvLiveProvider(net::HttpClient& httpClient) : httpClient_(httpClient) {}

    std::vector<CctvChannel> channels() const;
    cloud::ProviderResult<cloud::PlaybackSource> playback(const std::string& channel) const;

private:
    net::HttpClient& httpClient_;
};
}
