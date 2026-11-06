#include "okswitch/media/MediaSource.hpp"
#include "okswitch/media/Guazi.hpp"
#include "okswitch/media/YueYue.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <curl/curl.h>
#include <jansson.h>

namespace okswitch::media
{
namespace
{
std::string attribute(const std::string& tag, const std::string& name)
{
    const std::string key = name + "=";
    size_t pos = tag.find(key);
    if (pos == std::string::npos) return {};
    pos += key.size();
    if (pos >= tag.size() || (tag[pos] != '"' && tag[pos] != '\'')) return {};
    const char quote = tag[pos++];
    const size_t end = tag.find(quote, pos);
    return end == std::string::npos ? std::string() : tag.substr(pos, end - pos);
}

std::string decode(std::string value)
{
    struct Entity { const char* encoded; const char* plain; };
    const Entity entities[] = {
        {"&amp;", "&"}, {"&quot;", "\""}, {"&#39;", "'"},
        {"&lt;", "<"}, {"&gt;", ">"}, {"&nbsp;", " "}
    };
    for (const Entity& entity : entities)
    {
        size_t pos = 0;
        while ((pos = value.find(entity.encoded, pos)) != std::string::npos)
        {
            value.replace(pos, std::char_traits<char>::length(entity.encoded), entity.plain);
            pos += std::char_traits<char>::length(entity.plain);
        }
    }
    return value;
}

std::string stripTags(const std::string& html)
{
    std::string result;
    bool inTag = false;
    for (char c : html)
    {
        if (c == '<') inTag = true;
        else if (c == '>') inTag = false;
        else if (!inTag) result.push_back(c);
    }
    result = decode(result);
    size_t first = result.find_first_not_of(" \r\n\t");
    if (first == std::string::npos) return {};
    size_t last = result.find_last_not_of(" \r\n\t");
    return result.substr(first, last - first + 1);
}

std::string elementText(const std::string& html, const std::string& marker)
{
    const size_t markerPos = html.find(marker);
    if (markerPos == std::string::npos) return {};
    const size_t start = html.find('>', markerPos);
    const size_t end = start == std::string::npos ? std::string::npos : html.find('<', start + 1);
    return end == std::string::npos ? std::string() : stripTags(html.substr(start + 1, end - start - 1));
}

std::string absoluteUrl(const std::string& base, const std::string& path)
{
    if (path.find("http://") == 0 || path.find("https://") == 0) return decode(path);
    const size_t scheme = base.find("://");
    if (scheme == std::string::npos) return path;
    const size_t slash = base.find('/', scheme + 3);
    const std::string origin = slash == std::string::npos ? base : base.substr(0, slash);
    return origin + (path.empty() || path[0] == '/' ? path : "/" + path);
}

std::string urlEncode(const std::string& value)
{
    CURL* curl = curl_easy_init();
    if (curl == nullptr) return value;
    char* encoded = curl_easy_escape(curl, value.c_str(), static_cast<int>(value.size()));
    std::string result = encoded == nullptr ? value : encoded;
    if (encoded != nullptr) curl_free(encoded);
    curl_easy_cleanup(curl);
    return result;
}

std::string fieldValue(const std::string& html, const std::string& label)
{
    const size_t labelPos = html.find(label);
    if (labelPos == std::string::npos) return {};
    const size_t itemPos = html.find("video-info-item", labelPos + label.size());
    const size_t start = itemPos == std::string::npos ? std::string::npos : html.find('>', itemPos);
    const size_t end = start == std::string::npos ? std::string::npos : html.find("</div>", start);
    return end == std::string::npos ? std::string() : stripTags(html.substr(start + 1, end - start - 1));
}

bool blockedTlsHost(const std::string& url)
{
    const size_t scheme=url.find("://");
    if(scheme==std::string::npos)return false;
    const size_t start=scheme+3;
    const size_t end=url.find_first_of("/:?#",start);
    std::string host=url.substr(start,end==std::string::npos?std::string::npos:end-start);
    std::transform(host.begin(),host.end(),host.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    // Ryujinx 1.3.3 terminates the whole guest when this server returns an
    // incomplete TLS chain instead of reporting the handshake error to curl.
    return host=="tv.yydsys.cc";
}

bool isHuaJuanPage(const std::string& url)
{
    const size_t scheme=url.find("://");
    if(scheme==std::string::npos)return false;
    const size_t start=scheme+3,end=url.find_first_of("/:?#",start);
    const std::string host=url.substr(start,end==std::string::npos?std::string::npos:end-start);
    return host=="www.hjzhencai.top"||host=="www.huanjuanwp.shop";
}

net::HttpResponse getPublicPage(net::HttpClient& client,net::HttpRequest& request,bool huaJuan)
{
    auto response=client.get(request);
    // Retry only for public Huajuan pages after certificate verification fails.
    // Cloud and login requests keep normal HTTPS verification.
    if(huaJuan&&!response.succeeded&&isHuaJuanPage(request.url)
       &&request.url.rfind("https://",0)==0
       &&response.error.find("HTTPS 证书校验失败")!=std::string::npos)
    {
        request.url.replace(0,5,"http");
        response=client.get(request);
    }
    return response;
}

std::vector<SearchResult> parseSimpleSearchHtml(
    const std::string& sourceName, const std::string& responseUrl, const std::string& html)
{
    std::vector<SearchResult> results;
    size_t pos=0;
    while((pos=html.find("class=\"main\"",pos))!=std::string::npos)
    {
        const size_t tagStart=html.rfind("<a",pos);
        const size_t tagEnd=html.find('>',pos);
        const size_t close=tagEnd==std::string::npos?std::string::npos:html.find("</a>",tagEnd);
        if(tagStart==std::string::npos||tagEnd==std::string::npos||close==std::string::npos)break;
        const std::string tag=html.substr(tagStart,tagEnd-tagStart+1);
        const std::string href=attribute(tag,"href");
        if(!href.empty())
        {
            SearchResult item;
            item.sourceName=sourceName;
            item.detailUrl=absoluteUrl(responseUrl,href);
            item.title=stripTags(html.substr(tagEnd+1,close-tagEnd-1));
            if(!item.title.empty()&&!std::any_of(results.begin(),results.end(),[&](const auto& old){return old.detailUrl==item.detailUrl;}))results.push_back(std::move(item));
            if(results.size()>=24)break;
        }
        pos=close+4;
    }
    return results;
}

std::vector<SearchResult> parseHuaJuanSearchHtml(
    const std::string& sourceName, const std::string& responseUrl, const std::string& html)
{
    std::vector<SearchResult> results;
    size_t pos=0;
    while((pos=html.find("class=\"module-card-item ",pos))!=std::string::npos)
    {
        const size_t blockStart=html.rfind("<div",pos);
        const size_t next=html.find("class=\"module-card-item ",pos+24);
        const std::string block=html.substr(blockStart==std::string::npos?pos:blockStart,
            (next==std::string::npos?html.size():next)-(blockStart==std::string::npos?pos:blockStart));
        const size_t detailPos=block.find("/index.php/vod/detail/id/");
        const size_t tagStart=detailPos==std::string::npos?std::string::npos:block.rfind("<a",detailPos);
        const size_t tagEnd=tagStart==std::string::npos?std::string::npos:block.find('>',tagStart);
        if(tagEnd!=std::string::npos)
        {
            const std::string tag=block.substr(tagStart,tagEnd-tagStart+1);
            SearchResult item;item.sourceName=sourceName;
            item.detailUrl=absoluteUrl(responseUrl,attribute(tag,"href"));
            const size_t titleMarker=block.find("module-card-item-title");
            const size_t strong=titleMarker==std::string::npos?std::string::npos:block.find("<strong",titleMarker);
            const size_t strongEnd=strong==std::string::npos?std::string::npos:block.find("</strong>",strong);
            item.title=strongEnd==std::string::npos?decode(attribute(tag,"title")):stripTags(block.substr(strong,strongEnd-strong));
            const size_t image=block.find("<img");
            const size_t imageEnd=image==std::string::npos?std::string::npos:block.find('>',image);
            if(imageEnd!=std::string::npos)
            {
                const std::string imageTag=block.substr(image,imageEnd-image+1);
                item.posterUrl=decode(attribute(imageTag,"data-original"));
                if(item.posterUrl.empty()||item.posterUrl=="/")item.posterUrl=decode(attribute(imageTag,"data-src"));
                if(item.posterUrl.empty()||item.posterUrl=="/")item.posterUrl=decode(attribute(imageTag,"src"));
                item.posterUrl=absoluteUrl(responseUrl,item.posterUrl);
            }
            item.status=elementText(block,"module-item-note");
            if(!item.title.empty()&&!item.detailUrl.empty())results.push_back(std::move(item));
            if(results.size()>=24)break;
        }
        pos=next==std::string::npos?html.size():next;
    }
    return results;
}

std::vector<SearchResult> parseWoNiuSearchHtml(
    const std::string& sourceName, const std::string& responseUrl, const std::string& html)
{
    std::vector<SearchResult> results;
    size_t pos=0;
    while((pos=html.find("class=\"video-card\"",pos))!=std::string::npos)
    {
        const size_t tagStart=html.rfind("<a",pos);
        const size_t tagEnd=html.find('>',pos);
        const size_t close=tagEnd==std::string::npos?std::string::npos:html.find("</a>",tagEnd);
        if(tagStart==std::string::npos||tagEnd==std::string::npos||close==std::string::npos)break;
        const std::string tag=html.substr(tagStart,tagEnd-tagStart+1);
        const std::string block=html.substr(tagStart,close-tagStart+4);
        SearchResult item;item.sourceName=sourceName;
        item.detailUrl="okswitch:woniu:"+absoluteUrl(responseUrl,attribute(tag,"href"));
        item.title=elementText(block,"video-title");
        if(item.title.empty())item.title=decode(attribute(tag,"title"));
        item.status=elementText(block,"video-episode");
        const std::string meta=elementText(block,"video-meta");
        if(!meta.empty())item.status+=(item.status.empty()?"":" · ")+meta;
        const size_t image=block.find("<img");
        const size_t imageEnd=image==std::string::npos?std::string::npos:block.find('>',image);
        if(imageEnd!=std::string::npos)item.posterUrl=absoluteUrl(responseUrl,decode(attribute(block.substr(image,imageEnd-image+1),"src")));
        if(!item.title.empty())results.push_back(std::move(item));
        if(results.size()>=24)break;
        pos=close+4;
    }
    return results;
}

std::vector<SearchResult> parsePanMiSearchHtml(
    const std::string& sourceName, const std::string& responseUrl,
    const std::string& keyword, const std::string& html)
{
    std::vector<SearchResult> results;
    size_t pos=0;
    while((pos=html.find("class=\"media thread",pos))!=std::string::npos)
    {
        const size_t blockStart=html.rfind("<li",pos);
        const size_t close=blockStart==std::string::npos?std::string::npos:html.find("</li>",pos);
        if(blockStart==std::string::npos||close==std::string::npos)break;
        const std::string block=html.substr(blockStart,close-blockStart+5);
        const size_t liEnd=block.find('>');
        const std::string liTag=liEnd==std::string::npos?std::string():block.substr(0,liEnd+1);
        const std::string href=attribute(liTag,"data-href");
        const size_t subject=block.find("style3_subject");
        const size_t titleTag=subject==std::string::npos?std::string::npos:block.find("<a",subject);
        const size_t titleStart=titleTag==std::string::npos?std::string::npos:block.find('>',titleTag);
        const size_t titleEnd=titleStart==std::string::npos?std::string::npos:block.find("</a>",titleStart);
        const std::string title=titleEnd==std::string::npos?std::string():stripTags(block.substr(titleStart+1,titleEnd-titleStart-1));
        if(!href.empty()&&!title.empty()&&(keyword.empty()||title.find(keyword)!=std::string::npos))
        {
            SearchResult item;item.sourceName=sourceName;item.title=title;
            item.detailUrl="okswitch:panmi:"+absoluteUrl(responseUrl,href);
            const size_t image=block.find("<img");
            const size_t imageEnd=image==std::string::npos?std::string::npos:block.find('>',image);
            if(imageEnd!=std::string::npos)item.posterUrl=absoluteUrl(responseUrl,decode(attribute(block.substr(image,imageEnd-image+1),"src")));
            results.push_back(std::move(item));
            if(results.size()>=24)break;
        }
        pos=close+5;
    }
    return results;
}
}

MediaSource::MediaSource(net::HttpClient& httpClient) : httpClient_(httpClient) {}

SearchResponse MediaSource::browse(const SourceDefinition& source, const std::string& path) const
{
    SearchResponse result;
    net::HttpRequest request;
    request.url = source.baseUrl + path;
    const net::HttpResponse response = httpClient_.get(request);
    if (!response.succeeded)
    {
        result.error = source.name + ": " + response.error;
        return result;
    }
    result.items = parseListingHtml(source.name,
        response.effectiveUrl.empty() ? request.url : response.effectiveUrl, response.body);
    result.succeeded = true;
    return result;
}

SearchResponse MediaSource::catalog(const std::string& endpoint, const std::atomic<bool>* cancel) const
{
    SearchResponse result;
    net::HttpRequest request;
    const bool hot360 = endpoint == "okswitch:360-hot";
    const std::string suggestPrefix="okswitch:suggest:";
    const bool suggestion=endpoint.rfind(suggestPrefix,0)==0;
    request.url = suggestion ? "https://suggest.video.iqiyi.com/?if=mobile&key="+urlEncode(endpoint.substr(suggestPrefix.size()))
                         : hot360 ? "https://api.web.360kan.com/v1/rank?cat=1"
                         : "https://frodo.douban.com/api/v2/" + endpoint;
    request.cancel = cancel;
    request.closeConnection = true;
    request.headers.push_back({"Referer", suggestion?"https://www.iqiyi.com/":hot360 ? "https://www.360kan.com/rank/general"
        : "https://servicewechat.com/wx2f9b06c1de1ccfca/84/page-frame.html"});
    request.headers.push_back({"User-Agent", (hot360||suggestion) ? "okhttp/5.3.2"
        : "Mozilla/5.0 (Windows NT 6.1; WOW64) AppleWebKit/537.36 (KHTML, like Gecko) "
          "Chrome/53.0.2785.143 Safari/537.36 MicroMessenger/7.0.9.501 NetType/WIFI "
          "MiniProgramEnv/Windows WindowsWechat"});
    const net::HttpResponse response = httpClient_.get(request);
    if (!response.succeeded)
    {
        result.error = response.error;
        return result;
    }
    json_error_t error{};
    json_t* root = json_loadb(response.body.data(), response.body.size(), 0, &error);
    if (root == nullptr)
    {
        result.error = "Invalid catalog JSON";
        return result;
    }
    if(suggestion){
        json_t* data=json_object_get(root,"data");
        if(json_is_array(data))for(size_t i=0;i<json_array_size(data)&&result.items.size()<10;++i){json_t* value=json_array_get(data,i);json_t* name=json_object_get(value,"name");if(!json_is_string(name)||json_string_length(name)==0)continue;SearchResult item;item.sourceName="联想";item.title=json_string_value(name);if(std::none_of(result.items.begin(),result.items.end(),[&](const auto& old){return old.title==item.title;}))result.items.push_back(std::move(item));}
        json_decref(root);result.total=(int)result.items.size();result.succeeded=!result.items.empty();if(!result.succeeded)result.error="暂无联想";return result;
    }
    if (hot360)
    {
        json_t* data = json_object_get(root, "data");
        if (json_is_array(data))
        {
            const size_t count = json_array_size(data);
            for (size_t i = 0; i < count; ++i)
            {
                json_t* value = json_array_get(data, i);
                json_t* title = json_object_get(value, "title");
                if (!json_is_string(title) || json_string_length(title) == 0) continue;
                SearchResult item;
                item.sourceName = "360kan";
                item.title = json_string_value(title);
                result.items.push_back(std::move(item));
            }
        }
        json_decref(root);
        result.total = static_cast<int>(result.items.size());
        result.succeeded = !result.items.empty();
        if (!result.succeeded) result.error = "360kan 暂无热搜";
        return result;
    }
    json_t* total = json_object_get(root, "total");
    if (json_is_integer(total)) result.total = static_cast<int>(json_integer_value(total));
    json_t* items = json_object_get(root, "subject_collection_items");
    if (!json_is_array(items)) items = json_object_get(root, "items");
    if (json_is_array(items))
    {
        const size_t count = json_array_size(items);
        for (size_t i = 0; i < count; ++i)
        {
            json_t* value = json_array_get(items, i);
            json_t* title = json_object_get(value, "title");
            json_t* type = json_object_get(value, "type");
            if (!json_is_string(title)) continue;
            const char* typeValue = json_is_string(type) ? json_string_value(type) : "";
            if (std::string(typeValue) == "ad" || std::string(typeValue) == "playlist") continue;
            SearchResult item;
            item.sourceName = "Douban";
            item.title = json_string_value(title);
            json_t* subtitle = json_object_get(value, "card_subtitle");
            if (json_is_string(subtitle)) item.status = json_string_value(subtitle);
            json_t* cover = json_object_get(value, "cover_url");
            if (json_is_string(cover)) item.posterUrl = json_string_value(cover);
            {
                json_t* pic = json_object_get(value, "pic");
                json_t* normal = json_is_object(pic) ? json_object_get(pic, "normal") : nullptr;
                if (json_is_string(normal) && json_string_length(normal)>0) item.posterUrl = json_string_value(normal);
            }
            json_t* rating = json_object_get(value, "rating");
            json_t* ratingValue = json_is_object(rating) ? json_object_get(rating, "value") : nullptr;
            if (json_is_number(ratingValue)) item.rating = json_number_value(ratingValue);
            json_t* rank = json_object_get(value, "rank");
            if (json_is_integer(rank)) item.rank = static_cast<int>(json_integer_value(rank));
            result.items.push_back(item);
        }
    }
    json_decref(root);
    result.succeeded = true;
    return result;
}

SearchResponse MediaSource::search(const SourceDefinition& source, const std::string& keyword,
                                   const std::atomic<bool>* cancel) const
{
    if(source.api.find("YueYue")!=std::string::npos){
        auto response=yueyue::search(httpClient_,keyword,cancel);
        for(auto& item:response.items)item.sourceName=source.name;
        return response;
    }
    if (source.api.find("GuaZi") != std::string::npos)
    {
        auto response = guazi::search(httpClient_, keyword, cancel);
        for (auto& item : response.items) item.sourceName = source.name;
        return response;
    }
    if(source.api.find("GuanYing")!=std::string::npos)
    {
        SearchResponse response;
        response.error="观影需要站点登录令牌，暂不能安全地在原生版复用";
        return response;
    }
    SearchResponse result;
    std::vector<std::string> bases=source.baseUrls;
    if(bases.empty())bases.push_back(source.baseUrl);
    bool receivedResponse=false;
    std::string lastError;
    for(const auto& base:bases)
    {
        if(cancel&&cancel->load())break;
        if(blockedTlsHost(base)){
            lastError="已跳过证书链异常地址 "+base;
            continue;
        }
        net::HttpRequest request;
        const bool simpleJuTou=source.api.find("JuTou")!=std::string::npos;
        const bool huaJuan=source.api.find("HuaJuan")!=std::string::npos;
        const bool wogg=source.api.find("Wogg")!=std::string::npos;
        const bool woNiu=source.api.find("WoNiu")!=std::string::npos;
        const bool panMi=source.api.find("Pan1Me")!=std::string::npos;
        std::string requestBase=base;
        while(!requestBase.empty()&&requestBase.back()=='/')requestBase.pop_back();
        request.url = simpleJuTou ? base+"/search/?keyword="+urlEncode(keyword)
                    : wogg ? requestBase+"/index.php/vodsearch/"+urlEncode(keyword)+"----------1---.html"
                    : woNiu ? base+"/vodsearch/"+urlEncode(keyword)+"----------1---.html"
                    : panMi ? base+"/?search-"+urlEncode(keyword)+".htm"
                    : base+"/index.php/vod/search/page/1/wd/"+urlEncode(keyword)+".html";
        request.cancel = cancel;
        request.connectTimeoutSeconds = huaJuan ? 8 : 4;
        request.timeoutSeconds = huaJuan ? 18 : 8;
        request.maxResponseBytes = 3 * 1024 * 1024;
        request.closeConnection = true;
        // Any Wogg mirror can answer 301 and then stall at its destination.
        // Inspect the response and move to another mirror instead of waiting
        // inside curl's automatic redirect chain.
        if(wogg)request.followRedirects=false;
        request.headers.push_back({"Accept-Encoding", "gzip"});
        request.headers.push_back({"User-Agent", "Mozilla/5.0 (Linux; Android 14) AppleWebKit/537.36 Chrome/140.0 Mobile Safari/537.36"});
        const net::HttpResponse response = getPublicPage(httpClient_,request,huaJuan);
        if(!response.succeeded){
            lastError=wogg&&response.statusCode>=300&&response.statusCode<400
                ? "玩偶镜像返回跳转，已尝试其他地址"
                :wogg&&response.statusCode==403&&response.body.find("Just a moment")!=std::string::npos
                ? "玩偶站点要求浏览器验证（403）":response.error;
            continue;
        }
        receivedResponse=true;
        const std::string responseUrl=response.effectiveUrl.empty()?request.url:response.effectiveUrl;
        auto items=simpleJuTou?parseSimpleSearchHtml(source.name,responseUrl,response.body)
                  :huaJuan?parseHuaJuanSearchHtml(source.name,responseUrl,response.body)
                  :woNiu?parseWoNiuSearchHtml(source.name,responseUrl,response.body)
                  :panMi?parsePanMiSearchHtml(source.name,responseUrl,keyword,response.body)
                  :parseSearchHtml(source.name,responseUrl,response.body);
        if(!items.empty()){result.items=std::move(items);result.succeeded=true;return result;}
    }
    result.succeeded=receivedResponse;
    if(!result.succeeded)result.error=source.name+": "+(lastError.empty()?"所有备用地址均不可用":lastError);
    return result;
}

DetailResponse MediaSource::detail(const std::string& url,
                                   const std::atomic<bool>* cancel) const
{
    constexpr const char* yueyuePrefix="okswitch:yueyue:";
    if(url.rfind(yueyuePrefix,0)==0)
        return yueyue::detail(httpClient_,url.substr(std::strlen(yueyuePrefix)),cancel);
    constexpr const char* guaziPrefix = "okswitch:guazi:";
    if (url.rfind(guaziPrefix, 0) == 0)
        return guazi::detail(httpClient_, url.substr(std::strlen(guaziPrefix)), cancel);
    constexpr const char* woNiuPrefix="okswitch:woniu:";
    constexpr const char* panMiPrefix="okswitch:panmi:";
    const bool woNiu=url.rfind(woNiuPrefix,0)==0;
    const bool panMi=url.rfind(panMiPrefix,0)==0;
    DetailResponse result;
    net::HttpRequest request;
    request.url = woNiu?url.substr(std::strlen(woNiuPrefix))
                :panMi?url.substr(std::strlen(panMiPrefix)):url;
    request.cancel = cancel;
    request.maxResponseBytes = 4 * 1024 * 1024;
    request.closeConnection = true;
    request.headers.push_back({"Accept-Encoding", "gzip"});
    request.headers.push_back({"User-Agent", "Mozilla/5.0 (Linux; Android 14) AppleWebKit/537.36 Chrome/140.0 Mobile Safari/537.36"});
    const net::HttpResponse response = getPublicPage(httpClient_,request,isHuaJuanPage(request.url));
    if (!response.succeeded)
    {
        result.error = response.error;
        return result;
    }
    result.detail = parseDetailHtml(response.body);
    result.succeeded = !result.detail.title.empty()&&!result.detail.shares.empty();
    if (!result.succeeded)
    {
        if(woNiu&&(response.body.find("登录后可查看")!=std::string::npos||response.body.find("网盘资源已锁定")!=std::string::npos))
            result.error="蜗牛详情需要登录 Cookie";
        else if(panMi&&(response.body.find("123pan")!=std::string::npos||response.body.find("123云盘")!=std::string::npos))
            result.error="该盘迷帖子只有 123 网盘，当前播放器暂不支持";
        else result.error="详情页没有可用的夸克或百度链接";
    }
    return result;
}

std::vector<SearchResult> MediaSource::parseSearchHtml(
    const std::string& sourceName, const std::string& responseUrl, const std::string& html)
{
    std::vector<SearchResult> results;
    size_t pos = 0;
    while (true)
    {
        const size_t classicPos=html.find("/index.php/vod/detail/id/",pos);
        const size_t woggPos=html.find("/voddetail/",pos);
        pos=classicPos==std::string::npos?woggPos
            :woggPos==std::string::npos?classicPos:std::min(classicPos,woggPos);
        if(pos==std::string::npos)break;
        const size_t tagStart = html.rfind("<a", pos);
        const size_t tagEnd = html.find('>', pos);
        if (tagStart == std::string::npos || tagEnd == std::string::npos) break;
        const std::string tag = html.substr(tagStart, tagEnd - tagStart + 1);
        const std::string href = attribute(tag, "href");
        if(href.empty()){pos=tagEnd+1;continue;}
        const std::string detailUrl = absoluteUrl(responseUrl, href);
        const bool duplicate = std::any_of(results.begin(), results.end(), [&](const SearchResult& item) {
            return item.detailUrl == detailUrl;
        });
        if (!duplicate)
        {
            const size_t blockStart = html.rfind("<div class=\"module-search-item\"", tagStart);
            const size_t nextBlock = html.find("<div class=\"module-search-item\"", tagEnd + 1);
            const size_t anchorEnd=html.find("</a>",tagEnd+1);
            const size_t start=blockStart==std::string::npos?tagStart:blockStart;
            const size_t end=blockStart==std::string::npos
                ?(anchorEnd==std::string::npos?tagEnd+1:anchorEnd+4)
                :(nextBlock==std::string::npos?html.size():nextBlock);
            const std::string block=html.substr(start,end-start);
            SearchResult item;
            item.sourceName = sourceName;
            item.detailUrl = detailUrl;
            const size_t h3 = block.find("<h3");
            const size_t h3End = h3 == std::string::npos ? std::string::npos : block.find("</h3>", h3);
            item.title = h3End == std::string::npos ? decode(attribute(tag, "title")) : stripTags(block.substr(h3, h3End - h3));
            if(item.title.empty()){
                const size_t imgAlt=block.find("<img");
                const size_t imgAltEnd=imgAlt==std::string::npos?std::string::npos:block.find('>',imgAlt);
                if(imgAltEnd!=std::string::npos)item.title=decode(attribute(block.substr(imgAlt,imgAltEnd-imgAlt+1),"alt"));
            }
            if(item.title.empty()&&anchorEnd!=std::string::npos)item.title=stripTags(html.substr(tagEnd+1,anchorEnd-tagEnd-1));
            const size_t img = block.find("<img");
            const size_t imgEnd = img == std::string::npos ? std::string::npos : block.find('>', img);
            if (imgEnd != std::string::npos)
            {
                const std::string imageTag = block.substr(img, imgEnd - img + 1);
                item.posterUrl = decode(attribute(imageTag, "data-src"));
            }
            item.status = elementText(block, "video-serial");
            if(!item.title.empty())results.push_back(item);
            if(results.size()>=24)break;
        }
        pos = tagEnd + 1;
    }
    return results;
}

std::vector<SearchResult> MediaSource::parseListingHtml(
    const std::string& sourceName, const std::string& responseUrl, const std::string& html)
{
    std::vector<SearchResult> results;
    size_t pos = 0;
    while ((pos = html.find("<div class=\"module-item\"", pos)) != std::string::npos)
    {
        const size_t blockEnd = html.find("<div class=\"module-item\"", pos + 24);
        const std::string block = html.substr(pos,
            (blockEnd == std::string::npos ? html.size() : blockEnd) - pos);
        const size_t detailPos = block.find("/index.php/vod/detail/id/");
        if (detailPos != std::string::npos)
        {
            const size_t tagStart = block.rfind("<a", detailPos);
            const size_t tagEnd = block.find('>', detailPos);
            if (tagStart != std::string::npos && tagEnd != std::string::npos)
            {
                const std::string tag = block.substr(tagStart, tagEnd - tagStart + 1);
                SearchResult item;
                item.sourceName = sourceName;
                item.detailUrl = absoluteUrl(responseUrl, attribute(tag, "href"));
                const size_t namePos = block.find("video-name");
                const size_t nameStart = namePos == std::string::npos ? std::string::npos : block.find('>', namePos);
                const size_t nameEnd = nameStart == std::string::npos ? std::string::npos : block.find("</div>", nameStart);
                item.title = nameEnd == std::string::npos ? decode(attribute(tag, "title"))
                    : stripTags(block.substr(nameStart + 1, nameEnd - nameStart - 1));
                if (item.title.size() > 6 && item.title.substr(item.title.size() - 6) == "下载")
                    item.title.resize(item.title.size() - 6);
                const size_t img = block.find("<img");
                const size_t imgEnd = img == std::string::npos ? std::string::npos : block.find('>', img);
                if (imgEnd != std::string::npos)
                    item.posterUrl = decode(attribute(block.substr(img, imgEnd - img + 1), "data-src"));
                item.status = elementText(block, "module-item-note");
                if (!item.title.empty()) results.push_back(item);
                if(results.size()>=24)break;
            }
        }
        pos = blockEnd == std::string::npos ? html.size() : blockEnd;
    }
    return results;
}

MediaDetail MediaSource::parseDetailHtml(const std::string& html)
{
    MediaDetail detail;
    detail.title = elementText(html, "page-title");
    // Huajuan uses a plain h1; its document title includes site/SEO text.
    if(detail.title.empty() && html.find("down-card-urlbox")!=std::string::npos)
    {
        const size_t heading=html.find("<h1>");
        const size_t end=heading==std::string::npos?std::string::npos:html.find("</h1>",heading);
        if(end!=std::string::npos)detail.title=stripTags(html.substr(heading+4,end-heading-4));
    }
    if(detail.title.empty()){
        const size_t heading=html.find("<h1");
        const size_t start=heading==std::string::npos?std::string::npos:html.find('>',heading);
        const size_t end=start==std::string::npos?std::string::npos:html.find("</h1>",start);
        if(end!=std::string::npos)detail.title=stripTags(html.substr(start+1,end-start-1));
    }
    if(detail.title.empty()){
        const size_t titleStart=html.find("<title>");
        const size_t titleEnd=titleStart==std::string::npos?std::string::npos:html.find("</title>",titleStart+7);
        if(titleEnd!=std::string::npos){detail.title=stripTags(html.substr(titleStart+7,titleEnd-titleStart-7));const size_t suffix=detail.title.rfind("-剧透社");if(suffix!=std::string::npos)detail.title.resize(suffix);}
    }
    detail.year = fieldValue(html, "年代：");
    detail.director = fieldValue(html, "导演：");
    detail.actors = fieldValue(html, "主演：");
    detail.status = fieldValue(html, "集数：");
    const size_t description = html.find("sqjj_a");
    if (description != std::string::npos)
    {
        const size_t start = html.find('>', description);
        const size_t end = start == std::string::npos ? std::string::npos : html.find("</p>", start);
        if (end != std::string::npos) detail.description = stripTags(html.substr(start + 1, end - start - 1));
    }
    const size_t cover = html.find("video-cover");
    const size_t image = cover == std::string::npos ? std::string::npos : html.find("<img", cover);
    const size_t imageEnd = image == std::string::npos ? std::string::npos : html.find('>', image);
    if (imageEnd != std::string::npos)
    {
        detail.posterUrl = decode(attribute(html.substr(image, imageEnd - image + 1), "data-src"));
    }
    size_t pos = 0;
    while ((pos = html.find("https://pan.quark.cn/s/", pos)) != std::string::npos)
    {
        const size_t end = html.find_first_of("\"'<> \r\n\t", pos);
        const std::string url = decode(html.substr(pos, end == std::string::npos ? html.size() - pos : end - pos));
        const bool duplicate = std::any_of(detail.shares.begin(), detail.shares.end(), [&](const ShareLink& link) {
            return link.url == url;
        });
        if (!duplicate) detail.shares.push_back({"Quark " + std::to_string(detail.shares.size() + 1), url});
        pos += url.size();
    }
    pos = 0;
    while ((pos = html.find("https://pan.baidu.com/", pos)) != std::string::npos)
    {
        const size_t end = html.find_first_of("\"'<> \r\n\t", pos);
        const std::string url = decode(html.substr(pos,
            end == std::string::npos ? html.size() - pos : end - pos));
        const bool sharePath = url.find("/s/") != std::string::npos
            || url.find("/share/init?") != std::string::npos;
        const bool duplicate = std::any_of(detail.shares.begin(), detail.shares.end(),
            [&](const ShareLink& link) { return link.url == url; });
        if (sharePath && !duplicate)
            detail.shares.push_back({"Baidu " + std::to_string(detail.shares.size() + 1), url});
        pos += std::max<size_t>(1, url.size());
    }
    return detail;
}

cloud::ProviderResult<cloud::PlaybackSource> MediaSource::guaziPlayback(
    const std::string& opaque, const std::atomic<bool>* cancel) const
{
    constexpr const char* yueyuePrefix="yueyue:";
    if(opaque.rfind(yueyuePrefix,0)==0)
        return yueyue::playback(httpClient_,opaque.substr(std::strlen(yueyuePrefix)),cancel);
    return guazi::playback(httpClient_, opaque, cancel);
}
}
