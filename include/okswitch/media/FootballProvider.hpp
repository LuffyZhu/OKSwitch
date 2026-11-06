#pragma once

#include <string>
#include <vector>

#include "okswitch/cloud/CloudDriveProvider.hpp"
#include "okswitch/net/HttpClient.hpp"

namespace okswitch::media
{
struct FootballMatch
{
    std::string id;
    std::string league;
    std::string home;
    std::string away;
    std::string date;
    std::string time;
    int liveStatus = 0;
    int matchStatus = 0;
};

struct FootballResult
{
    bool succeeded = false;
    std::string error;
    std::vector<FootballMatch> matches;
};

class FootballProvider
{
public:
    explicit FootballProvider(net::HttpClient& httpClient) : httpClient_(httpClient) {}

    FootballResult schedule() const;
    cloud::ProviderResult<cloud::PlaybackSource> playback(const FootballMatch& match) const;

private:
    net::HttpClient& httpClient_;
};
}
