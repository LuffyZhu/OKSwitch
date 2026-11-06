#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <functional>

namespace okswitch::cloud
{
struct RequestHeader
{
    std::string name;
    std::string value;
};

struct ShareReference
{
    std::string url;
    std::string accessCode;
};

struct ShareSession
{
    std::string shareId;
    std::string rootId;
    std::string opaqueState;
};

enum class EntryType
{
    File,
    Directory
};

struct CloudEntry
{
    std::string id;
    std::string shareToken;
    // Provider-specific, short-lived playback metadata returned with a file
    // listing. For Baidu this is the signed thumbnail URL used to derive the
    // matching d.pcs file URL.
    std::string playbackHint;
    std::string parentId;
    std::string name;
    EntryType type = EntryType::File;
    std::uint64_t size = 0;
    double durationSeconds = 0;
};

struct PlaybackSource
{
    std::string url;
    // Optional separately delivered audio track (for adaptive video streams).
    std::string audioUrl;
    std::string proxyUrl;
    std::string accessNotice;
    // A server-transcoded HLS playlist can be exposed as a seekable virtual
    // MPEG-TS stream when FFmpeg itself has no HTTPS protocol enabled.
    std::vector<std::string> segmentUrls;
    // Regenerate short-lived signatures immediately before each HLS fetch.
    std::function<std::string(const std::string&)> refreshSegmentUrl;
    std::vector<std::uint64_t> segmentSizes;
    std::vector<double> segmentDurations;
    std::string temporaryId;
    double durationSeconds = 0;
    std::uint32_t parallelFetches = 4;
    std::uint32_t initialBufferSegments = 3;
    bool forceSoftwareVideo = false;
    bool deinterlaceVideo = false;
    double displayAspectRatio = 0;
    std::vector<RequestHeader> headers;
};

enum class ProviderErrorCode
{
    None,
    InvalidShareLink,
    AuthenticationRequired,
    NetworkError,
    InvalidResponse,
    Unsupported,
    NotImplemented
};

struct ProviderError
{
    ProviderErrorCode code = ProviderErrorCode::None;
    std::string message;
};

template <typename T>
struct ProviderResult
{
    bool succeeded = false;
    T value{};
    ProviderError error{};
};

class CloudDriveProvider
{
public:
    virtual ~CloudDriveProvider() = default;

    virtual const char* name() const = 0;
    virtual bool canHandle(const ShareReference& share) const = 0;
    virtual ProviderResult<ShareSession> resolveShare(
        const ShareReference& share) = 0;
    virtual ProviderResult<std::vector<CloudEntry>> listFiles(
        const ShareSession& session,
        const std::string& directoryId) = 0;
    virtual ProviderResult<PlaybackSource> getPlaybackSource(
        const ShareSession& session,
        const CloudEntry& file) = 0;
};
}
