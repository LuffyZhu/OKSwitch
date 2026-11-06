#pragma once

#include <atomic>
#include <ctime>
#include <deque>
#include <string>
#include <vector>
#include <SDL2/SDL.h>
#include "okswitch/cloud/CloudDriveProvider.hpp"
#include "okswitch/net/HttpClient.hpp"

namespace okswitch::media {
struct YouTubeItem {
    std::string id, title, channel, thumbnail, duration;
    bool isChannel = false;
};
struct YouTubeResult {
    bool succeeded = false;
    bool durationOnly = false;
    int videoHeight = 0;
    std::string error, message;
    std::vector<YouTubeItem> items;
    cloud::PlaybackSource stream;
    cloud::PlaybackSource fallbackStream;
    int fallbackHeight = 0;
};
struct YouTubeLoginState {
    bool running = false, authenticated = false;
    std::string verificationUrl, userCode, message;
};
class YouTubeProvider {
public:
    explicit YouTubeProvider(net::HttpClient& http, std::string proxyUrl = {}, int maxHeight = 720);
    ~YouTubeProvider();
    YouTubeProvider(const YouTubeProvider&) = delete;
    YouTubeProvider& operator=(const YouTubeProvider&) = delete;
    void home();
    void search(const std::string& query);
    void subscriptions();
    void accountHistory();
    void channelVideos(const std::string& channelId);
    void playback(const std::string& videoId);
    void durations(const std::vector<YouTubeItem>& items);
    void beginLogin();
    void beginTvLoginProbe();
    void signOut();
    void cancel();
    bool poll(YouTubeResult& result);
    YouTubeLoginState loginState();
    std::string profileImage();
    std::string profileStatus();
    bool signedIn() const;
    bool configured() const;
    void reloadConfig();
    void setProxy(const std::string& url);
    std::string currentProxy(){return proxy();}
    void shutdown();
private:
    enum class Action { None, Home, Search, Subscriptions, ChannelVideos, Playback, Login, TvLoginProbe, AccountHistory, Durations };
    static int entry(void* self);
    int run();
    YouTubeResult perform(Action action, const std::string& value);
    void clearListCache();
    YouTubeResult homeNow();
    YouTubeResult searchNow(const std::string& query);
    YouTubeResult subscriptionsNow(int maxPages=5);
    YouTubeResult subscriptionFeedNow();
    YouTubeResult accountBrowseNow(const char* browseId, const char* label);
    YouTubeResult personalBrowseNow(const char* browseId, const char* path, const char* label);
    void loadProfileNow(const std::string& token,const std::string& proxyUrl);
    bool loadTvProfileNow(const std::string& token,const std::string& proxyUrl);
    YouTubeResult channelVideosNow(const std::string& channelId);
    YouTubeResult playbackNow(const std::string& videoId);
    YouTubeResult durationsNow(const std::string& ids);
    YouTubeResult loginNow();
    YouTubeResult tvLoginProbeNow();
    YouTubeResult cookieBrowse(const std::string& cookie,const std::string& path,bool channels);
    std::string cookie();
    bool refreshToken();
    std::string accessToken();
    std::string tvBrowseToken();
    std::string proxy();
    void setLoginState(const YouTubeLoginState& state);
    net::HttpClient& http_;
    SDL_mutex* mutex_ = nullptr;
    SDL_cond* condition_ = nullptr;
    SDL_Thread* thread_ = nullptr;
    SDL_Thread* profileThread_ = nullptr;
    std::atomic<bool> cancel_{false};
    std::atomic<bool> profileCancel_{false}, profileDone_{true};
    std::atomic<bool> authenticated_{false};
    bool stopping_ = false, pending_ = false, ready_ = false;
    unsigned serial_ = 0;
    Action action_ = Action::None;
    std::string value_, clientId_, clientSecret_, refreshToken_, token_, tvRefreshToken_, tvToken_, proxyUrl_;
    std::string cookie_;
    std::string rejectedCookie_;
    std::string profileImageUrl_;
    std::string profileStatus_;
    std::string profileToken_, profileProxy_, profileRefreshToken_, profileClientId_, profileClientSecret_;
    std::string profileTvToken_, profileTvRefreshToken_;
    bool profileAttempted_ = false;
    bool accountBrowseScopeDenied_ = false;
    Uint32 profileLastAttempt_ = 0;
    int maxHeight_ = 720;
    std::time_t tokenExpires_ = 0;
    std::time_t tvTokenExpires_ = 0;
    YouTubeResult result_;
    YouTubeLoginState login_;
    struct ListCacheEntry {
        Action action;
        std::string value;
        Uint32 createdAt;
        YouTubeResult result;
    };
    std::deque<ListCacheEntry> listCache_;
};
}
