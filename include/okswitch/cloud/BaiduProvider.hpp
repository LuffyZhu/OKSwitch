#pragma once

#include "okswitch/cloud/CloudDriveProvider.hpp"
#include "okswitch/cloud/QuarkSession.hpp"
#include <atomic>

namespace okswitch::net { class HttpClient; }

namespace okswitch::cloud {
class BaiduProvider final : public CloudDriveProvider {
public:
    explicit BaiduProvider(const net::HttpClient& httpClient);
    void setSession(QuarkSession session);
    void setPreferredMaxHeight(int height){preferredMaxHeight_=height;}
    bool isAuthenticated() const;
    void resetBrowseCancel();
    void cancelBrowse();
    void resetPlaybackCancel();
    void cancelPlayback();
    const char* name() const override;
    bool canHandle(const ShareReference& share) const override;
    ProviderResult<ShareSession> resolveShare(const ShareReference& share) override;
    ProviderResult<std::vector<CloudEntry>> listFiles(const ShareSession& session,const std::string& directoryId) override;
    ProviderResult<PlaybackSource> getPlaybackSource(const ShareSession& session,const CloudEntry& file) override;
    bool cleanupPlaybackSource(const PlaybackSource&,std::string& error);
private:
    const net::HttpClient& httpClient_;
    QuarkSession session_;
    std::string accountUid_;
    std::string encryptedSk_;
    std::string bdstoken_;
    std::atomic<bool> browseCancel_{false};
    std::atomic<bool> playbackCancel_{false};
    int preferredMaxHeight_=1080;
};
}
