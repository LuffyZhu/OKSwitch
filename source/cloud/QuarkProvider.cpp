#include "okswitch/cloud/QuarkProvider.hpp"
#include "okswitch/net/HttpClient.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <jansson.h>
#include <SDL2/SDL.h>
#include <sstream>
#include <utility>

namespace okswitch::cloud
{
namespace
{
bool isShareIdCharacter(char value)
{
    const unsigned char character = static_cast<unsigned char>(value);
    return std::isalnum(character) != 0 || value == '-' || value == '_';
}

bool extractShareId(const std::string& url, std::string& shareId)
{
    const std::string schemeSeparator = "://";
    const std::size_t schemePosition = url.find(schemeSeparator);
    if (schemePosition == std::string::npos)
    {
        return false;
    }

    std::string scheme = url.substr(0, schemePosition);
    std::transform(scheme.begin(), scheme.end(), scheme.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    if (scheme != "http" && scheme != "https")
    {
        return false;
    }

    const std::size_t authorityStart = schemePosition + schemeSeparator.size();
    const std::size_t pathStart = url.find('/', authorityStart);
    const std::size_t authorityEnd = pathStart == std::string::npos
        ? url.find_first_of("?#", authorityStart)
        : pathStart;
    const std::size_t hostEnd = authorityEnd == std::string::npos
        ? url.size() : authorityEnd;
    std::string host = url.substr(authorityStart, hostEnd - authorityStart);
    const std::size_t portPosition = host.find(':');
    if (portPosition != std::string::npos)
    {
        host.resize(portPosition);
    }
    std::transform(host.begin(), host.end(), host.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    if (host != "pan.quark.cn")
    {
        return false;
    }

    if (pathStart == std::string::npos)
    {
        return false;
    }
    constexpr const char sharePath[] = "/s/";
    if (url.compare(pathStart, sizeof(sharePath) - 1, sharePath) != 0)
    {
        return false;
    }

    const std::size_t idStart = pathStart + sizeof(sharePath) - 1;
    std::size_t idEnd = idStart;
    while (idEnd < url.size() && isShareIdCharacter(url[idEnd]))
    {
        ++idEnd;
    }
    if (idEnd == idStart)
    {
        return false;
    }
    if (idEnd < url.size() && url[idEnd] != '/' && url[idEnd] != '?'
        && url[idEnd] != '#')
    {
        return false;
    }

    shareId = url.substr(idStart, idEnd - idStart);
    return true;
}

std::string percentEncode(const std::string& value)
{
    std::ostringstream encoded;
    encoded << std::uppercase << std::hex;
    for (const unsigned char character : value)
    {
        if (std::isalnum(character) != 0 || character == '-'
            || character == '_' || character == '.' || character == '~')
        {
            encoded << static_cast<char>(character);
        }
        else
        {
            encoded << '%' << std::setw(2) << std::setfill('0')
                << static_cast<int>(character);
        }
    }
    return encoded.str();
}

ProviderError responseError(const net::HttpResponse& response)
{
    ProviderError error;
    if (!response.succeeded)
    {
        error.code = ProviderErrorCode::NetworkError;
        // Quark reports expired and cancelled shares as HTTP 404 with a JSON
        // business code. Preserve that distinction instead of showing a
        // misleading generic network error.
        json_error_t jsonError{};
        json_t* root = response.body.empty() ? nullptr : json_loadb(
            response.body.data(), response.body.size(), 0, &jsonError);
        json_t* code = root == nullptr ? nullptr : json_object_get(root, "code");
        const json_int_t businessCode = json_is_integer(code)
            ? json_integer_value(code) : 0;
        if (businessCode == 41011)
            error.message = "夸克分享已失效";
        else if (businessCode == 41012)
            error.message = "夸克分享已被取消";
        else
        {
            json_t* messageValue = root == nullptr
                ? nullptr : json_object_get(root, "message");
            const std::string message = json_is_string(messageValue)
                ? json_string_value(messageValue) : std::string();
            error.message = message.empty()
                ? (response.error.empty() ? "Quark request failed" : response.error)
                : message;
        }
        if (root != nullptr) json_decref(root);
    }
    else
    {
        error.code = ProviderErrorCode::InvalidResponse;
        error.message = "Invalid Quark response";
    }
    return error;
}

bool isSuccessfulResponse(json_t* root)
{
    json_t* status = json_object_get(root, "status");
    json_t* code = json_object_get(root, "code");
    return json_is_integer(status) && json_integer_value(status) == 200
        && json_is_integer(code) && json_integer_value(code) == 0;
}

std::string jsonString(json_t* object, const char* name)
{
    json_t* value = json_object_get(object, name);
    return json_is_string(value) ? json_string_value(value) : std::string();
}

struct TranscodedStream
{
    std::string url;
    int score = -1;
};

std::string trimLine(std::string value)
{
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n'
        || value.back() == ' ' || value.back() == '\t')) value.pop_back();
    std::size_t first = 0;
    while (first < value.size() && (value[first] == ' ' || value[first] == '\t')) ++first;
    return value.substr(first);
}

std::string absoluteUrl(const std::string& base, const std::string& path)
{
    if (path.rfind("http://", 0) == 0 || path.rfind("https://", 0) == 0) return path;
    const std::size_t scheme = base.find("://");
    if (scheme == std::string::npos) return path;
    const std::size_t hostEnd = base.find('/', scheme + 3);
    if (hostEnd == std::string::npos) return base + "/" + path;
    if (!path.empty() && path.front() == '/') return base.substr(0, hostEnd) + path;
    const std::size_t query = base.find('?');
    const std::string clean = base.substr(0, query);
    const std::size_t slash = clean.rfind('/');
    return clean.substr(0, slash + 1) + path;
}

std::string quotedAttribute(const std::string& line, const std::string& name)
{
    const std::string marker = name + "=\"";
    const std::size_t begin = line.find(marker);
    if (begin == std::string::npos) return {};
    const std::size_t valueBegin = begin + marker.size();
    const std::size_t end = line.find('"', valueBegin);
    return end == std::string::npos ? std::string() : line.substr(valueBegin, end - valueBegin);
}

int playlistHeight(const std::string& line)
{
    const std::size_t marker = line.find("RESOLUTION=");
    if (marker == std::string::npos) return 0;
    const std::size_t x = line.find('x', marker + 11);
    if (x == std::string::npos) return 0;
    return std::atoi(line.c_str() + x + 1);
}

std::string selectPlaylistVariant(const std::string& body, const std::string& base,
                                  int maxHeight)
{
    std::istringstream input(body);
    std::string line;
    int pendingHeight = -1, bestHeight = -1;
    std::string best;
    while (std::getline(input, line))
    {
        line = trimLine(line);
        if (line.rfind("#EXT-X-STREAM-INF:", 0) == 0)
        {
            pendingHeight = playlistHeight(line);
            continue;
        }
        if (pendingHeight < 0 || line.empty() || line.front() == '#') continue;
        const int height = pendingHeight > 0 ? pendingHeight : maxHeight;
        if (height <= maxHeight && height > bestHeight)
        {
            bestHeight = height;
            best = absoluteUrl(base, line);
        }
        pendingHeight = -1;
    }
    return best;
}

bool parseMediaPlaylist(const std::string& body, const std::string& base,
                        std::vector<std::string>& urls,
                        std::vector<std::uint64_t>& sizes,
                        std::vector<double>& durations, double& total)
{
    if (body.find("#EXT-X-KEY:") != std::string::npos) return false;
    std::istringstream input(body);
    std::string line;
    double pendingDuration = 0.0;
    bool addedInit = false;
    while (std::getline(input, line))
    {
        line = trimLine(line);
        if (line.rfind("#EXT-X-MAP:", 0) == 0 && !addedInit)
        {
            const std::string uri = quotedAttribute(line, "URI");
            if (!uri.empty())
            {
                urls.push_back(absoluteUrl(base, uri));
                sizes.push_back(0);
                durations.push_back(0.001);
                addedInit = true;
            }
        }
        else if (line.rfind("#EXTINF:", 0) == 0)
        {
            pendingDuration = std::max(0.001, std::atof(line.c_str() + 8));
        }
        else if (!line.empty() && line.front() != '#' && pendingDuration > 0.0)
        {
            urls.push_back(absoluteUrl(base, line));
            sizes.push_back(0);
            durations.push_back(pendingDuration);
            total += pendingDuration;
            pendingDuration = 0.0;
        }
    }
    return !urls.empty() && urls.size() > (addedInit ? 1U : 0U);
}

bool looksLikeMedia(const std::string& body)
{
    if (body.size() >= 4
        && static_cast<unsigned char>(body[0]) == 0x1a
        && static_cast<unsigned char>(body[1]) == 0x45
        && static_cast<unsigned char>(body[2]) == 0xdf
        && static_cast<unsigned char>(body[3]) == 0xa3) return true; // Matroska
    if (body.size() >= 12 && body.compare(4, 4, "ftyp") == 0) return true;
    if (body.size() >= 4 && body.compare(0, 4, "FLV\x01") == 0) return true;
    if (body.size() >= 12 && body.compare(0, 4, "RIFF") == 0
        && body.compare(8, 4, "AVI ") == 0) return true;
    if (body.size() >= 7 && body.compare(0, 7, "#EXTM3U") == 0) return true;
    // MPEG transport streams may start after a short CDN prefix.
    for (std::size_t offset = 0; offset < std::min<std::size_t>(188, body.size()); ++offset)
        if (static_cast<unsigned char>(body[offset]) == 0x47
            && offset + 188 < body.size()
            && static_cast<unsigned char>(body[offset + 188]) == 0x47) return true;
    return false;
}

std::string responseMessage(const net::HttpResponse& response)
{
    json_error_t error{};
    json_t* root = response.body.empty() ? nullptr
        : json_loadb(response.body.data(), response.body.size(), 0, &error);
    const std::string message = json_is_object(root)
        ? jsonString(root, "message") : std::string();
    if (root != nullptr) json_decref(root);
    return message;
}

int resolutionScore(const std::string& resolution)
{
    // "super" is Quark's 1080p tier. It is the best fit for Switch and is
    // substantially cheaper to decode than the original 2160p60 HEVC file.
    if (resolution == "super" || resolution == "1080p") return 80;
    if (resolution == "high" || resolution == "720p") return 70;
    if (resolution == "normal") return 60;
    if (resolution == "low") return 50;
    if (resolution == "2k") return 40;
    if (resolution == "4k") return 30;
    return 0;
}

void findTranscodedStream(json_t* value, int inheritedScore,
                          TranscodedStream& best,int maxHeight)
{
    if (json_is_array(value))
    {
        const std::size_t count = json_array_size(value);
        for (std::size_t index = 0; index < count; ++index)
            findTranscodedStream(json_array_get(value, index), inheritedScore, best,maxHeight);
        return;
    }
    if (!json_is_object(value)) return;

    int score = inheritedScore;
    const std::string resolution = jsonString(value, "resolution");
    if(maxHeight<=720&&(resolution=="super"||resolution=="1080p"||resolution=="2k"||resolution=="4k"))return;
    if (!resolution.empty()) score += resolutionScore(resolution);
    const std::string format = jsonString(value, "format");
    if (format == "fmp4" || format == "mp4") score += 20;
    if (format == "m3u8") score -= 20;

    const char* key = nullptr;
    json_t* child = nullptr;
    json_object_foreach(value, key, child)
    {
        if (json_is_string(child))
        {
            const std::string text = json_string_value(child);
            std::string lowerKey = key == nullptr ? std::string() : std::string(key);
            std::transform(lowerKey.begin(), lowerKey.end(), lowerKey.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const bool mediaUrlKey = lowerKey == "url"
                || lowerKey == "play_url" || lowerKey == "video_url"
                || lowerKey == "download_url";
            if (mediaUrlKey
                && (text.compare(0, 7, "http://") == 0
                    || text.compare(0, 8, "https://") == 0))
            {
                int urlScore = score;
                if (text.find(".m3u8") != std::string::npos) urlScore -= 20;
                else urlScore += 10;
                if (urlScore > best.score)
                {
                    best.url = text;
                    best.score = urlScore;
                }
            }
        }
        else
        {
            findTranscodedStream(child, score, best,maxHeight);
        }
    }
}

std::vector<net::HttpHeader> quarkHeaders(
    const QuarkSession& session,
    const std::string& referer)
{
    std::vector<net::HttpHeader> headers = {
        {"Accept", "application/json, text/plain, */*"},
        {"Origin", "https://pan.quark.cn"},
        {"Referer", referer}
    };
    if (!session.cookie.empty())
    {
        headers.push_back({"Cookie", session.cookie});
    }
    if (!session.userAgent.empty())
    {
        headers.push_back({"User-Agent", session.userAgent});
    }
    return headers;
}

template <typename T>
ProviderResult<T> notImplemented()
{
    ProviderResult<T> result;
    result.error.code = ProviderErrorCode::NotImplemented;
    result.error.message = "Quark protocol has not been verified yet";
    return result;
}
}

QuarkProvider::QuarkProvider(const net::HttpClient& httpClient)
    : httpClient_(httpClient)
{
}

void QuarkProvider::setSession(QuarkSession session)
{
    session_ = std::move(session);
}

bool QuarkProvider::isAuthenticated() const
{
    return session_.hasCredentials();
}

void QuarkProvider::resetBrowseCancel(){browseCancel_.store(false);}
void QuarkProvider::cancelBrowse(){browseCancel_.store(true);}
void QuarkProvider::resetPlaybackCancel(){playbackCancel_.store(false);}
void QuarkProvider::cancelPlayback(){playbackCancel_.store(true);}

const char* QuarkProvider::name() const
{
    return "Quark";
}

bool QuarkProvider::canHandle(const ShareReference& share) const
{
    std::string shareId;
    return extractShareId(share.url, shareId);
}

ProviderResult<ShareSession> QuarkProvider::resolveShare(
    const ShareReference& share)
{
    std::string shareId;
    if (!extractShareId(share.url, shareId))
    {
        ProviderResult<ShareSession> result;
        result.error.code = ProviderErrorCode::InvalidShareLink;
        result.error.message = "Invalid Quark share link";
        return result;
    }

    json_t* requestRoot = json_object();
    json_object_set_new(requestRoot, "pwd_id", json_string(shareId.c_str()));
    json_object_set_new(
        requestRoot, "passcode", json_string(share.accessCode.c_str()));
    json_object_set_new(
        requestRoot, "support_visit_limit_private_share", json_true());
    char* requestText = json_dumps(requestRoot, JSON_COMPACT);
    json_decref(requestRoot);
    if (requestText == nullptr)
    {
        ProviderResult<ShareSession> result;
        result.error.code = ProviderErrorCode::InvalidResponse;
        result.error.message = "Failed to create Quark token request";
        return result;
    }

    net::HttpRequest request;
    request.url = "https://drive-h.quark.cn/1/clouddrive/share/"
        "sharepage/token?pr=ucpro&fr=pc&uc_param_str=";
    request.method = "POST";
    request.body = requestText;
    free(requestText);
    request.headers = quarkHeaders(session_, share.url);
    request.cancel = &browseCancel_;
    request.connectTimeoutSeconds = 5;
    request.timeoutSeconds = 12;
    request.headers.push_back({"Content-Type", "application/json"});
    const net::HttpResponse response = httpClient_.get(request);
    if (!response.succeeded)
    {
        ProviderResult<ShareSession> result;
        result.error = responseError(response);
        return result;
    }

    json_error_t jsonError{};
    json_t* root = json_loadb(
        response.body.data(), response.body.size(), 0, &jsonError);
    json_t* data = root == nullptr ? nullptr : json_object_get(root, "data");
    const std::string stoken = json_is_object(data)
        ? jsonString(data, "stoken") : std::string();
    if (root == nullptr || !isSuccessfulResponse(root) || stoken.empty())
    {
        if (root != nullptr)
        {
            json_decref(root);
        }
        ProviderResult<ShareSession> result;
        result.error = responseError(response);
        return result;
    }

    json_decref(root);
    ProviderResult<ShareSession> result;
    result.succeeded = true;
    result.value.shareId = shareId;
    result.value.rootId = "0";
    result.value.opaqueState = stoken;
    return result;
}

ProviderResult<std::vector<CloudEntry>> QuarkProvider::listFiles(
    const ShareSession& session, const std::string& directoryId)
{
    ProviderResult<std::vector<CloudEntry>> result;
    if (session.shareId.empty() || session.opaqueState.empty())
    {
        result.error.code = ProviderErrorCode::InvalidResponse;
        result.error.message = "Incomplete Quark share session";
        return result;
    }

    const std::string parentId = directoryId.empty()
        ? session.rootId : directoryId;
    constexpr std::int64_t pageSize = 50;
    for (std::int64_t page = 1; ; ++page)
    {
        std::ostringstream url;
        url << "https://drive-h.quark.cn/1/clouddrive/share/"
            "sharepage/detail?pr=ucpro&fr=pc&uc_param_str=&ver=2"
            "&pwd_id=" << percentEncode(session.shareId)
            << "&stoken=" << percentEncode(session.opaqueState)
            << "&pdir_fid=" << percentEncode(parentId)
            << "&force=0&_page=" << page << "&_size=" << pageSize
            << "&_fetch_banner=" << (page == 1 && parentId == "0" ? 1 : 0)
            << "&_fetch_share=" << (page == 1 && parentId == "0" ? 1 : 0)
            << "&fetch_relate_conversation=0&_fetch_total=1"
            "&_sort=file_type%3Aasc%2Cfile_name%3Aasc";

        net::HttpRequest request;
        request.url = url.str();
        request.headers = quarkHeaders(
            session_, "https://pan.quark.cn/s/" + session.shareId);
        request.cancel = &browseCancel_;
        request.connectTimeoutSeconds = 5;
        request.timeoutSeconds = 12;
        const net::HttpResponse response = httpClient_.get(request);
        if (!response.succeeded)
        {
            result.error = responseError(response);
            return result;
        }

        json_error_t jsonError{};
        json_t* root = json_loadb(
            response.body.data(), response.body.size(), 0, &jsonError);
        json_t* data = root == nullptr ? nullptr : json_object_get(root, "data");
        json_t* list = json_is_object(data)
            ? json_object_get(data, "list") : nullptr;
        if (root == nullptr || !isSuccessfulResponse(root) || !json_is_array(list))
        {
            if (root != nullptr)
            {
                json_decref(root);
            }
            result.error = responseError(response);
            return result;
        }

        const std::size_t count = json_array_size(list);
        for (std::size_t index = 0; index < count; ++index)
        {
            json_t* item = json_array_get(list, index);
            const std::string id = jsonString(item, "fid");
            const std::string name = jsonString(item, "file_name");
            if (id.empty() || name.empty())
            {
                json_decref(root);
                result.value.clear();
                result.error.code = ProviderErrorCode::InvalidResponse;
                result.error.message = "Invalid Quark file entry";
                return result;
            }

            CloudEntry entry;
            entry.id = id;
            entry.shareToken = jsonString(item, "share_fid_token");
            entry.parentId = jsonString(item, "pdir_fid");
            entry.name = name;
            entry.type = json_is_true(json_object_get(item, "dir"))
                ? EntryType::Directory : EntryType::File;
            json_t* size = json_object_get(item, "size");
            if (json_is_integer(size) && json_integer_value(size) >= 0)
            {
                entry.size = static_cast<std::uint64_t>(json_integer_value(size));
            }
            json_t* duration = json_object_get(item, "duration");
            if (json_is_number(duration) && json_number_value(duration) > 0)
            {
                entry.durationSeconds = json_number_value(duration);
            }
            result.value.push_back(std::move(entry));
        }
        json_decref(root);
        if (count < static_cast<std::size_t>(pageSize))
        {
            break;
        }
    }

    result.succeeded = true;
    return result;
}

ProviderResult<PlaybackSource> QuarkProvider::getPlaybackSource(
    const ShareSession& share, const CloudEntry& file)
{
    ProviderResult<PlaybackSource> result;
    if (!isAuthenticated())
    {
        result.error.code = ProviderErrorCode::AuthenticationRequired;
        result.error.message = "请先登录夸克";
        return result;
    }
    if(playbackCancel_.load()){
        result.error.code=ProviderErrorCode::NetworkError;
        result.error.message="已取消播放地址解析";
        return result;
    }
    if (file.type != EntryType::File || share.shareId.empty()
        || share.opaqueState.empty() || file.id.empty() || file.shareToken.empty())
    {
        result.error.code = ProviderErrorCode::InvalidResponse;
        result.error.message = "分享文件缺少转存信息";
        return result;
    }

    auto postJson = [&](const std::string& url, json_t* body,
                        const std::string& referer) {
        char* text = json_dumps(body, JSON_COMPACT);
        net::HttpRequest request;
        request.url = url;
        request.method = "POST";
        request.body = text == nullptr ? std::string() : std::string(text);
        free(text);
        request.headers = quarkHeaders(session_, referer);
        request.headers.push_back({"Content-Type", "application/json; charset=utf-8"});
        request.cancel = &playbackCancel_;
        request.connectTimeoutSeconds = 5;
        request.timeoutSeconds = 15;
        return httpClient_.get(request);
    };
    auto parse = [](const net::HttpResponse& response) {
        json_error_t error{};
        return json_loadb(response.body.data(), response.body.size(), 0, &error);
    };
    auto fail = [&](const net::HttpResponse& response, json_t* root,
                    const char* fallback) {
        if (!response.succeeded)
        {
            result.error = responseError(response);
        }
        else
        {
            result.error.code = ProviderErrorCode::InvalidResponse;
            const std::string message = root == nullptr
                ? std::string() : jsonString(root, "message");
            result.error.message = message.empty() ? fallback : message;
        }
        if (root != nullptr) json_decref(root);
    };

    json_t* saveBody = json_object();
    json_t* ids = json_array();
    json_array_append_new(ids, json_string(file.id.c_str()));
    json_object_set_new(saveBody, "fid_list", ids);
    json_t* tokens = json_array();
    json_array_append_new(tokens, json_string(file.shareToken.c_str()));
    json_object_set_new(saveBody, "fid_token_list", tokens);
    json_object_set_new(saveBody, "to_pdir_fid", json_string("0"));
    json_object_set_new(saveBody, "pwd_id", json_string(share.shareId.c_str()));
    json_object_set_new(saveBody, "stoken", json_string(share.opaqueState.c_str()));
    json_object_set_new(saveBody, "pdir_fid", json_string("0"));
    json_object_set_new(saveBody, "scene", json_string("link"));
    const std::string shareReferer = "https://pan.quark.cn/s/" + share.shareId;
    net::HttpResponse response = postJson(
        "https://drive-pc.quark.cn/1/clouddrive/share/sharepage/save?pr=ucpro&fr=pc",
        saveBody, shareReferer);
    json_decref(saveBody);
    json_t* root = parse(response);
    if (!response.succeeded || root == nullptr || !isSuccessfulResponse(root))
    {
        fail(response, root, "夸克转存失败");
        return result;
    }
    json_t* data = json_object_get(root, "data");
    const std::string taskId = jsonString(data, "task_id");
    json_decref(root);
    if (taskId.empty())
    {
        result.error.code = ProviderErrorCode::InvalidResponse;
        result.error.message = "夸克未返回转存任务";
        return result;
    }

    std::string savedId;
    for (int retry = 0; retry < 20 && savedId.empty()&&!playbackCancel_.load(); ++retry)
    {
        std::ostringstream taskUrl;
        taskUrl << "https://drive-pc.quark.cn/1/clouddrive/task?pr=ucpro&fr=pc"
            << "&task_id=" << percentEncode(taskId)
            << "&retry_index=" << retry;
        net::HttpRequest request;
        request.url = taskUrl.str();
        request.headers = quarkHeaders(session_, "https://pan.quark.cn/");
        request.cancel = &playbackCancel_;
        request.connectTimeoutSeconds = 5;
        request.timeoutSeconds = 15;
        response = httpClient_.get(request);
        root = parse(response);
        if (!response.succeeded || root == nullptr || !isSuccessfulResponse(root))
        {
            fail(response, root, "夸克转存任务查询失败");
            return result;
        }
        data = json_object_get(root, "data");
        json_t* status = json_object_get(data, "status");
        if (json_is_integer(status) && json_integer_value(status) == 2)
        {
            json_t* saveAs = json_object_get(data, "save_as");
            json_t* saved = json_object_get(saveAs, "save_as_top_fids");
            json_t* first = json_is_array(saved) ? json_array_get(saved, 0) : nullptr;
            if (json_is_string(first)) savedId = json_string_value(first);
        }
        json_decref(root);
        if (savedId.empty()) for(int wait=0;wait<5&&!playbackCancel_.load();++wait)SDL_Delay(100);
    }
    if (savedId.empty())
    {
        result.error.code = ProviderErrorCode::InvalidResponse;
        result.error.message = "夸克转存超时";
        return result;
    }

    std::string url;
    std::vector<std::string> hlsUrls;
    std::vector<std::uint64_t> hlsSizes;
    std::vector<double> hlsDurations;
    double hlsDuration = 0.0;
    std::string playbackAddressError;
    auto probePlaybackUrl = [&](const std::string& candidate) {
        if (candidate.empty()) return false;
        net::HttpRequest request;
        request.url = candidate;
        request.headers = quarkHeaders(session_, "https://pan.quark.cn/");
        request.headers.push_back({"Range", "bytes=0-131071"});
        request.cancel = &playbackCancel_;
        request.connectTimeoutSeconds = 5;
        request.timeoutSeconds = 10;
        request.maxResponseBytes = 132 * 1024;
        const net::HttpResponse probe = httpClient_.get(request);
        if (probe.body.rfind("#EXTM3U", 0) == 0)
        {
            net::HttpRequest full;
            full.url = candidate;
            full.headers = quarkHeaders(session_, "https://pan.quark.cn/");
            full.cancel = &playbackCancel_;
            full.connectTimeoutSeconds = 5;
            full.timeoutSeconds = 12;
            full.maxResponseBytes = 1024 * 1024;
            const net::HttpResponse complete = httpClient_.get(full);
            if (!complete.succeeded || complete.body.rfind("#EXTM3U", 0) != 0)
            {
                playbackAddressError = complete.error;
                return false;
            }
            std::string playlistBody = complete.body;
            std::string playlistUrl = complete.effectiveUrl.empty() ? candidate : complete.effectiveUrl;
            const std::string variant = selectPlaylistVariant(
                playlistBody, playlistUrl, preferredMaxHeight_);
            if (!variant.empty())
            {
                net::HttpRequest child;
                child.url = variant;
                child.headers = quarkHeaders(session_, "https://pan.quark.cn/");
                child.cancel = &playbackCancel_;
                child.connectTimeoutSeconds = 5;
                child.timeoutSeconds = 10;
                child.maxResponseBytes = 1024 * 1024;
                const net::HttpResponse media = httpClient_.get(child);
                if (!media.succeeded || media.body.rfind("#EXTM3U", 0) != 0)
                {
                    playbackAddressError = media.error;
                    return false;
                }
                playlistBody = media.body;
                playlistUrl = media.effectiveUrl.empty() ? variant : media.effectiveUrl;
            }
            hlsUrls.clear(); hlsSizes.clear(); hlsDurations.clear(); hlsDuration = 0.0;
            if (parseMediaPlaylist(playlistBody, playlistUrl,
                                   hlsUrls, hlsSizes, hlsDurations, hlsDuration))
            {
                url = playlistUrl;
                return true;
            }
            playbackAddressError = "夸克转码清单不受支持";
            return false;
        }
        if (looksLikeMedia(probe.body)) return true;
        playbackAddressError = responseMessage(probe);
        if (playbackAddressError.empty()) playbackAddressError = probe.error;
        return false;
    };

    // Ask Quark for its server-side transcode first. The original links in the
    // catalogue are commonly 2160p60 10-bit HEVC, which is too expensive for
    // smooth software decoding. Prefer 1080p/720p fMP4 and retain the original
    // download URL as a compatibility fallback.
    json_t* playBody = json_object();
    json_object_set_new(playBody, "fid", json_string(savedId.c_str()));
    json_object_set_new(playBody, "resolutions",
                        json_string(preferredMaxHeight_>=1080?"super,high,normal,low":"high,normal,low"));
    json_object_set_new(playBody, "supports", json_string("fmp4,m3u8"));
    response = postJson(
        "https://drive-pc.quark.cn/1/clouddrive/file/v2/play?pr=ucpro&fr=pc",
        playBody, "https://pan.quark.cn/");
    json_decref(playBody);
    root = parse(response);
    if (response.succeeded && root != nullptr && isSuccessfulResponse(root))
    {
        TranscodedStream stream;
        findTranscodedStream(json_object_get(root, "data"), 0, stream,preferredMaxHeight_);
        if (probePlaybackUrl(stream.url)) url = std::move(stream.url);
    }
    if (root != nullptr) json_decref(root);

    if (url.empty())
    {
        json_t* downloadBody = json_object();
        ids = json_array();
        json_array_append_new(ids, json_string(savedId.c_str()));
        json_object_set_new(downloadBody, "fids", ids);
        response = postJson(
            "https://drive-pc.quark.cn/1/clouddrive/file/download?pr=ucpro&fr=pc&uc_param_str=",
            downloadBody, "https://pan.quark.cn/");
        json_decref(downloadBody);
        root = parse(response);
        if (!response.succeeded || root == nullptr || !isSuccessfulResponse(root))
        {
            fail(response, root, "夸克直链解析失败");
            return result;
        }
        data = json_object_get(root, "data");
        json_t* first = json_is_array(data) ? json_array_get(data, 0) : nullptr;
        const std::string downloadUrl = jsonString(first, "download_url");
        json_decref(root);
        if (probePlaybackUrl(downloadUrl)) url = downloadUrl;
    }
    if (url.empty())
    {
        PlaybackSource temporary;
        temporary.temporaryId = savedId;
        std::string ignoredCleanupError;
        cleanupPlaybackSource(temporary, ignoredCleanupError);
        result.error.code = ProviderErrorCode::InvalidResponse;
        result.error.message = playbackAddressError.empty()
            ? "夸克未返回可用的播放地址"
            : "夸克播放地址不可用：" + playbackAddressError;
        return result;
    }

    result.succeeded = true;
    result.value.url = url;
    result.value.segmentUrls = std::move(hlsUrls);
    result.value.segmentSizes = std::move(hlsSizes);
    result.value.segmentDurations = std::move(hlsDurations);
    if (!result.value.segmentUrls.empty())
    {
        result.value.durationSeconds = hlsDuration;
        result.value.initialBufferSegments = 3;
        result.value.parallelFetches = 3;
    }
    result.value.temporaryId = savedId;
    if (result.value.durationSeconds <= 0.0) result.value.durationSeconds = file.durationSeconds;
    result.value.headers.push_back({"Cookie", session_.cookie});
    result.value.headers.push_back({"User-Agent", session_.userAgent});
    result.value.headers.push_back({"Referer", "https://pan.quark.cn/"});
    return result;
}

bool QuarkProvider::cleanupPlaybackSource(
    const PlaybackSource& source, std::string& error)
{
    if (source.temporaryId.empty()) return true;
    auto requestJson = [&](const std::string& url, json_t* body) {
        char* text = json_dumps(body, JSON_COMPACT);
        net::HttpRequest request;
        request.url = url;
        request.method = "POST";
        request.body = text == nullptr ? std::string() : std::string(text);
        free(text);
        request.headers = quarkHeaders(session_, "https://pan.quark.cn/");
        request.headers.push_back({"Content-Type", "application/json; charset=utf-8"});
        request.timeoutSeconds = 30;
        return httpClient_.get(request);
    };
    auto parse = [](const net::HttpResponse& response) {
        json_error_t parseError{};
        return json_loadb(response.body.data(), response.body.size(), 0, &parseError);
    };

    json_t* body = json_object();
    json_object_set_new(body, "action_type", json_integer(2));
    json_t* files = json_array();
    json_array_append_new(files, json_string(source.temporaryId.c_str()));
    json_object_set_new(body, "filelist", files);
    json_object_set_new(body, "exclude_fids", json_array());
    net::HttpResponse response = requestJson(
        "https://drive-pc.quark.cn/1/clouddrive/file/delete?pr=ucpro&fr=pc&uc_param_str=",
        body);
    json_decref(body);
    json_t* root = parse(response);
    if (!response.succeeded || root == nullptr || !isSuccessfulResponse(root))
    {
        error = response.error.empty() ? "临时文件删除失败" : response.error;
        if (root != nullptr) json_decref(root);
        return false;
    }
    std::string taskId = jsonString(json_object_get(root, "data"), "task_id");
    json_decref(root);
    if (!taskId.empty())
    {
        for (int retry = 0; retry < 10; ++retry)
        {
            std::ostringstream url;
            url << "https://drive-pc.quark.cn/1/clouddrive/task?pr=ucpro&fr=pc"
                << "&task_id=" << percentEncode(taskId)
                << "&retry_index=" << retry;
            net::HttpRequest request;
            request.url = url.str();
            request.headers = quarkHeaders(session_, "https://pan.quark.cn/");
            response = httpClient_.get(request);
            root = parse(response);
            json_t* status = root == nullptr ? nullptr
                : json_object_get(json_object_get(root, "data"), "status");
            const bool complete = json_is_integer(status)
                && json_integer_value(status) == 2;
            if (root != nullptr) json_decref(root);
            if (complete) break;
            SDL_Delay(300);
        }
    }

    std::string recordId;
    for (int retry = 0; retry < 5 && recordId.empty(); ++retry)
    {
        net::HttpRequest request;
        request.url = "https://drive-pc.quark.cn/1/clouddrive/file/recycle/list"
            "?pr=ucpro&fr=pc&uc_param_str=&_page=1&_size=100&_fetch_total=1";
        request.headers = quarkHeaders(session_, "https://pan.quark.cn/");
        response = httpClient_.get(request);
        root = parse(response);
        json_t* list = root == nullptr ? nullptr
            : json_object_get(json_object_get(root, "data"), "list");
        if (json_is_array(list))
        {
            for (std::size_t index = 0; index < json_array_size(list); ++index)
            {
                json_t* item = json_array_get(list, index);
                if (jsonString(item, "fid") == source.temporaryId)
                {
                    recordId = jsonString(item, "record_id");
                    break;
                }
            }
        }
        if (root != nullptr) json_decref(root);
        if (recordId.empty()) SDL_Delay(300);
    }
    if (recordId.empty())
    {
        error = "临时文件已删除，但回收站清理失败";
        return false;
    }

    body = json_object();
    json_t* records = json_array();
    json_array_append_new(records, json_string(recordId.c_str()));
    json_object_set_new(body, "record_list", records);
    response = requestJson(
        "https://drive-pc.quark.cn/1/clouddrive/file/recycle/remove?pr=ucpro&fr=pc&uc_param_str=",
        body);
    json_decref(body);
    root = parse(response);
    const bool removed = response.succeeded && root != nullptr
        && isSuccessfulResponse(root);
    if (root != nullptr) json_decref(root);
    if (!removed)
    {
        error = response.error.empty() ? "回收站清理失败" : response.error;
        return false;
    }
    return true;
}
}
