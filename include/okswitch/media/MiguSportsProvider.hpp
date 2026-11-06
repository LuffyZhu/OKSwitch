#pragma once

#include <string>
#include <vector>
#include "okswitch/cloud/CloudDriveProvider.hpp"
#include "okswitch/net/HttpClient.hpp"

namespace okswitch::media
{
struct MiguMatch
{
    std::string id;
    std::string date;
    std::string home;
    std::string away;
    std::string score;
    std::string day, kickoffTime, liveId;
    std::string homeId, awayId, homeLogo, awayLogo;
    int status = 0; // Official status: 0 upcoming, 1 live, 2 finished.
};

struct MiguClip
{
    std::string id;
    std::string title;
    std::string duration;
};

struct MiguLiveMatch
{
    std::string id;
    std::string title;
    std::string competition;
    std::string startTime;
    std::string kickoffTime;
    std::string endTime;
    bool live = false;
};

struct MiguSession
{
    std::string userId, userToken, sign, userInfoJson, clientId;
    bool valid() const { return !userId.empty() && !userToken.empty() && !userInfoJson.empty(); }
};

template <typename T> struct MiguResult
{
    bool succeeded = false;
    std::string error;
    std::vector<T> items;
};

class MiguSportsProvider
{
public:
    explicit MiguSportsProvider(net::HttpClient& http) : http_(http) {}
    MiguResult<MiguMatch> matches(const std::string& anchor = {}, const std::string& direction = {}, const std::atomic<bool>* cancel = nullptr) const;
    MiguResult<MiguClip> clips(const MiguMatch& match, const std::atomic<bool>* cancel = nullptr) const;
    MiguResult<MiguLiveMatch> featuredFootball() const;
    cloud::ProviderResult<cloud::PlaybackSource> playback(const MiguClip& clip, const std::atomic<bool>* cancel = nullptr) const;
    cloud::ProviderResult<cloud::PlaybackSource> playbackLive(const MiguLiveMatch& match,int rateType=3) const;
    void setSession(MiguSession session) { session_ = std::move(session); }
    bool hasSession() const { return session_.valid(); }
private:
    net::HttpClient& http_;
    MiguSession session_;
};
}
