#include "okswitch/media/FootballProvider.hpp"

#include <switch.h>
#include <jansson.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <vector>

namespace okswitch::media
{
namespace
{
constexpr char kKey[] = "zNuZTzWqOliEgJAL";
constexpr char kApi[] = "http://api.l1d0k2y5x8d0b8h.vip";
constexpr char kPlayApi[] = "https://openim-php-api.xwwx71mv2j2mukypz4kd.cc";
constexpr char kBase[] =
    "cSpSxjfKU4tVAu154bUMhiH5JFAciJ6goWs+T2gzaqtAGhs3MAFTWz74HwYx6Lno"
    "gT9m/ALQThBkfnLsyWTGxi958dHxUJs4aSTAMe13xx5RQQrXGOrPzTangYMvEs94f"
    "xGlwTpt1nKmD/pEvZwk8NeBSd3AZ/MM1cxIMNB/wLxFO0yXGFNT8iTcvqEL0dCELu"
    "86dymfoAtv6V8twMg0iFXTmUI7wNuvbZ58NXNEPUebc/ATGvw4X5f7982oZEKvOcq"
    "46EtvGyct6BjBJLt8vg";

const char b64Table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const std::vector<unsigned char>& data)
{
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    for (size_t i = 0; i < data.size(); i += 3)
    {
        const unsigned a = data[i];
        const unsigned b = i + 1 < data.size() ? data[i + 1] : 0;
        const unsigned c = i + 2 < data.size() ? data[i + 2] : 0;
        const unsigned n = (a << 16) | (b << 8) | c;
        out.push_back(b64Table[(n >> 18) & 63]);
        out.push_back(b64Table[(n >> 12) & 63]);
        out.push_back(i + 1 < data.size() ? b64Table[(n >> 6) & 63] : '=');
        out.push_back(i + 2 < data.size() ? b64Table[n & 63] : '=');
    }
    return out;
}

std::vector<unsigned char> base64Decode(const std::string& text)
{
    std::vector<unsigned char> out;
    int value = 0, bits = -8;
    for (unsigned char c : text)
    {
        if (std::isspace(c)) continue;
        if (c == '=') break;
        const char* found = std::strchr(b64Table, c);
        if (!found) return {};
        value = (value << 6) | static_cast<int>(found - b64Table);
        bits += 6;
        if (bits >= 0)
        {
            out.push_back(static_cast<unsigned char>((value >> bits) & 0xff));
            bits -= 8;
        }
    }
    return out;
}

std::string encrypt(const std::string& plain)
{
    std::vector<unsigned char> padded(plain.begin(), plain.end());
    const unsigned char padding = static_cast<unsigned char>(16 - padded.size() % 16);
    padded.insert(padded.end(), padding, padding);
    std::vector<unsigned char> encrypted(padded.size());
    Aes128CbcContext context{};
    aes128CbcContextCreate(&context, kKey, kKey, true);
    if (aes128CbcEncrypt(&context, encrypted.data(), padded.data(), padded.size()) != padded.size())
        return {};
    return base64Encode(encrypted);
}

std::string decrypt(const std::string& encoded)
{
    auto input = base64Decode(encoded);
    if (input.empty() || input.size() % 16 != 0) return {};
    std::vector<unsigned char> output(input.size());
    Aes128CbcContext context{};
    aes128CbcContextCreate(&context, kKey, kKey, false);
    if (aes128CbcDecrypt(&context, output.data(), input.data(), input.size()) != input.size())
        return {};
    const unsigned char padding = output.back();
    if (padding == 0 || padding > 16 || padding > output.size()) return {};
    for (size_t i = output.size() - padding; i < output.size(); ++i)
        if (output[i] != padding) return {};
    return std::string(reinterpret_cast<char*>(output.data()), output.size() - padding);
}

std::string urlEncode(const std::string& value)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : value)
    {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out.push_back(c);
        else { out.push_back('%'); out.push_back(hex[c >> 4]); out.push_back(hex[c & 15]); }
    }
    return out;
}

std::vector<net::HttpHeader> headers(bool form = false)
{
    std::vector<net::HttpHeader> result = {
        {"User-Agent", "okhttp/4.2.2"},
        {"imei", "ed28e174-0bbe-4aa6-94ef-6ec64df6d1a7"},
        {"device", "1"}, {"platform", "fqzb"}, {"base", kBase}
    };
    if (form) result.push_back({"Content-Type", "application/x-www-form-urlencoded"});
    return result;
}

std::string objectString(json_t* object, const char* name)
{
    json_t* value = json_object_get(object, name);
    if (json_is_string(value)) return json_string_value(value);
    if (json_is_integer(value)) return std::to_string(json_integer_value(value));
    return {};
}

int objectInt(json_t* object, const char* name)
{
    json_t* value = json_object_get(object, name);
    if (json_is_integer(value)) return static_cast<int>(json_integer_value(value));
    if (json_is_string(value)) return std::atoi(json_string_value(value));
    return 0;
}

struct JsonReply { json_t* root = nullptr; std::string error; };

