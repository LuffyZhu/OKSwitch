#include "okswitch/media/CctvLiveProvider.hpp"

#include <SDL2/SDL.h>
#include <jansson.h>

#include <chrono>
#include <cstdio>
#include <sstream>
#include <vector>

extern "C" {
#include <libavutil/md5.h>
}

namespace okswitch::media
{
namespace
{
constexpr char kApi[] = "https://vdnx.live.cntv.cn/api/v3/vdn/live";
constexpr char kSecret[] = "a4220a71b31746908fa3e7fdd7a6852a";
constexpr char kUserAgent[] =
    "Mozilla/5.0 (Nintendo Switch; WifiWebAuthApplet) AppleWebKit/609.4";

std::string md5(const std::string& value)
{
    unsigned char digest[16]{};
    av_md5_sum(digest, reinterpret_cast<const unsigned char*>(value.data()), value.size());
    char text[33]{};
    for (size_t i = 0; i < sizeof(digest); ++i)
        std::snprintf(text + i * 2, 3, "%02x", digest[i]);
    return text;
}

std::string jsonString(json_t* object, const char* key)
{
    json_t* value = object ? json_object_get(object, key) : nullptr;
    return json_is_string(value) ? json_string_value(value) : "";
}

std::string absoluteUrl(const std::string& base, const std::string& path)
{
    if (path.rfind("http://", 0) == 0 || path.rfind("https://", 0) == 0)
        return path;
    const size_t scheme = base.find("://");
    if (scheme == std::string::npos) return path;
    const size_t hostEnd = base.find('/', scheme + 3);
    if (hostEnd == std::string::npos) return base + "/" + path;
    if (!path.empty() && path.front() == '/')
        return base.substr(0, hostEnd) + path;
    const size_t query = base.find('?');
    const std::string clean = base.substr(0, query);
    const size_t slash = clean.rfind('/');
    return clean.substr(0, slash + 1) + path;
}
}

std::vector<CctvChannel> CctvLiveProvider::channels() const
{
    return {{"cctv5", "CCTV-5 体育"},
            {"cctv5plus", "CCTV-5+ 体育赛事"},
            {"wuxing", "五星体育"},
            {"cctv16", "CCTV-16 奥林匹克"}};
}

cloud::ProviderResult<cloud::PlaybackSource> CctvLiveProvider::playback(
    const std::string& channel) const
{
    cloud::ProviderResult<cloud::PlaybackSource> result;
    // CCTV's web API currently exposes cdrm/udrm transport streams. They look
    // like ordinary H.264 to FFmpeg but their slices are encrypted, producing
    // grey/white frames without the proprietary web DRM module. Use a plain
    // MPEG-TS relay that was verified to contain no DRM license payload.
    std::vector<std::string> publicPlaylists;
    // Prefer relays whose media fragments can be downloaded comfortably faster
    // than real time. The old 183.129 route delivered an 8 Mbps fragment in
    // about 12 seconds, so its three-fragment live window inevitably stalled.
    if(channel=="cctv5") publicPlaylists={
        "http://221.226.51.220:50081/newlive/live/hls/5/live.m3u8",
        "http://58.56.162.102:4466/newlive/live/hls/5/live.m3u8",
        "http://183.129.255.66:8480/hls/5/index.m3u8"};
    else if(channel=="cctv5plus") publicPlaylists={
        // Android's Kimentanm list: verified CCTV5+ 720p progressive stream.
        // Resolve the expiring CDN redirect each time playback starts.
        "http://101.35.240.114:88/live.php?id=CCTV5p",
        // These entry URLs were checked against the on-screen channel bug.
        // The 173.208.212 and 107.150 routes are still alive but have been
        // rebound to shopping channels, so accepting them by HTTP status
        // alone selects the wrong programme.
        "http://173.208.234.146/live/cctv5p.m3u8",
        "http://183.129.255.66:8480/hls/6/index.m3u8",
        "http://120.198.86.186:9901/tsfile/live/1025_1.m3u8?key=txiptv&playlive=1&authid=0"};
    else if(channel=="wuxing") publicPlaylists={
        "http://101.35.240.114:88/live.php?id=%E4%BA%94%E6%98%9F%E4%BD%93%E8%82%B2"};
    else if(channel=="cctv16") publicPlaylists={
        "http://173.208.234.146/live/cctv16.m3u8",
        "http://183.129.255.66:8480/hls/17/index.m3u8",
        "http://38.75.136.137:98/gslb/dsdqpub/cctv16hd.m3u8?auth=testpub"};
    for(const auto& publicPlaylist:publicPlaylists){
        const bool androidRoute=publicPlaylist.find("101.35.240.114:88/")!=std::string::npos;
        net::HttpRequest health;health.url=publicPlaylist;health.connectTimeoutSeconds=3;health.timeoutSeconds=5;health.maxResponseBytes=64*1024;health.headers={{"User-Agent",androidRoute?"iPhone":kUserAgent}};
        if(androidRoute)health.headers.push_back({"Referer",channel=="wuxing"?"https://live.kankanews.com/":"https:/aptv.app/"});
        const auto checked=httpClient_.get(health);
        if(!checked.succeeded||checked.body.find("#EXTM3U")==std::string::npos)continue;
        std::string resolved=checked.effectiveUrl.empty()?publicPlaylist:checked.effectiveUrl;
        // The Android entry is live.php; FFmpeg must receive its final HLS URL
        // so it uses the rolling-live reader and preserves relative segment URLs.
        if(androidRoute){
            std::istringstream lines(checked.body);std::string line;bool variant=false;
            while(std::getline(lines,line)){
                if(!line.empty()&&line.back()=='\r')line.pop_back();
                if(variant&&!line.empty()&&line[0]!='#'){resolved=absoluteUrl(resolved,line);break;}
                if(line.rfind("#EXT-X-STREAM-INF:",0)==0)variant=true;
            }
            health.url=resolved;const auto media=httpClient_.get(health);
            if(!media.succeeded||media.body.find("#EXTINF:")==std::string::npos)continue;
            if(!media.effectiveUrl.empty())resolved=media.effectiveUrl;
        }
        result.value.url=resolved;
        for(const auto& header:health.headers)result.value.headers.push_back({header.name,header.value});
        result.value.forceSoftwareVideo=false;result.value.deinterlaceVideo=true;result.value.displayAspectRatio=16.0/9.0;result.succeeded=true;return result;
    }
    if(!publicPlaylists.empty()){
        result.error={cloud::ProviderErrorCode::NetworkError,"公共直播线路暂时不可用"};return result;
    }
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const unsigned nonce = 100 + static_cast<unsigned>((now ^ SDL_GetTicks64()) % 900);
    const std::string timestamp = std::to_string(now);
    const std::string nonceText = std::to_string(nonce);
    const std::string auth = timestamp + "-" + nonceText + "-"
        + md5(channel + timestamp + nonceText + kSecret);

    net::HttpRequest request;
    request.url = std::string(kApi) + "?channel=" + channel
        + "&vn=1&pdrm=1&uid=&hbss=" + timestamp;
    request.connectTimeoutSeconds = 8;
    request.timeoutSeconds = 15;
    request.maxResponseBytes = 64 * 1024;
    request.headers = {{"User-Agent", kUserAgent},
                       {"Referer", "https://tv.cctv.com/"},
                       {"Origin", "https://tv.cctv.com"},
                       {"auth-key", auth}};
    const auto response = httpClient_.get(request);
    if (!response.succeeded)
    {
        result.error = {cloud::ProviderErrorCode::NetworkError,
                        "央视直播接口失败：" + response.error};
        return result;
    }

    json_error_t error{};
    json_t* root = json_loadb(response.body.data(), response.body.size(), 0, &error);
    if (!root)
    {
        result.error = {cloud::ProviderErrorCode::InvalidResponse,
                        "央视直播接口返回无法解析"};
        return result;
    }
    const std::string ack = jsonString(root, "ack");
    const std::string play = jsonString(root, "play");
    const std::string message = jsonString(root, "tip_msg");
    json_t* manifest = json_object_get(root, "manifest");
    json_t* backup = json_object_get(root, "backup");
    std::string playlist = jsonString(manifest, "hls_cdrm");
    if (playlist.empty()) playlist = jsonString(backup, "hls_cdrm");
    json_decref(root);
    if (ack != "yes" || play != "1" || playlist.empty())
    {
        result.error = {cloud::ProviderErrorCode::Unsupported,
                        message.empty() ? "当前频道暂时不可播放" : message};
        return result;
    }

    net::HttpRequest masterRequest;
    masterRequest.url = playlist;
    masterRequest.connectTimeoutSeconds = 8;
    masterRequest.timeoutSeconds = 15;
    masterRequest.maxResponseBytes = 128 * 1024;
    masterRequest.headers = {{"User-Agent", kUserAgent}};
    const auto masterResponse = httpClient_.get(masterRequest);
    if (!masterResponse.succeeded)
    {
        result.error = {cloud::ProviderErrorCode::NetworkError,
                        "央视直播清单失败：" + masterResponse.error};
        return result;
    }
    std::istringstream lines(masterResponse.body);
    std::string line;
    bool takeNext = false;
    std::string variant;
    while (std::getline(lines, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (takeNext && !line.empty() && line.front() != '#')
        {
            variant = absoluteUrl(playlist, line);
            break;
        }
        takeNext = line.rfind("#EXT-X-STREAM-INF:", 0) == 0
            && line.find("RESOLUTION=1280x720") != std::string::npos;
    }
    if (variant.empty())
    {
        result.error = {cloud::ProviderErrorCode::InvalidResponse,
                        "央视直播没有可用的 720P 线路"};
        return result;
    }
    playlist = std::move(variant);

    // The Switch FFmpeg build has HTTP and HLS but no HTTPS protocol. CCTV's
    // CDN exposes the same rolling playlist and MPEG-TS fragments over HTTP.
    if (playlist.rfind("https://", 0) == 0)
        playlist.replace(0, 8, "http://");
    result.value.url = std::move(playlist);
    result.value.forceSoftwareVideo = true;
    result.value.headers = {{"User-Agent", kUserAgent}};
    result.succeeded = true;
    return result;
}
}
