#include "okswitch/media/MiguSportsProvider.hpp"
#include "okswitch/media/MiguSigner.hpp"

#include <jansson.h>
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <sstream>

namespace okswitch::media
{
namespace
{
constexpr const char* kApi = "https://webapi.miguvideo.com/gateway";
constexpr const char* kAgent = "Mozilla/5.0";
constexpr const char* kFeaturedPage = "16ed73096e0244d1ba1034d973a020fe";
constexpr const char* kFeaturedSchedule = "dd3925ee0ae443a29df32b56fc283b79";

std::string field(json_t* object, const char* key)
{
    json_t* value = json_object_get(object, key);
    if (json_is_string(value)) return json_string_value(value);
    if (json_is_integer(value)) return std::to_string(json_integer_value(value));
    return {};
}

json_t* fetch(net::HttpClient& http, const std::string& url, std::string& error, const std::atomic<bool>* cancel = nullptr)
{
    net::HttpRequest request;
    request.cancel = cancel;
    request.url = url;
    request.connectTimeoutSeconds = 7;
    request.timeoutSeconds = 16;
    request.maxResponseBytes = 2 * 1024 * 1024;
    request.headers = {{"User-Agent", kAgent}, {"Referer", "https://www.miguvideo.com/"}};
    auto response = http.get(request);
    if (!response.succeeded || response.statusCode != 200)
    {
        error = response.error.empty() ? "咪咕请求失败 HTTP " + std::to_string(response.statusCode) : response.error;
        return nullptr;
    }
    json_error_t parseError{};
    json_t* root = json_loadb(response.body.data(), response.body.size(), 0, &parseError);
    if (!root) error = "咪咕数据解析失败";
    return root;
}

std::string chinaNow()
{
    const std::time_t now = std::time(nullptr) + 8 * 3600;
    std::tm calendar{};
    const std::tm* china = gmtime_r(&now, &calendar);
    if (!china) return {};
    char value[16]{};
    std::strftime(value, sizeof(value), "%Y%m%d%H%M", china);
    return value;
}

std::string chinaTimeFromMilliseconds(json_t* value)
{
    if (!json_is_integer(value)) return {};
    const json_int_t milliseconds = json_integer_value(value);
    if (milliseconds <= 0) return {};
    const std::time_t seconds = static_cast<std::time_t>(milliseconds / 1000 + 8 * 3600);
    std::tm calendar{};
    const std::tm* china = gmtime_r(&seconds, &calendar);
    if (!china) return {};
    char formatted[16]{};
    std::strftime(formatted, sizeof(formatted), "%Y%m%d%H%M", china);
    return formatted;
}

bool footballCompetition(const std::string& name)
{
    static const char* terms[] = {"足球", "中国之队", "中超", "英超联赛", "英格兰足球",
        "西甲", "意甲", "德甲", "法甲", "欧冠", "亚冠", "足协杯", "欧联", "美职联", "沙特联"};
    for (const char* term : terms) if (name.find(term) != std::string::npos) return true;
    return false;
}

json_t* playInfo(net::HttpClient& http, const MiguSession& session,
                 const std::string& id, std::string& error, const std::atomic<bool>* cancel = nullptr,
                 int rateType=3)
{
    net::HttpRequest request;request.cancel=cancel;request.closeConnection=true;
    request.url = std::string(kApi) + "/playurl/v3/play/playurl?contId=" + id
        + "&rateType="+std::to_string(rateType)+"&startPlay=true&xh265=false&chip=mgwww&channelId=0132_10010001005";
    request.connectTimeoutSeconds = 7;
    request.timeoutSeconds = 16;
    request.maxResponseBytes = 512 * 1024;
    request.headers = {{"User-Agent", kAgent}, {"Referer", "https://www.miguvideo.com/"},
        {"Accept-Encoding","identity"}, {"appId", "miguvideo"}, {"terminalId", "www"}};
    if (session.valid())
    {
        // Match the official migu_login_callback: signed userInfo is the
        // whole original response object; the application ID is not deviceId.
        request.headers.push_back({"channel", "H5"});
        request.headers.push_back({"userId", session.userId});
        request.headers.push_back({"userToken", session.userToken});
        request.headers.push_back({"userInfo", session.userInfoJson});
        request.headers.push_back({"sign", session.sign});
        request.headers.push_back({"clientId", "27fb3129-5a54-45bc-8af1-7dc8f1155501"});
        request.headers.push_back({"SDKCEId", "27fb3129-5a54-45bc-8af1-7dc8f1155501"});

    }
    const auto response = http.get(request);
    json_t* root = nullptr;
    if (response.succeeded && response.statusCode == 200)
    {
        json_error_t parseError{};
        root = json_loadb(response.body.data(), response.body.size(), 0, &parseError);
    }
    if (!root) error = response.error.empty()
        ? "咪咕播放地址请求失败 HTTP " + std::to_string(response.statusCode) : response.error;
    return root;
}

std::string absoluteUrl(const std::string& base, const std::string& path)
{
    if (path.rfind("https://", 0) == 0 || path.rfind("http://", 0) == 0) return path;
    const size_t scheme = base.find("://");
    if (scheme == std::string::npos) return {};
    const size_t hostEnd = base.find('/', scheme + 3);
    const std::string origin = hostEnd == std::string::npos ? base : base.substr(0, hostEnd);
    if (path.rfind("//", 0) == 0) return base.substr(0, scheme + 1) + path;
    if (!path.empty() && path[0] == '/') return origin + path;
    const size_t query = base.find('?');
    const size_t slash = base.rfind('/', query == std::string::npos ? std::string::npos : query);
    return (slash == std::string::npos ? origin + "/" : base.substr(0, slash + 1)) + path;
}

bool parsePlaylist(const std::string& body, const std::string& url, cloud::PlaybackSource& source)
{
    if (body.find("#EXT-X-KEY:") != std::string::npos) return false;
    std::istringstream lines(body);
    std::string line;
    double pending = 0;
    while (std::getline(lines, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("#EXTINF:", 0) == 0) { pending = std::strtod(line.c_str() + 8, nullptr); continue; }
        if (pending <= 0 || line.empty() || line[0] == '#') continue;
        std::string segment = absoluteUrl(url, line);
        // The official web player fetches the signed manifest from h5vod but
        // routes its media fragments through the mgsp-hs CDN. Keeping the
        // manifest host for fragments results in HTTP 661 on that CDN.
        const std::string manifestHost = "h5vod.gslb.cmvideo.cn";
        const size_t host = segment.find(manifestHost);
        if (host != std::string::npos)
            segment.replace(host, manifestHost.size(), "mgsp-hs.vod.miguvideo.com");
        source.segmentUrls.push_back(std::move(segment));
        source.segmentSizes.push_back(0);
        source.segmentDurations.push_back(pending);
        source.durationSeconds += pending;
        pending = 0;
    }
    return !source.segmentUrls.empty();
}
}

MiguResult<MiguMatch> MiguSportsProvider::matches(const std::string& anchor, const std::string& direction, const std::atomic<bool>* cancel) const
{
    MiguResult<MiguMatch> result;
    const std::string today = chinaNow().substr(0, 8);
    if (today.size() != 8) { result.error = "系统日期不可用"; return result; }
    if (direction.empty()) {
        // Both directions exclude the anchor. Down from yesterday includes today.
        std::time_t yesterday = std::time(nullptr) + 8 * 3600 - 86400;
        std::tm calendar{}; gmtime_r(&yesterday, &calendar);
        char previous[16]{}; std::strftime(previous, sizeof(previous), "%Y%m%d", &calendar);
        auto past = matches(today, "up", cancel);
        auto future = matches(previous, "down", cancel);
        if (!past.succeeded && !future.succeeded) {
            result.error = past.error.empty() ? future.error : past.error; return result;
        }
        if (past.succeeded) result.items = std::move(past.items);
        if (future.succeeded)
            result.items.insert(result.items.end(), future.items.begin(), future.items.end());
    } else {
        if (anchor.size() != 8 || anchor.find_first_not_of("0123456789") != std::string::npos
            || (direction != "up" && direction != "down")) { result.error = "赛程日期无效"; return result; }
        const std::string url = std::string(kApi) + "/vms-match/v6/staticcache/basic/match-list/normal-match-list/"
            + anchor + "/1/" + direction + "/1/miguvideo/";
        json_t* root = fetch(http_, url, result.error, cancel);
        if (!root) return result;
        json_t* list = json_object_get(json_object_get(root, "body"), "matchList");
        if (!json_is_object(list)) { result.error = "咪咕赛事列表格式有误"; json_decref(root); return result; }
        const char* key; json_t* games;
        json_object_foreach(list, key, games) {
            if (!json_is_array(games)) continue;
            for (size_t i = 0; i < json_array_size(games); ++i) {
                json_t* game = json_array_get(games, i);
                json_t* teams = json_object_get(game, "confrontTeams");
                if (!json_is_array(teams) || json_array_size(teams) < 2) continue;
                json_t* a = json_array_get(teams, 0); json_t* b = json_array_get(teams, 1);
                if (field(a, "isHome") == "0" && field(b, "isHome") == "1") std::swap(a, b);
                MiguMatch match;
                match.id = field(game, "mgdbId"); match.day = key;
                match.kickoffTime = chinaTimeFromMilliseconds(json_object_get(game, "matchStartTime"));
                if (match.kickoffTime.empty()) match.kickoffTime = chinaTimeFromMilliseconds(json_object_get(game, "startTime"));
                match.date = match.day.size() == 8 ? match.day.substr(4,2) + "/" + match.day.substr(6,2) : match.day;
                if (match.kickoffTime.size() >= 12) match.date += " " + match.kickoffTime.substr(8,2) + ":" + match.kickoffTime.substr(10,2);
                match.home = field(a, "name"); match.away = field(b, "name");
                match.homeId = field(a, "id"); match.awayId = field(b, "id");
                match.homeLogo = field(a, "image"); match.awayLogo = field(b, "image");
                const auto state = field(game, "matchStatus");
                match.status = state == "2" ? 2 : state == "1" ? 1 : 0;
                match.liveId = field(game, "pID");
                match.score = match.status == 0 ? "VS" : field(a, "score") + ":" + field(b, "score");
                if (!match.id.empty()) result.items.push_back(std::move(match));
            }
        }
        json_decref(root);
    }
    std::stable_sort(result.items.begin(), result.items.end(), [](const auto& a, const auto& b) {
        return a.kickoffTime == b.kickoffTime ? a.id < b.id : a.kickoffTime < b.kickoffTime;
    });
    result.items.erase(std::unique(result.items.begin(), result.items.end(), [](const auto& a, const auto& b) {return a.id == b.id;}), result.items.end());
    result.succeeded = true; return result;
}

MiguResult<MiguClip> MiguSportsProvider::clips(const MiguMatch& match, const std::atomic<bool>* cancel) const
{
    MiguResult<MiguClip> result;
    if (match.id.empty()) { result.error = "赛事编号缺失"; return result; }
    const std::string url = std::string(kApi) + "/vms-match/v5/staticcache/basic/all-view-list/"
        + match.id + "/2/miguvideo";
    // Avoid compressed keep-alive responses hanging on the console TLS stack.
    net::HttpRequest request;request.url=url;request.cancel=cancel;
    request.closeConnection=true;request.connectTimeoutSeconds=5;request.timeoutSeconds=10;
    request.maxResponseBytes=512*1024;
    request.headers={{"User-Agent",kAgent},{"Referer","https://www.miguvideo.com/"},{"Accept-Encoding","identity"},{"Connection","close"}};
    net::HttpResponse response;
    for(int attempt=0;attempt<2;++attempt){
        if(cancel&&cancel->load()){result.error="已取消加载";return result;}
        response=http_.get(request);
        if(response.succeeded||response.error.find("Timeout")==std::string::npos)break;
    }
    if(cancel&&cancel->load()){result.error="已取消加载";return result;}
    if(!response.succeeded){result.error=response.error;return result;}
    json_error_t parseError{};
    json_t* root=json_loadb(response.body.data(),response.body.size(),0,&parseError);
    if(!root)result.error="咪咕赛事视频列表数据不完整";
    if (!root) return result;
    json_t* body = json_object_get(root, "body");
    // The official match page puts the full highlights and complete replays in replayList.
    json_t* clips = json_object_get(body, "replayList");
    if(!json_is_array(clips)){json_decref(root);result.error="咪咕未返回赛事视频列表";return result;}
    if (json_is_array(clips)) for (size_t i = 0; i < json_array_size(clips); ++i)
    {
        json_t* item = json_array_get(clips, i);
        MiguClip clip{field(item, "pID"), field(item, "name"), field(item, "duration")};
        if (!clip.id.empty() && !clip.title.empty()) result.items.push_back(std::move(clip));
    }
    json_decref(root);
    result.succeeded = true;
    return result;
}

MiguResult<MiguLiveMatch> MiguSportsProvider::featuredFootball() const
{
    MiguResult<MiguLiveMatch> result;
    net::HttpRequest request;
    request.url = std::string(kApi) + "/display/v3/static/" + kFeaturedPage + "/" + kFeaturedSchedule;
    request.connectTimeoutSeconds = 7;
    request.timeoutSeconds = 18;
    request.maxResponseBytes = 2 * 1024 * 1024;
    request.headers = {{"User-Agent", kAgent}, {"Referer", "https://www.miguvideo.com/"}};
    const auto response = http_.get(request);
    if (!response.succeeded || response.statusCode != 200)
    {
        result.error = response.error.empty() ? "精选赛事请求失败 HTTP "
            + std::to_string(response.statusCode) : response.error;
        return result;
    }
    json_error_t parseError{};
    json_t* root = json_loadb(response.body.data(), response.body.size(), 0, &parseError);
    if (!root) { result.error = "精选赛事数据解析失败"; return result; }
    json_t* components = json_object_get(json_object_get(root, "body"), "components");
    json_t* list = nullptr;
    if (json_is_array(components)) for (size_t i = 0; i < json_array_size(components); ++i)
    {
        json_t* component = json_array_get(components, i);
        if (field(component, "name").find("赛事列表") == std::string::npos) continue;
        json_t* data = json_object_get(component, "data");
        if (json_is_array(data) && (!list || json_array_size(data) > json_array_size(list))) list = data;
    }
    if (!list) { json_decref(root); result.error = "官网暂未提供精选赛事列表"; return result; }
    const std::string now = chinaNow();
    if (now.empty()) { json_decref(root); result.error = "系统日期不可用"; return result; }
    for (size_t i = 0; i < json_array_size(list); ++i)
    {
        json_t* item = json_array_get(list, i);
        const std::string competition = field(item, "competitionName");
        if (!footballCompetition(competition) || field(item, "programTypeV2") != "LIVE") continue;
        MiguLiveMatch match;
        match.id = field(item, "liveId");
        match.title = field(item, "name");
        match.competition = competition;
        match.startTime = field(item, "startTime");
        match.kickoffTime = chinaTimeFromMilliseconds(json_object_get(item, "matchStartTime"));
        if (match.kickoffTime.empty()) match.kickoffTime = match.startTime;
        match.endTime = field(item, "endTime");
        if (match.id.empty() || match.title.empty() || match.startTime.size() < 12) continue;
        if (match.endTime.size() >= 12 && match.endTime < now) continue;
        match.live = match.startTime <= now;
        result.items.push_back(std::move(match));
    }
    json_decref(root);
    // The curated display page omits some live competitions (including ACL Two).
    // Merge the website's all-sports schedule, which carries their live IDs.
    const std::time_t yesterday = std::time(nullptr) + 8 * 3600 - 86400;
    std::tm calendar{};
    gmtime_r(&yesterday, &calendar);
    char anchor[16]{};
    std::strftime(anchor, sizeof(anchor), "%Y%m%d", &calendar);
    std::string scheduleError;
    const std::string scheduleUrl = std::string(kApi)
        + "/vms-match/v6/staticcache/basic/match-list/normal-match-list/"
        + anchor + "/all/down/1/miguvideo/";
    json_t* schedule = fetch(http_, scheduleUrl, scheduleError);
    if (schedule)
    {
        json_t* days = json_object_get(json_object_get(schedule, "body"), "matchList");
        if (json_is_object(days))
        {
            const char* day;
            json_t* games;
            json_object_foreach(days, day, games)
            {
                if (std::string(day) < now.substr(0, 8) || !json_is_array(games)) continue;
                for (size_t i = 0; i < json_array_size(games); ++i)
                {
                    json_t* game = json_array_get(games, i);
                    const std::string competition = field(game, "title");
                    const std::string id = field(game, "pID");
                    if (!footballCompetition(competition) || id.empty()
                        || field(game, "matchStatus") == "2") continue;
                    if (std::any_of(result.items.begin(), result.items.end(),
                        [&](const MiguLiveMatch& existing) { return existing.id == id; })) continue;
                    json_t* teams = json_object_get(game, "confrontTeams");
                    if (!json_is_array(teams) || json_array_size(teams) < 2) continue;
                    json_t* home = json_array_get(teams, 0);
                    json_t* away = json_array_get(teams, 1);
                    if (field(home, "isHome") == "0" && field(away, "isHome") == "1")
                        std::swap(home, away);
                    MiguLiveMatch match;
                    match.id = id;
                    match.title = field(home, "name") + " VS " + field(away, "name");
                    match.competition = competition;
                    match.kickoffTime = chinaTimeFromMilliseconds(json_object_get(game, "matchStartTime"));
                    match.startTime = match.kickoffTime;
                    match.live = field(game, "matchStatus") == "1" || match.startTime <= now;
                    if (!match.title.empty() && match.startTime.size() >= 12)
                        result.items.push_back(std::move(match));
                }
            }
        }
        json_decref(schedule);
    }
    std::stable_sort(result.items.begin(), result.items.end(), [](const auto& a, const auto& b) {
        if (a.live != b.live) return a.live;
        return a.startTime < b.startTime;
    });
    if (result.items.size() > 60) result.items.resize(60);
    result.succeeded = true;
    return result;
}

cloud::ProviderResult<cloud::PlaybackSource> MiguSportsProvider::playback(const MiguClip& clip, const std::atomic<bool>* cancel) const
{
    cloud::ProviderResult<cloud::PlaybackSource> result;
    if (clip.id.empty())
    {
        result.error = {cloud::ProviderErrorCode::InvalidResponse, "视频编号缺失"};
        return result;
    }
    std::string error;
    json_t* root = playInfo(http_, session_, clip.id, error, cancel);
    if (!root)
    {
        result.error = {cloud::ProviderErrorCode::NetworkError, error};
        return result;
    }
    json_t* body = json_object_get(root, "body");
    json_t* info = json_object_get(body, "urlInfo");
    const std::string mediaUrl = field(info, "url");
    const std::string auth = field(json_object_get(body, "auth"), "authResult");
    const bool loggedIn=json_is_true(json_object_get(json_object_get(body,"auth"),"logined"));
    json_decref(root);
    if (mediaUrl.empty())
    {
        result.error = {auth == "FAIL" && !loggedIn ? cloud::ProviderErrorCode::AuthenticationRequired : cloud::ProviderErrorCode::Unsupported,
            auth == "FAIL" ? (loggedIn ? "当前账号没有此节目的播放权限" : "此节目需要扫码登录") : "咪咕未提供播放地址"};
        return result;
    }
    std::string signedUrl;
    if (!signMiguPlaylist(http_, mediaUrl, signedUrl, error, cancel))
    {
        result.error = {cloud::ProviderErrorCode::Unsupported, error};
        return result;
    }
    if(cancel&&cancel->load()){result.error={cloud::ProviderErrorCode::NetworkError,"已取消获取视频"};return result;}
    net::HttpRequest probe;probe.cancel=cancel;probe.closeConnection=true;
    probe.url = signedUrl;
    probe.connectTimeoutSeconds = 5;
    probe.timeoutSeconds = 8;
    probe.maxResponseBytes = 4 * 1024 * 1024;
    probe.headers = {{"Accept-Encoding","identity"},{"User-Agent", kAgent}, {"Referer", "https://www.miguvideo.com/"}};
    auto checked = http_.get(probe);
    if (!checked.succeeded || checked.statusCode != 200 || checked.body.find("#EXTM3U") == std::string::npos)
    {
        const std::string reason = !checked.error.empty() ? checked.error
            : "HTTP " + std::to_string(checked.statusCode);
        const bool denied = checked.statusCode == 401 || checked.statusCode == 403;
        result.error = {denied ? cloud::ProviderErrorCode::AuthenticationRequired
                               : cloud::ProviderErrorCode::NetworkError,
            denied ? "咪咕播放授权失效，请重新登录 (" + reason + ")"
                   : "咪咕播放清单下载失败 (" + reason + ")"};
        return result;
    }
    const std::string playlistUrl = checked.effectiveUrl.empty() ? signedUrl : checked.effectiveUrl;
    if (!parsePlaylist(checked.body, playlistUrl, result.value))
    {
        result.error = {cloud::ProviderErrorCode::Unsupported, "咪咕视频使用加密分片或不支持的播放清单"};
        return result;
    }
    result.value.url = playlistUrl;
    result.value.parallelFetches = 3;
    result.value.initialBufferSegments = 3;
    result.value.headers = {{"User-Agent", kAgent}, {"Referer", "https://www.miguvideo.com/"}};
    result.succeeded = true;
    return result;
}

cloud::ProviderResult<cloud::PlaybackSource> MiguSportsProvider::playbackLive(
    const MiguLiveMatch& match,int rateType) const
{
    cloud::ProviderResult<cloud::PlaybackSource> result;
    if (match.id.empty())
    {
        result.error = {cloud::ProviderErrorCode::InvalidResponse, "直播编号缺失"};
        return result;
    }
    if (!match.live && match.startTime > chinaNow())
    {
        result.error = {cloud::ProviderErrorCode::Unsupported, "比赛尚未开始"};
        return result;
    }
    std::string error;
    if(rateType!=2&&rateType!=3&&rateType!=4){result.error={cloud::ProviderErrorCode::Unsupported,"不支持该画质"};return result;}
    json_t* root = playInfo(http_, session_, match.id, error,nullptr,rateType);
    if (!root)
    {
        result.error = {cloud::ProviderErrorCode::NetworkError, error};
        return result;
    }
    json_t* body = json_object_get(root, "body");
    json_t* info = json_object_get(body, "urlInfo");
    json_t* authInfo = json_object_get(body, "auth");
    const std::string mediaUrl = field(info, "url");
    const std::string auth = field(authInfo, "authResult");
    const bool loggedIn = json_is_true(json_object_get(authInfo, "logined"))
        ||field(authInfo,"logined")=="true"||field(authInfo,"logined")=="1";
    const std::string authDescription=field(authInfo,"resultDesc");
    const std::string playbackCode=field(info,"resultCode");
    const int trialSeconds = std::max(0, std::atoi(field(info, "trySeeDuration").c_str()));
    json_decref(root);
    if (mediaUrl.empty())
    {
        const bool needsLogin = !loggedIn && (auth == "FAIL" || session_.valid());
        result.error = {needsLogin ? cloud::ProviderErrorCode::AuthenticationRequired
                                   : cloud::ProviderErrorCode::Unsupported,
            needsLogin ? (session_.valid()?"咪咕未认可当前登录，请重新扫码":"此场直播需要扫码登录")
                : !authDescription.empty()?"咪咕直播鉴权："+authDescription
                : auth=="FAIL"?"咪咕已识别登录，但未授予本场直播播放权限"
                : "咪咕未返回直播地址"+(playbackCode.empty()?std::string():"（"+playbackCode+"）")};
        return result;
    }
    std::string signedUrl;
    if (!signMiguPlaylist(http_, mediaUrl, signedUrl, error))
    {
        result.error = {cloud::ProviderErrorCode::Unsupported, error};
        return result;
    }
    net::HttpRequest probe;
    probe.url = signedUrl;
    probe.connectTimeoutSeconds = 6;
    probe.timeoutSeconds = 12;
    probe.maxResponseBytes = 256 * 1024;
    probe.headers = {{"User-Agent", kAgent}, {"Referer", "https://www.miguvideo.com/"}};
    const auto checked = http_.get(probe);
    if (!checked.succeeded || checked.statusCode != 200
        || checked.body.find("#EXTM3U") == std::string::npos)
    {
        const bool denied = checked.statusCode == 401 || checked.statusCode == 403;
        const std::string reason = checked.error.empty() ? "HTTP " + std::to_string(checked.statusCode)
            : checked.error;
        result.error = {denied && !loggedIn ? cloud::ProviderErrorCode::AuthenticationRequired
                                            : cloud::ProviderErrorCode::NetworkError,
            denied && loggedIn ? "当前账号可能没有该场直播权限 (" + reason + ")"
                : "咪咕直播清单不可用 (" + reason + ")"};
        return result;
    }
    result.value.url = std::move(signedUrl);
    result.value.headers = {{"User-Agent", kAgent}, {"Referer", "https://www.miguvideo.com/"}};
    if (trialSeconds > 0)
        result.value.accessNotice = "免费试看约 " + std::to_string((trialSeconds + 59) / 60) + " 分钟";
    result.succeeded = true;
    return result;
}
}