JsonReply request(net::HttpClient& http, const std::string& path,
                  const std::string& payload, bool post = false)
{
    const std::string encrypted = encrypt(payload);
    if (encrypted.empty()) return {nullptr, "赛事请求加密失败"};
    net::HttpRequest request;
    request.url = std::string(post ? kPlayApi : kApi) + path;
    request.headers = headers(post);
    request.connectTimeoutSeconds = 8;
    request.timeoutSeconds = 18;
    request.maxResponseBytes = 1024 * 1024;
    if (post)
    {
        request.method = "POST";
        request.body = "data=" + urlEncode(encrypted);
    }
    else request.url += "?data=" + urlEncode(encrypted);
    auto response = http.get(request);
    if (!response.succeeded) return {nullptr, response.error.empty() ? "赛事网络请求失败" : response.error};
    std::string body = response.body;
    const size_t first = body.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {nullptr, "赛事接口返回空内容"};
    if (body[first] != '{' && body[first] != '[') body = decrypt(body.substr(first));
    if (body.empty()) return {nullptr, "赛事响应解密失败"};
    json_error_t error{};
    json_t* root = json_loadb(body.data(), body.size(), 0, &error);
    if (!root) return {nullptr, "赛事响应格式错误"};
    return {root, {}};
}
}

FootballResult FootballProvider::schedule() const
{
    FootballResult result;
    auto reply = request(httpClient_, "/v220/schedule",
        R"({"sport_id":"1","tab":"0","hot":"0"})");
    if (!reply.root) { result.error = reply.error; return result; }
    json_t* data = json_object_get(reply.root, "data");
    json_t* groups = data ? json_object_get(data, "list") : nullptr;
    if (!json_is_array(groups))
    {
        result.error = "赛事列表内容不完整";
        json_decref(reply.root);
        return result;
    }
    const size_t groupCount = json_array_size(groups);
    for (size_t groupIndex = 0; groupIndex < groupCount; ++groupIndex)
    {
        json_t* group = json_array_get(groups, groupIndex);
        json_t* list = json_object_get(group, "list");
        if (!json_is_array(list)) continue;
        for (size_t index = 0; index < json_array_size(list); ++index)
        {
            json_t* item = json_array_get(list, index);
            FootballMatch match;
            match.id = objectString(item, "match_id");
            match.league = objectString(item, "alias_name");
            match.home = objectString(item, "home_name");
            match.away = objectString(item, "away_name");
            match.date = objectString(item, "match_date");
            match.time = objectString(item, "match_hour");
            match.liveStatus = objectInt(item, "live_status");
            match.matchStatus = objectInt(item, "match_status");
            if (!match.id.empty() && !match.home.empty() && !match.away.empty())
                result.matches.push_back(std::move(match));
        }
    }
    json_decref(reply.root);
    std::stable_sort(result.matches.begin(), result.matches.end(), [](const auto& a, const auto& b) {
        return a.liveStatus != b.liveStatus ? a.liveStatus > b.liveStatus : a.date < b.date;
    });
    if (result.matches.size() > 80) result.matches.resize(80);
    result.succeeded = true;
    return result;
}

cloud::ProviderResult<cloud::PlaybackSource> FootballProvider::playback(const FootballMatch& match) const
{
    cloud::ProviderResult<cloud::PlaybackSource> result;
    auto fail = [&](const std::string& message) {
        result.error = {cloud::ProviderErrorCode::InvalidResponse, message};
        return result;
    };
    const std::string matchId = match.id;
    auto rooms = request(httpClient_, "/v171/matchs_live",
        "{\"sport_id\":\"1\",\"match_id\":\"" + matchId
        + "\",\"page\":\"0\",\"page_size\":\"18\"}");
    if (!rooms.root) return fail(rooms.error);
    json_t* roomList = json_object_get(rooms.root, "data");
    if (!json_is_array(roomList) || json_array_size(roomList) == 0)
    {
        json_decref(rooms.root);
        return fail("这场比赛暂时没有可用直播间");
    }
    json_t* room = json_array_get(roomList, json_array_size(roomList) - 1);
    const std::string roomId = objectString(room, "chatroom_id");
    json_decref(rooms.root);
    if (roomId.empty()) return fail("直播间编号缺失");

    auto detail = request(httpClient_, "/v131/room",
        "{\"room_id\":\"" + roomId + "\",\"sport_id\":\"1\",\"match_id\":\""
        + matchId + "\"}");
    if (!detail.root) return fail(detail.error);
    json_t* detailData = json_object_get(detail.root, "data");
    json_t* flows = detailData ? json_object_get(detailData, "play_flow") : nullptr;
    std::string code;
    if (json_is_array(flows))
    {
        for (size_t i = 0; i < json_array_size(flows); ++i)
        {
            json_t* flow = json_array_get(flows, i);
            const std::string candidate = objectString(flow, "code_id");
            if (code.empty()) code = candidate;
            if (candidate == "gqzm") { code = candidate; break; }
        }
    }
    json_decref(detail.root);
    if (code.empty()) return fail("直播间没有可用清晰度");

    auto play = request(httpClient_, "/v230/play/url",
        "{\"room_id\":\"" + roomId + "\",\"sport_id\":\"1\",\"match_id\":\""
        + matchId + "\",\"code_id\":\"" + code + "\"}", true);
    if (!play.root) return fail(play.error);
    json_t* playData = json_object_get(play.root, "data");
    const std::string url = playData ? objectString(playData, "play_url") : std::string();
    json_decref(play.root);
    if (url.empty()) return fail("直播播放地址为空");
    result.value.url = url;
    result.succeeded = true;
    return result;
}
}
