#include "okswitch/media/YueYue.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <sstream>
#include <switch.h>
#include <jansson.h>
extern "C" {
#include <libavutil/aes.h>
#include <libavutil/base64.h>
#include <libavutil/md5.h>
#include <libavutil/mem.h>
}

namespace okswitch::media::yueyue {
namespace {
constexpr const char* base="https://u.yyxdmn.com/api";
constexpr const char* key="aZ9$kU5%qI7=yC2=zH2#gM0@pX7^wF3a";
constexpr const char* iv="hY2&tN3]kF7,dL7=";
constexpr const char* requestSecret="zD9[bM4~sF4~uY2)";
constexpr const char* mediaSecret="IysJHCZ2Z7HBEoPPKC8nXIibjGHkKNrc";
std::mutex sessionMutex;
struct Session {std::string device,token,app="tianjishipin",channel="tjsp_sp01";};
Session session;

std::string value(json_t* root,const char* field){
    json_t* item=json_is_object(root)?json_object_get(root,field):nullptr;
    if(json_is_string(item))return json_string_value(item);
    if(json_is_integer(item))return std::to_string(json_integer_value(item));
    return {};
}
std::string md5(const std::string& input,bool upper=false){
    uint8_t digest[16];av_md5_sum(digest,reinterpret_cast<const uint8_t*>(input.data()),input.size());
    const char* alphabet=upper?"0123456789ABCDEF":"0123456789abcdef";
    std::string out;out.reserve(32);
    for(uint8_t b:digest){out.push_back(alphabet[b>>4]);out.push_back(alphabet[b&15]);}
    return out;
}
std::string encode(const std::string& input){
    CURL* curl=curl_easy_init();if(!curl)return {};
    char* escaped=curl_easy_escape(curl,input.c_str(),static_cast<int>(input.size()));
    std::string out=escaped?escaped:"";if(escaped)curl_free(escaped);curl_easy_cleanup(curl);return out;
}
std::string form(const std::vector<std::pair<std::string,std::string>>& fields){
    std::string out;
    for(const auto& field:fields){if(!out.empty())out+='&';out+=encode(field.first)+'='+encode(field.second);}
    return out;
}
std::string milliseconds(){
    const auto now=std::chrono::system_clock::now().time_since_epoch();
    return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}
std::string badci(){
    const long long now=std::stoll(milliseconds());
    const long long micro=(now%1000)*1000;
    const long long delta=7000+std::rand()%8000;
    const long long random=100000000LL+std::rand()%2047483647;
    const long long seconds=now/1000;
    return md5("10-"+std::to_string(random)+"-"+std::to_string(seconds)+"-"+
        std::to_string(micro)+"-"+std::to_string(seconds)+"-"+
        std::to_string(micro+delta)+"-"+std::to_string(seconds)+"-"+
        std::to_string(micro+2*delta));
}
bool decode(const std::string& encoded,json_t*& root){
    std::string clean;clean.reserve(encoded.size());
    for(char c:encoded)if(c!='\\'&&!std::isspace(static_cast<unsigned char>(c)))clean.push_back(c);
    std::vector<uint8_t> raw(clean.size()*3/4+4);
    int length=av_base64_decode(raw.data(),clean.c_str(),static_cast<int>(raw.size()));
    if(length<=0||length%16)return false;
    AVAES* aes=av_aes_alloc();if(!aes)return false;
    uint8_t currentIv[16];std::copy(iv,iv+16,reinterpret_cast<char*>(currentIv));
    av_aes_init(aes,reinterpret_cast<const uint8_t*>(key),256,1);
    std::vector<uint8_t> plain(length);
    av_aes_crypt(aes,plain.data(),raw.data(),length/16,currentIv,1);
    av_free(aes);
    const size_t pad=plain.back();
    if(pad==0||pad>16||pad>plain.size())return false;
    if(!std::all_of(plain.end()-pad,plain.end(),[pad](uint8_t byte){return byte==pad;}))return false;
    json_error_t error{};
    root=json_loadb(reinterpret_cast<const char*>(plain.data()),plain.size()-pad,0,&error);
    return root!=nullptr;
}
std::string deviceId(){
    const auto ticks=std::chrono::steady_clock::now().time_since_epoch().count();
    std::string id=md5(std::to_string(ticks)+std::to_string(std::rand()));
    return id.substr(0,16);
}
json_t* request(net::HttpClient& http,const std::string& path,
                const std::vector<std::pair<std::string,std::string>>& fields,
                const std::atomic<bool>* cancel,std::string& error){
    if(session.device.empty())session.device=deviceId();
    const std::string stamp=milliseconds();
    net::HttpRequest req;req.url=std::string(base)+path;req.method="POST";req.body=form(fields);
    req.cancel=cancel;req.connectTimeoutSeconds=8;req.timeoutSeconds=20;
    req.maxResponseBytes=2*1024*1024;req.closeConnection=true;
    req.headers={{"User-Agent","okhttp/4.9.0"},{"sys_platform","2"},
        {"device_id",session.device},{"sysrelease","14"},
        {"sign",md5(std::string(requestSecret)+session.device+stamp,true)},
        {"cur_time",stamp},{"channel_code",session.channel},{"mobmodel","Pixel 7"},
        {"version","51000"},{"token",session.token},{"mob_mfr","Google"},
        {"package_name","com.wfighter.zmz"},{"app_id",session.app},
        {"log-header","I am the log request header."},
        {"Content-Type","application/x-www-form-urlencoded"}};
    auto response=http.get(req);if(!response.succeeded){error=response.error;return nullptr;}
    json_t* root=nullptr;if(!decode(response.body,root)){error="闪电响应解密失败";return nullptr;}
    json_t* result=json_object_get(root,"result");
    if(json_integer_value(json_object_get(root,"code"))!=10000||!result){
        error=value(root,"message");if(error.empty())error="闪电接口返回错误";
        json_decref(root);return nullptr;
    }
    json_incref(result);json_decref(root);return result;
}
bool ensureSession(net::HttpClient& http,const std::atomic<bool>* cancel,std::string& error){
    if(!session.token.empty())return true;
    json_t* result=request(http,"/new_public/init_v2",{{"invited_by",""},{"ua",""},{"is_install","0"}},cancel,error);
    if(!result)return false;
    json_t* info=json_object_get(result,"user_info");
    session.token=value(info,"token");
    const std::string app=value(info,"app_id"),channel=value(info,"channel_code");
    if(!app.empty())session.app=app;
    if(!channel.empty())session.channel=channel;
    json_decref(result);
    if(session.token.empty()){error="闪电匿名会话初始化失败";return false;}
    return true;
}
std::vector<std::pair<std::string,std::string>> detailFields(const std::string& id){
    return {{"sig",""},{"nc_token",""},{"code",""},{"phone",""},{"session_id",""},{"vod_id",id}};
}
std::string pathFromUrl(const std::string& url){
    const size_t scheme=url.find("://"),first=url.find('/',scheme==std::string::npos?0:scheme+3);
    if(first==std::string::npos)return "/";
    const size_t query=url.find('?',first);return url.substr(first,query-first);
}
std::string signedUrl(const std::string& url,const std::string& ck){
    const std::string path=pathFromUrl(url);
    std::ostringstream stamp;stamp<<std::hex<<static_cast<unsigned long long>(std::time(nullptr));
    const std::string query=url.find('?')==std::string::npos?"":url.substr(url.find('?')+1);
    const std::string signature=md5(std::string(mediaSecret)+path+stamp.str());
    return url.substr(0,url.find('?'))+'?'+(query.empty()?"":query+'&')+ck+
        "&wsSecret="+signature+"&wsTime="+stamp.str();
}
std::string absoluteUrl(const std::string& baseUrl,const std::string& relative){
    if(relative.rfind("http://",0)==0||relative.rfind("https://",0)==0)return relative;
    if(relative.empty())return {};
    const size_t slash=baseUrl.rfind('/');
    return (slash==std::string::npos?baseUrl:baseUrl.substr(0,slash+1))+relative;
}
std::string cookieValue(const std::string& raw){
    std::vector<uint8_t> decoded(raw.size()+4);
    const int len=av_base64_decode(decoded.data(),raw.c_str(),decoded.size());
    if(len<=0)return raw;
    std::string value(reinterpret_cast<char*>(decoded.data()),len);
    if(value.find('=')==std::string::npos)return raw;
    if(!std::all_of(value.begin(),value.end(),[](char c){return(c>=32&&c<=126)||c=='\r'||c=='\n'||c=='\t';}))return raw;
    return value;
}
}

SearchResponse search(net::HttpClient& http,const std::string& keyword,const std::atomic<bool>* cancel){
    std::lock_guard<std::mutex> lock(sessionMutex);SearchResponse out;std::string error;
    if(!ensureSession(http,cancel,error)){out.error=error;return out;}
    json_t* rows=request(http,"/new_search/result_v2",{{"kw",keyword},{"pn","1"}},cancel,error);
    if(!rows){out.error=error;return out;}
    if(json_is_array(rows))for(size_t i=0;i<json_array_size(rows);++i){
        json_t* row=json_array_get(rows,i);SearchResult item;
        item.title=value(row,"vod_name");item.posterUrl=value(row,"vod_pic");
        item.status=value(row,"remarks");item.detailUrl="okswitch:yueyue:"+value(row,"id");
        if(!item.title.empty()&&item.detailUrl!="okswitch:yueyue:")out.items.push_back(std::move(item));
    }
    json_decref(rows);out.total=out.items.size();out.succeeded=true;return out;
}
DetailResponse detail(net::HttpClient& http,const std::string& id,const std::atomic<bool>* cancel){
    std::lock_guard<std::mutex> lock(sessionMutex);DetailResponse out;std::string error;
    if(!ensureSession(http,cancel,error)){out.error=error;return out;}
    json_t* vod=request(http,"/new_video/result_v2",detailFields(id),cancel,error);
    if(!vod){out.error=error;return out;}
    out.detail.title=value(vod,"vod_name");out.detail.posterUrl=value(vod,"vod_pic");
    out.detail.year=value(vod,"vod_year");out.detail.status=value(vod,"remarks");
    out.detail.description=value(vod,"vod_blurb");out.detail.actors=value(vod,"vod_actor");
    out.detail.director=value(vod,"vod_director");
    json_t* episodes=json_object_get(vod,"vod_collection");
    if(json_is_array(episodes))for(size_t i=0;i<json_array_size(episodes);++i){
        json_t* row=json_array_get(episodes,i);const std::string episodeId=value(row,"id");
        if(episodeId.empty())continue;
        EpisodeLink entry;entry.label=value(row,"title");
        if(entry.label.empty())entry.label="第"+std::to_string(i+1)+"集";
        json_t* data=json_pack("{s:s,s:s,s:s,s:s}","vod_id",id.c_str(),"collection_id",episodeId.c_str(),
            "vod_token",value(row,"vod_token").c_str(),"cur_time",value(row,"cur_time").c_str());
        char* dump=json_dumps(data,JSON_COMPACT);entry.opaque=dump?dump:"";
        if(dump)std::free(dump);
        json_decref(data);out.detail.episodes.push_back(std::move(entry));
    }
    json_decref(vod);
    if(!out.detail.episodes.empty())out.detail.shares.push_back({"闪电 | 秒播","okswitch:yueyue"});
    out.succeeded=!out.detail.title.empty()&&!out.detail.episodes.empty();
    if(!out.succeeded)out.error="闪电暂无可播放剧集";
    return out;
}
cloud::ProviderResult<cloud::PlaybackSource> playback(net::HttpClient& http,const std::string& opaque,const std::atomic<bool>* cancel){
    std::lock_guard<std::mutex> lock(sessionMutex);cloud::ProviderResult<cloud::PlaybackSource> out;std::string error;
    if(!ensureSession(http,cancel,error)){out.error={cloud::ProviderErrorCode::NetworkError,error};return out;}
    json_error_t parseError{};json_t* data=json_loadb(opaque.data(),opaque.size(),0,&parseError);
    if(!data){out.error={cloud::ProviderErrorCode::InvalidResponse,"闪电剧集参数无效"};return out;}
    const std::string id=value(data,"vod_id");auto fields=detailFields(id);
    fields.push_back({"collection_id",value(data,"collection_id")});
    fields.push_back({"vod_token",value(data,"vod_token")});
    fields.push_back({"cur_time",value(data,"cur_time")});json_decref(data);
    json_t* stream=request(http,"/new_video/collection_v2",fields,cancel,error);
    if(!stream){out.error={cloud::ProviderErrorCode::NetworkError,error};return out;}
    if(value(stream,"vid")=="1"&&!value(stream,"check_page_url").empty()){
        json_decref(stream);out.error={cloud::ProviderErrorCode::AuthenticationRequired,"闪电要求设备滑块验证"};return out;
    }
    std::string playlistUrl;
    for(const char* field:{"out_vod_url","vod_url","tc_vod_url","url","play_url","proxy"}){
        playlistUrl=value(stream,field);if(!playlistUrl.empty())break;
    }
    std::string ck;
    for(const char* field:{"ck","cookie","token"}){ck=value(stream,field);if(!ck.empty())break;}
    if(ck.empty())ck=session.token;
    ck=cookieValue(ck);json_decref(stream);
    if(playlistUrl.empty()||ck.empty()){
        out.error={cloud::ProviderErrorCode::InvalidResponse,"闪电没有返回有效播放参数"};return out;
    }
    net::HttpRequest req;req.url=signedUrl(playlistUrl,ck);req.cancel=cancel;
    req.connectTimeoutSeconds=8;req.timeoutSeconds=20;req.maxResponseBytes=1024*1024;
    req.headers={{"User-Agent","Mozi"},{"Accept","*/*"},{"Badci",badci()}};
    auto response=http.get(req);
    if((!response.succeeded||response.body.find("#EXTM3U")==std::string::npos)
        &&playlistUrl.find("index3.m3u8")!=std::string::npos){
        playlistUrl.replace(playlistUrl.find("index3.m3u8"),std::strlen("index3.m3u8"),"index.m3u8");
        req.url=signedUrl(playlistUrl,ck);response=http.get(req);
    }
    if(!response.succeeded||response.body.find("#EXTM3U")==std::string::npos){
        out.error={cloud::ProviderErrorCode::NetworkError,"闪电播放清单加载失败："+response.error};return out;
    }
    std::istringstream lines(response.body);std::string line;double pending=0,total=0;
    while(std::getline(lines,line)){
        if(!line.empty()&&line.back()=='\r')line.pop_back();
        if(line.rfind("#EXTINF:",0)==0){pending=std::strtod(line.c_str()+8,nullptr);continue;}
        if(line.empty()||line[0]=='#'||pending<=0)continue;
        const std::string segment=absoluteUrl(playlistUrl,line);
        if(segment.empty())continue;
        out.value.segmentUrls.push_back(segment);
        const auto pos=segment.find("sz=");
        out.value.segmentSizes.push_back(pos==std::string::npos?0:std::strtoull(segment.c_str()+pos+3,nullptr,10));
        out.value.segmentDurations.push_back(pending);total+=pending;pending=0;
    }
    if(out.value.segmentUrls.empty()){
        out.error={cloud::ProviderErrorCode::InvalidResponse,"闪电播放清单没有有效分片"};return out;
    }
    out.value.url=playlistUrl;out.value.durationSeconds=total;
    out.value.parallelFetches=3;out.value.initialBufferSegments=3;
    out.value.headers={{"User-Agent","Mozi"},{"Accept","*/*"},{"Badci",req.headers.back().value}};
    out.value.refreshSegmentUrl=[ck](const std::string& segment){return signedUrl(segment,ck);};
    out.succeeded=true;return out;
}
}
