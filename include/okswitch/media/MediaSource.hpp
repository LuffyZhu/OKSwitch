#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "okswitch/net/HttpClient.hpp"
#include "okswitch/cloud/CloudDriveProvider.hpp"

namespace okswitch::media
{
struct SearchResult
{
    std::string sourceName;
    std::string title;
    std::string status;
    std::string posterUrl;
    std::string detailUrl;
    std::string duration;
    double rating = 0.0;
    int rank = 0;
};

struct ShareLink
{
    std::string label;
    std::string url;
};

struct EpisodeLink
{
    std::string label;
    std::string opaque;
};

struct MediaDetail
{
    std::string title;
    std::string posterUrl;
    std::string year;
    std::string status;
    std::string description;
    std::string director, actors;
    std::vector<ShareLink> shares;
    std::vector<EpisodeLink> episodes;
};

struct MediaResult
{
    bool succeeded = false;
    std::string error;
};

struct SearchResponse : MediaResult
{
    std::vector<SearchResult> items;
    int total = 0;
};

struct DetailResponse : MediaResult
{
    MediaDetail detail;
};

struct SourceDefinition
{
    std::string name;
    std::string baseUrl;
    std::string key;
    std::string api;
    std::vector<std::string> baseUrls;
};

class MediaSource
{
public:
    explicit MediaSource(net::HttpClient& httpClient);
    SearchResponse browse(const SourceDefinition& source, const std::string& path) const;
    SearchResponse catalog(const std::string& endpoint, const std::atomic<bool>* cancel = nullptr) const;
    SearchResponse search(const SourceDefinition& source, const std::string& keyword,
                          const std::atomic<bool>* cancel = nullptr) const;
    DetailResponse detail(const std::string& url,
                          const std::atomic<bool>* cancel = nullptr) const;
    cloud::ProviderResult<cloud::PlaybackSource> guaziPlayback(
        const std::string& opaque, const std::atomic<bool>* cancel = nullptr) const;

    static std::vector<SearchResult> parseSearchHtml(
        const std::string& sourceName,
        const std::string& responseUrl,
        const std::string& html);
    static std::vector<SearchResult> parseListingHtml(
        const std::string& sourceName,
        const std::string& responseUrl,
        const std::string& html);
    static MediaDetail parseDetailHtml(const std::string& html);

private:
    net::HttpClient& httpClient_;
};
}
