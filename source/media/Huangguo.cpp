#include "okswitch/media/Huangguo.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <jansson.h>
#include <curl/curl.h>

namespace okswitch::media::huangguo {
namespace {
constexpr const char* site="https://huangguoai.com";
constexpr const char* agent="Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";

bool numeric(const std::string& value){
    return !value.empty()&&value.size()<20&&std::all_of(value.begin(),value.end(),
        [](unsigned char c){return std::isdigit(c)!=0;});
}
std::string attribute(const std::string& tag,const char* key){
    const std::string needle=std::string(key)+"=\"";
    const auto start=tag.find(needle);if(start==std::string::npos)return {};
    const auto value=start+needle.size(),end=tag.find('"',value);
    return end==std::string::npos?std::string():tag.substr(value,end-value);
}
std::string clean(std::string value){
    std::string out;bool tag=false;
    for(char c:value){if(c=='<')tag=true;else if(c=='>')tag=false;else if(!tag)out+=c;}
    for(const auto& pair:{std::pair<const char*,const char*>{"&amp;","&"},
            {"&quot;","\""},{"&#39;","'"},{"&nbsp;"," "}}){
        size_t at=0;while((at=out.find(pair.first,at))!=std::string::npos){
            out.replace(at,std::char_traits<char>::length(pair.first),pair.second);
            at+=std::char_traits<char>::length(pair.second);
        }
    }
    const auto first=out.find_first_not_of(" \t\r\n");if(first==std::string::npos)return {};
    return out.substr(first,out.find_last_not_of(" \t\r\n")-first+1);
}
std::string absolute(const std::string& url){
    if(url.rfind("https://",0)==0||url.rfind("http://",0)==0)return url;
    if(url.rfind("//",0)==0)return "https:"+url;
    if(!url.empty()&&url[0]=='/')return std::string(site)+url;
    return {};
}
std::string encoded(const std::string& value){
    CURL* curl=curl_easy_init();if(!curl)return {};
    char* raw=curl_easy_escape(curl,value.c_str(),(int)value.size());
    std::string result=raw?raw:"";if(raw)curl_free(raw);curl_easy_cleanup(curl);return result;
}
net::HttpResponse page(net::HttpClient& http,const std::string& url,
    const std::atomic<bool>* cancel,bool post=false){
    net::HttpRequest request;request.url=url;request.cancel=cancel;
    request.connectTimeoutSeconds=5;request.timeoutSeconds=15;
    request.maxResponseBytes=2*1024*1024;request.closeConnection=true;
    request.headers={{"User-Agent",agent},{"Referer",std::string(site)+"/"},
        {"Accept",post?"application/json":"text/html,application/xhtml+xml"}};
    if(post){request.method="POST";request.body="{}";
        request.headers.push_back({"Content-Type","application/json"});}
    return http.get(request);
}
std::string digitsAfter(const std::string& text,const std::string& prefix){
    const auto at=text.find(prefix);if(at==std::string::npos)return {};
    const auto start=at+prefix.size();auto end=start;
    while(end<text.size()&&std::isdigit((unsigned char)text[end]))++end;
    return end==start?std::string():text.substr(start,end-start);
}
std::string firstImage(const std::string& html){
    const auto at=html.find("data-src=\"");if(at==std::string::npos)return {};
    const auto end=html.find('>',at);if(end==std::string::npos)return {};
    return absolute(clean(attribute(html.substr(at,end-at+1),"data-src")));
}
std::vector<SearchResult> cards(const std::string& html){
    std::vector<SearchResult> result;size_t pos=0;
    while((pos=html.find("class=\"hg-drama-card\"",pos))!=std::string::npos){
        const auto next=html.find("class=\"hg-drama-card\"",pos+10);
        const auto block=html.substr(pos,next==std::string::npos?std::string::npos:next-pos);
        const std::string id=digitsAfter(block,"/video/");
        if(numeric(id)&&std::none_of(result.begin(),result.end(),[&](const auto& item){
                return item.detailUrl=="okswitch:huangguo:"+id;})){
            const auto titleAt=block.find("hg-drama-card__title");
            const auto anchor=titleAt==std::string::npos?std::string::npos:block.find('>',titleAt);
            const auto end=anchor==std::string::npos?std::string::npos:block.find("</a>",anchor);
            const std::string title=end==std::string::npos?std::string():clean(block.substr(anchor+1,end-anchor-1));
            if(!title.empty()){
                SearchResult item;item.sourceName="黄果短剧";item.title=title;
                item.detailUrl="okswitch:huangguo:"+id;
                item.posterUrl=firstImage(block);
                const auto episodeAt=block.find("hg-drama-card__episode");
                const auto episodeStart=episodeAt==std::string::npos?std::string::npos:block.find('>',episodeAt);
                const auto episodeEnd=episodeStart==std::string::npos?std::string::npos:block.find("</span>",episodeStart);
                if(episodeEnd!=std::string::npos)item.status=clean(block.substr(episodeStart+1,episodeEnd-episodeStart-1));
                result.push_back(std::move(item));
            }
        }
        if(next==std::string::npos||result.size()>=30)break;
        pos=next;
    }
    return result;
}
json_t* initialData(const std::string& html){
    const auto marker=html.find("id=\"videoInitialData\"");
    const auto markerSingle=marker==std::string::npos?html.find("id='videoInitialData'"):marker;
    if(markerSingle==std::string::npos)return nullptr;
    const auto start=html.find('>',markerSingle),end=html.find("</script>",start);
    if(start==std::string::npos||end==std::string::npos)return nullptr;
    json_error_t error{};return json_loadb(html.data()+start+1,end-start-1,0,&error);
}
std::string jsonText(json_t* object,const char* key){
    json_t* value=json_object_get(object,key);
    return json_is_string(value)?json_string_value(value):"";
}
}

SearchResponse catalog(net::HttpClient& http,const std::string& section,int pageNumber,
    const std::atomic<bool>* cancel){
    SearchResponse result;
    const std::string allowed[]={"home","ai-duanju","ai-manju","ai-huanlian","ai-mogai","ranks/hot"};
    if(std::find(std::begin(allowed),std::end(allowed),section)==std::end(allowed)){
        result.error="未知短剧分类";return result;
    }
    const int pageIndex=std::clamp(pageNumber,1,100);
    const std::string url=std::string(site)+(section=="home"?"/":"/"+section+"/")
        +(pageIndex>1?std::to_string(pageIndex)+"/":"");
    const auto response=page(http,url,cancel);
    if(!response.succeeded){result.error=response.error;return result;}
    result.items=cards(response.body);
    result.total=(int)result.items.size();result.succeeded=!result.items.empty();
    if(result.items.empty())result.error="当前分类没有可用短剧";
    return result;
}

SearchResponse search(net::HttpClient& http,const std::string& keyword,
    const std::atomic<bool>* cancel){
    SearchResponse result;if(keyword.empty())return result;
    const auto response=page(http,std::string(site)+"/search/video/"+encoded(keyword)+"/",cancel);
    if(!response.succeeded){result.error=response.error;return result;}
    result.items=cards(response.body);result.total=(int)result.items.size();result.succeeded=true;
    return result;
}

DetailResponse detail(net::HttpClient& http,const std::string& id,
    const std::atomic<bool>* cancel){
    DetailResponse result;if(!numeric(id)){result.error="无效的短剧编号";return result;}
    const auto response=page(http,std::string(site)+"/video/"+id+"/",cancel);
    if(!response.succeeded){result.error=response.error;return result;}
    json_t* data=initialData(response.body);
    const auto h1=response.body.find("<h1");
    const auto start=h1==std::string::npos?std::string::npos:response.body.find('>',h1);
    const auto end=start==std::string::npos?std::string::npos:response.body.find("</h1>",start);
    if(end!=std::string::npos)result.detail.title=clean(response.body.substr(start+1,end-start-1));
    if(result.detail.title.empty())result.detail.title=id;
    json_t* episodes=json_object_get(data,"trackingEpisodes");
    size_t maximum=0;const char* key;json_t* value;
    if(json_is_object(episodes))json_object_foreach(episodes,key,value){
        const std::string number=key;if(numeric(number))maximum=std::max(maximum,(size_t)std::strtoul(key,nullptr,10));
    }
    maximum=std::clamp<size_t>(maximum,1,500);
    for(size_t i=1;i<=maximum;++i)
        result.detail.episodes.push_back({"第"+std::to_string(i)+"集",id+"/"+std::to_string(i)});
    result.detail.shares.push_back({"黄果短剧","okswitch:huangguo"});
    result.detail.status=std::to_string(maximum)+" 集";
    if(data)json_decref(data);
    result.succeeded=true;return result;
}

cloud::ProviderResult<cloud::PlaybackSource> playback(net::HttpClient& http,
    const std::string& opaque,const std::atomic<bool>* cancel){
    cloud::ProviderResult<cloud::PlaybackSource> result;
    const auto slash=opaque.find('/');
    const auto id=opaque.substr(0,slash);
    const auto ep=slash==std::string::npos?"1":opaque.substr(slash+1);
    if(!numeric(id)||!numeric(ep)){result.error={cloud::ProviderErrorCode::InvalidResponse,"无效的剧集编号"};return result;}
    const auto response=page(http,std::string(site)+"/api/videos/"+id+"/episodes/"+ep+"/playback",cancel,true);
    if(!response.succeeded){result.error={cloud::ProviderErrorCode::NetworkError,response.error};return result;}
    json_error_t error{};json_t* root=json_loadb(response.body.data(),response.body.size(),0,&error);
    json_t* data=json_object_get(root,"data");json_t* streams=json_object_get(data,"streams");
    const std::string url=jsonText(streams,"main");
    if(url.rfind("https://",0)!=0&&url.rfind("http://",0)!=0){
        if(root)json_decref(root);
        result.error={cloud::ProviderErrorCode::InvalidResponse,"短剧播放接口没有返回视频地址"};return result;
    }
    result.value.url=url;
    result.value.headers={{"User-Agent",agent},{"Referer",std::string(site)+"/"}};
    json_t* cdn=json_object_get(data,"cdn");
    const std::string authKey=jsonText(cdn,"authKey"),authValue=jsonText(cdn,"authValue"),app=jsonText(cdn,"app");
    if(!authKey.empty()&&!authValue.empty())result.value.headers.push_back({authKey,authValue});
    if(!app.empty())result.value.headers.push_back({"x-cdn-app",app});
    result.value.parallelFetches=2;result.value.initialBufferSegments=2;
    result.succeeded=true;if(root)json_decref(root);return result;
}
}
