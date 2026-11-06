#include "okswitch/media/SourceConfigLoader.hpp"

#include <algorithm>
#include <jansson.h>

namespace okswitch::media {
namespace {
struct NativeSource {
    const char* api;
    const char* baseUrl;
    const char* endpointKey;
};

// Adding another native provider only requires registering its csp name here
// after its protocol has been implemented by MediaSource.
constexpr NativeSource nativeSources[]={
    {"csp_WexGuaZiGuard","https://sdapi.e2wu4ht.com","guazi"},
    {"csp_WexAiGuaZiGuard","https://sdapi.e2wu4ht.com","guazi"},
    {"csp_WexAiYueYueGuard","https://u.yyxdmn.com","yueyue"},
    {"csp_WexYueYueGuard","https://u.yyxdmn.com","yueyue"},
    {"csp_NewErXiaoGuard","https://www.2xiaopan.top","erxiao"},
    {"csp_NewWoggGuard","https://www.wogg.live","wogg"},
    {"csp_AiNewWoggGuard","https://www.wogg.live","wogg"},
    {"csp_NewZhiZhenGuard","https://www.miqk.cc","zhizhen"},
    {"csp_NewGuanYingGuard","https://www.hgeme.com","guanying"},
    {"csp_AiNewGuanYingGuard","https://www.hgeme.com","guanying"},
    {"csp_AiNewHuaJuanGuard","https://www.hjzhencai.top","huajuan"},
    // Disabled until the site's login Cookie flow is available natively.
    // {"csp_AiNewWoNiu4KGuard","https://wn4k.com","woniu"},
    // Disabled until 123 cloud playback is supported.
    // {"csp_AiPan1MeGuard","https://pan1.me","123pan"},
    {"csp_NewJuTouGuard","https://1.star2.cn","jutou"},
    {"csp_AiNewJuTouGuard","https://1.star2.cn","jutou"},
    {"csp_AiNewHuBanGuard","http://43.248.128.118:16969","huban"},
    {"csp_NewHuBanGuard","http://43.248.128.118:16969","huban"},
    {"csp_NewMuOuGuard","https://www.muoua.top","muou"},
    {"csp_AiNewMuOuGuard","https://www.muoua.top","muou"},
    {"csp_NewDuoDuoGuard","https://tv.yydsys.top","duoduo"},
    {"csp_AiNewDuoDuoGuard","https://tv.yydsys.top","duoduo"},
    {"csp_AiNewZhiNan4KGuard","https://4k.pianku.online","panku"},
};

const char* jsonString(json_t* object,const char* key){
    json_t* value=json_object_get(object,key);
    return json_is_string(value)?json_string_value(value):"";
}
}

SourceConfigLoader::SourceConfigLoader(net::HttpClient& httpClient):httpClient_(httpClient){
    mutex_=SDL_CreateMutex();condition_=SDL_CreateCond();
    if(mutex_&&condition_)thread_=SDL_CreateThread(entry,"source-config",this);
}
SourceConfigLoader::~SourceConfigLoader(){shutdown();}
void SourceConfigLoader::shutdown(){
    if(mutex_){
        SDL_LockMutex(mutex_);stopping_=true;cancel_.store(true);
        if(condition_)SDL_CondSignal(condition_);
        SDL_UnlockMutex(mutex_);
    }
    if(thread_){SDL_WaitThread(thread_,nullptr);thread_=nullptr;}
    if(condition_){SDL_DestroyCond(condition_);condition_=nullptr;}
    if(mutex_){SDL_DestroyMutex(mutex_);mutex_=nullptr;}
}
void SourceConfigLoader::request(const std::string& url){
    if(!mutex_)return;
    SDL_LockMutex(mutex_);url_=url;++requestId_;cancel_.store(true);ready_=false;pending_=true;
    if(condition_)SDL_CondSignal(condition_);
    SDL_UnlockMutex(mutex_);
}
bool SourceConfigLoader::poll(SourceConfigResult& result){
    if(!mutex_)return false;
    SDL_LockMutex(mutex_);const bool ready=ready_;
    if(ready){result=std::move(result_);ready_=false;}
    SDL_UnlockMutex(mutex_);return ready;
}
int SourceConfigLoader::entry(void* self){return static_cast<SourceConfigLoader*>(self)->run();}
int SourceConfigLoader::run(){
    for(;;){
        SDL_LockMutex(mutex_);
        while(!pending_&&!stopping_)SDL_CondWait(condition_,mutex_);
        if(stopping_){SDL_UnlockMutex(mutex_);return 0;}
        const std::string url=url_;const size_t requestId=requestId_;pending_=false;cancel_.store(false);SDL_UnlockMutex(mutex_);
        auto loaded=load(url);
        SDL_LockMutex(mutex_);
        if(!stopping_&&requestId==requestId_){result_=std::move(loaded);ready_=true;}
        SDL_UnlockMutex(mutex_);
    }
}
SourceConfigResult SourceConfigLoader::load(const std::string& url){
    SourceConfigResult result;
    net::HttpRequest request;request.url=url;request.cancel=&cancel_;
    request.connectTimeoutSeconds=8;request.timeoutSeconds=15;
    const auto response=httpClient_.get(request);
    if(!response.succeeded){result.error=response.error;return result;}

    json_error_t error{};json_t* root=json_loadb(response.body.data(),response.body.size(),0,&error);
    if(!root){result.error="在线源配置格式错误";return result;}
    json_t* sites=json_object_get(root,"sites");
    if(!json_is_array(sites)){json_decref(root);result.error="在线配置缺少 sites";return result;}
    result.totalSites=json_array_size(sites);

    // The guarded plugin downloads an item document which points at a small
    // JSON endpoint map. Following this public chain lets native providers
    // track host/IP changes without executing the Android spider bundle.
    json_t* endpointMap=nullptr;
    const char* itemMirrors[]={
        "http://ossduo.baicanuc.cn/ossfiles/1768320816/item.txt",
        "http://ossone.baicanuc.cn/ossfiles/1768320816/item.txt",
        "http://upload.baicanuc.cn/ossfiles/1768320816/item.txt",
    };
    std::string endpointMapUrl;
    for(const char* mirror:itemMirrors){
        net::HttpRequest itemRequest;itemRequest.url=mirror;itemRequest.cancel=&cancel_;
        itemRequest.connectTimeoutSeconds=4;itemRequest.timeoutSeconds=6;
        const auto itemResponse=httpClient_.get(itemRequest);if(!itemResponse.succeeded)continue;
        json_error_t itemError{};json_t* item=json_loadb(itemResponse.body.data(),itemResponse.body.size(),0,&itemError);
        if(item){endpointMapUrl=jsonString(item,"config_site");json_decref(item);}
        if(!endpointMapUrl.empty())break;
    }
    if(!endpointMapUrl.empty()){
        net::HttpRequest mapRequest;mapRequest.url=endpointMapUrl;mapRequest.cancel=&cancel_;
        mapRequest.connectTimeoutSeconds=4;mapRequest.timeoutSeconds=6;
        const auto mapResponse=httpClient_.get(mapRequest);
        if(mapResponse.succeeded){
            json_error_t mapError{};endpointMap=json_loadb(mapResponse.body.data(),mapResponse.body.size(),0,&mapError);
        }
    }
    for(size_t i=0;i<result.totalSites;++i){
        json_t* site=json_array_get(sites,i);if(!json_is_object(site))continue;
        const std::string api=jsonString(site,"api");
        // Only FeiQiu has a native protocol. Hide the other sports entries
        // until they can actually open a match.
        if(api=="csp_SportFeiQiuGuard"){
            std::string name=jsonString(site,"name");if(name.empty())name=jsonString(site,"key");
            if(!name.empty())result.footballSources.push_back(std::move(name));
        }
        const auto native=std::find_if(std::begin(nativeSources),std::end(nativeSources),[&](const auto& item){return api==item.api;});
        if(native==std::end(nativeSources))continue;
        // Old and new configurations can expose aliases for the same native
        // provider. Keep the first configured entry and avoid duplicate tabs.
        if(std::any_of(result.sources.begin(),result.sources.end(),[&](const auto& item){return item.api==api;}))continue;
        SourceDefinition source;
        source.name=jsonString(site,"name");if(source.name.empty())source.name=jsonString(site,"key");
        source.key=jsonString(site,"key");source.api=api;
        // A newer public Wogg mirror can work when the older xxooo.cf hosts
        // redirect to an unresponsive destination.
        if(std::string(native->endpointKey)=="wogg")source.baseUrls.push_back("https://woggpan.888484.xyz");
        json_t* hosts=endpointMap?json_object_get(endpointMap,native->endpointKey):nullptr;
        if(json_is_array(hosts))for(size_t hostIndex=0;hostIndex<json_array_size(hosts);++hostIndex){
            if(source.baseUrls.size()>=3)break;
            json_t* host=json_array_get(hosts,hostIndex);if(!json_is_string(host))continue;
            std::string candidate=json_string_value(host);
            if(candidate.rfind("http://",0)!=0&&candidate.rfind("https://",0)!=0)continue;
            while(candidate.size()>8&&candidate.back()=='/')candidate.pop_back();
            if(std::find(source.baseUrls.begin(),source.baseUrls.end(),candidate)==source.baseUrls.end())source.baseUrls.push_back(std::move(candidate));
        }
        std::string fallback=native->baseUrl;
        while(fallback.size()>8&&fallback.back()=='/')fallback.pop_back();
        if(std::find(source.baseUrls.begin(),source.baseUrls.end(),fallback)==source.baseUrls.end())source.baseUrls.push_back(fallback);
        source.baseUrl=source.baseUrls.front();
        result.sources.push_back(std::move(source));
    }
    // ZhiZhen and ErXiao are useful native providers even when a particular
    // online TVBox list temporarily omits their guarded plugin entries. Pin
    // them into the merged list and still consume the live mirror map.
    auto addPinned=[&](const char* name,const char* api,const char* family,const char* endpointKey,const char* fallbackUrl){
        if(std::any_of(result.sources.begin(),result.sources.end(),[&](const auto& item){return item.api.find(family)!=std::string::npos;}))return;
        SourceDefinition source;source.name=name;source.key=api;source.api=api;
        json_t* hosts=endpointMap?json_object_get(endpointMap,endpointKey):nullptr;
        if(json_is_array(hosts))for(size_t hostIndex=0;hostIndex<json_array_size(hosts)&&source.baseUrls.size()<2;++hostIndex){
            json_t* host=json_array_get(hosts,hostIndex);if(!json_is_string(host))continue;
            std::string candidate=json_string_value(host);if(candidate.rfind("http://",0)!=0&&candidate.rfind("https://",0)!=0)continue;
            while(candidate.size()>8&&candidate.back()=='/')candidate.pop_back();
            if(std::find(source.baseUrls.begin(),source.baseUrls.end(),candidate)==source.baseUrls.end())source.baseUrls.push_back(std::move(candidate));
        }
        std::string fallback=fallbackUrl;while(fallback.size()>8&&fallback.back()=='/')fallback.pop_back();
        if(std::find(source.baseUrls.begin(),source.baseUrls.end(),fallback)==source.baseUrls.end())source.baseUrls.push_back(fallback);
        source.baseUrl=source.baseUrls.front();result.sources.push_back(std::move(source));
    };
    addPinned("至臻 | 4K","csp_NewZhiZhenGuard","ZhiZhen","zhizhen","https://www.miqk.cc");
    addPinned("二小 | 4K","csp_NewErXiaoGuard","ErXiao","erxiao","https://www.2xiaopan.top");
    addPinned("秒播 | 1080P","csp_WexGuaZiGuard","WexGuaZi","guazi","https://sdapi.e2wu4ht.com");
    addPinned("▶ 闪电┃秒播","csp_WexAiYueYueGuard","YueYue","yueyue","https://u.yyxdmn.com");

    // Keep the two newly integrated providers directly behind JuTou so their
    // results arrive early during the stable sequential online search.
    std::stable_sort(result.sources.begin(),result.sources.end(),[](const auto& left,const auto& right){
        auto rank=[](const auto& source){
            if(source.api.find("JuTou")!=std::string::npos)return 0;
            if(source.api.find("ZhiZhen")!=std::string::npos)return 1;
            if(source.api.find("ErXiao")!=std::string::npos)return 2;
            if(source.api.find("HuBan")!=std::string::npos)return 3;
            if(source.api.find("GuaZi")!=std::string::npos)return 4;
            if(source.api.find("YueYue")!=std::string::npos)return 5;
            return 3;
        };
        return rank(left)<rank(right);
    });
    if(endpointMap)json_decref(endpointMap);
    json_decref(root);
    result.succeeded=!result.sources.empty();
    if(!result.succeeded)result.error="在线配置没有已支持的原生来源";
    return result;
}
}
