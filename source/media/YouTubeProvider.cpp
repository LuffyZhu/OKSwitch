#include "okswitch/media/YouTubeProvider.hpp"
#include <jansson.h>
#include <algorithm>
#include <cctype>
#include <climits>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>

namespace okswitch::media {
namespace {
constexpr const char* configPath="sdmc:/switch/OKSwitch/youtube-oauth.json";
constexpr const char* sessionPath="sdmc:/switch/OKSwitch/youtube-session.json";
constexpr const char* tvSessionPath="sdmc:/switch/OKSwitch/youtube-tv-session.json";
constexpr const char* cookiePath="sdmc:/switch/OKSwitch/youtube-cookie.txt";
constexpr const char* profilePath="sdmc:/switch/OKSwitch/youtube-profile-url.txt";
std::string readProfileUrl(){
    FILE* file=std::fopen(profilePath,"rb");if(!file)return {};
    char data[2049]{};const size_t count=std::fread(data,1,sizeof(data)-1,file);
    const bool complete=std::feof(file)!=0;std::fclose(file);
    std::string value(data,count);
    return complete&&value.rfind("https://",0)==0
        &&value.find_first_of("\r\n")==std::string::npos
        &&value.find('\0')==std::string::npos?value:std::string();
}
void saveProfileUrl(const std::string& url){
    if(url.size()>2048||url.rfind("https://",0)!=0)return;
    FILE* file=std::fopen(profilePath,"wb");if(!file)return;
    const bool written=std::fwrite(url.data(),1,url.size(),file)==url.size();
    if(std::fclose(file)!=0||!written)std::remove(profilePath);
}
std::string readCookie(){
    FILE* file=std::fopen(cookiePath,"rb");if(!file)return {};
    char data[32769]{};const size_t count=std::fread(data,1,sizeof(data)-1,file);std::fclose(file);
    if(count>=sizeof(data)-1)return {};
    std::string value(data,count);
    if(value.compare(0,3,"\xEF\xBB\xBF")==0)value.erase(0,3);
    if(value.rfind("Cookie:",0)==0||value.rfind("cookie:",0)==0)value.erase(0,7);
    const size_t begin=value.find_first_not_of(" \r\n\t"),end=value.find_last_not_of(" \r\n\t");
    if(begin==std::string::npos)return {};
    value=value.substr(begin,end-begin+1);
    if(value.find_first_of("\r\n")!=std::string::npos||value.find('\0')!=std::string::npos||value.find('=')==std::string::npos)return {};
    return value;
}
constexpr const char* androidAgent="com.google.android.youtube/20.10.38 (Linux; U; Android 11) gzip";
constexpr const char* tvAgent="Mozilla/5.0 (ChromiumStylePlatform) Cobalt/25.lts.30.1034943-gold (unlike Gecko), Unknown_TV_Unknown_0/Unknown (Unknown, Unknown)";
constexpr const char* tvVersion="7.20260707.07.00";
std::string field(json_t* root,const char* name){auto* value=json_object_get(root,name);return json_is_string(value)?json_string_value(value):"";}
std::string textValue(json_t* value){
    if(json_is_string(value))return json_string_value(value);
    auto* simple=json_object_get(value,"simpleText");if(json_is_string(simple))return json_string_value(simple);
    auto* runs=json_object_get(value,"runs");std::string result;
    if(json_is_array(runs))for(size_t i=0;i<json_array_size(runs);++i)result+=field(json_array_get(runs,i),"text");
    return result;
}
std::string thumbnail(json_t* value){
    auto* list=json_object_get(value,"thumbnails");if(!json_is_array(list)||!json_array_size(list))return {};
    std::string chosen,fallback;int chosenWidth=INT_MAX,fallbackWidth=0;
    for(size_t i=0;i<json_array_size(list);++i){
        auto* image=json_array_get(list,i);const auto url=field(image,"url");
        const int width=(int)json_integer_value(json_object_get(image,"width"));
        if(url.empty())continue;
        if(fallback.empty()||width>fallbackWidth){fallback=url;fallbackWidth=width;}
        if(width>=320&&width<chosenWidth){chosen=url;chosenWidth=width;}
    }
    return chosen.empty()?fallback:chosen;
}
std::string compactVideoThumbnail(const std::string& id,const std::string& original){
    if(id.size()!=11||(!original.empty()&&original.find("ytimg.com")==std::string::npos))
        return original;
    if(!std::all_of(id.begin(),id.end(),[](unsigned char ch){
        return (ch>='A'&&ch<='Z')||(ch>='a'&&ch<='z')||(ch>='0'&&ch<='9')
            ||ch=='-'||ch=='_';}))return original;
    // A fixed 320 px JPEG is sufficient for the 299 px TV card. Stable URLs
    // also let the SD cache serve the same video across home and history.
    return "https://i.ytimg.com/vi/"+id+"/mqdefault.jpg";
}
std::string apiThumbnail(json_t* value){
    // Cards are about 300 px wide; avoid downloading 1280 px artwork first.
    for(const char* size:{"medium","high","standard","maxres","default"}){
        const auto url=field(json_object_get(value,size),"url");if(!url.empty())return url;
    }
    return {};
}
std::string displayDuration(int seconds){
    if(seconds<=0)return {};
    const int hours=seconds/3600,minutes=(seconds/60)%60;
    char value[32]{};
    if(hours)std::snprintf(value,sizeof(value),"%d:%02d:%02d",hours,minutes,seconds%60);
    else std::snprintf(value,sizeof(value),"%d:%02d",minutes,seconds%60);
    return value;
}
std::string isoDuration(const std::string& iso){
    if(iso.empty()||iso[0]!='P')return {};
    int total=0,part=0;bool clock=false;
    for(size_t i=1;i<iso.size();++i){const char ch=iso[i];
        if(ch=='T'){clock=true;continue;}
        if(ch>='0'&&ch<='9'){part=part*10+ch-'0';continue;}
        if(ch=='D')total+=part*86400;
        else if(clock&&ch=='H')total+=part*3600;
        else if(clock&&ch=='M')total+=part*60;
        else if(clock&&ch=='S')total+=part;
        part=0;
    }
    return displayDuration(total);
}
std::string rendererDuration(json_t* video){
    auto value=textValue(json_object_get(video,"lengthText"));
    if(!value.empty())return value;
    auto* overlays=json_object_get(video,"thumbnailOverlays");
    if(json_is_array(overlays))for(size_t i=0;i<json_array_size(overlays);++i){
        value=textValue(json_object_get(json_object_get(json_array_get(overlays,i),"thumbnailOverlayTimeStatusRenderer"),"text"));
        if(!value.empty())return value;
    }
    return {};
}
void gatherVideos(json_t* node,std::vector<YouTubeItem>& out,int depth){
    if(!node||depth>24||out.size()>=60)return;
    if(json_is_array(node)){for(size_t i=0;i<json_array_size(node);++i)gatherVideos(json_array_get(node,i),out,depth+1);return;}
    if(!json_is_object(node))return;
    auto* lockup=json_object_get(node,"lockupViewModel");
    const auto lockupId=field(lockup,"contentId");
    if(field(lockup,"contentType")=="LOCKUP_CONTENT_TYPE_VIDEO"&&!lockupId.empty()
        &&std::none_of(out.begin(),out.end(),[&](const auto& item){return item.id==lockupId;})){
        YouTubeItem item;item.id=lockupId;item.duration=rendererDuration(lockup);
        auto* metadata=json_object_get(json_object_get(lockup,"metadata"),"lockupMetadataViewModel");
        item.title=field(json_object_get(metadata,"title"),"content");
        auto* image=json_object_get(json_object_get(json_object_get(lockup,"contentImage"),"thumbnailViewModel"),"image");
        auto* sources=json_object_get(image,"sources");
        if(json_is_array(sources)){
            int chosenWidth=INT_MAX,fallbackWidth=0;std::string fallback;
            for(size_t i=0;i<json_array_size(sources);++i){
                auto* source=json_array_get(sources,i);const auto url=field(source,"url");
                const int width=(int)json_integer_value(json_object_get(source,"width"));
                if(url.empty())continue;
                if(fallback.empty()||width>fallbackWidth){fallback=url;fallbackWidth=width;}
                if(width>=320&&width<chosenWidth){item.thumbnail=url;chosenWidth=width;}
            }
            if(item.thumbnail.empty())item.thumbnail=fallback;
        }
        if(item.thumbnail.rfind("//",0)==0)item.thumbnail="https:"+item.thumbnail;
        item.thumbnail=compactVideoThumbnail(item.id,item.thumbnail);
        if(!item.title.empty())out.push_back(std::move(item));
    }
    // YouTube's TV browse response represents account home and history videos as tiles.
    auto* tile=json_object_get(node,"tileRenderer");
    if(json_is_object(tile)){
        const auto id=field(json_object_get(json_object_get(tile,"onSelectCommand"),"watchEndpoint"),"videoId");
        if(!id.empty()&&std::none_of(out.begin(),out.end(),[&](const auto& item){return item.id==id;})){
            YouTubeItem item;item.id=id;item.duration=rendererDuration(tile);
            auto* metadata=json_object_get(json_object_get(tile,"metadata"),"tileMetadataRenderer");
            item.title=textValue(json_object_get(metadata,"title"));
            item.channel=textValue(json_object_get(json_object_get(tile,"onLongPressCommand"),"subtitle"));
            if(item.channel.empty())item.channel=textValue(json_object_get(json_object_get(json_object_get(tile,"onLongPressCommand"),"showMenuCommand"),"subtitle"));
            auto* header=json_object_get(json_object_get(tile,"header"),"tileHeaderRenderer");
            item.thumbnail=thumbnail(json_object_get(header,"thumbnail"));
            if(item.thumbnail.empty())item.thumbnail="https://i.ytimg.com/vi/"+id+"/hqdefault.jpg";
            if(item.thumbnail.rfind("//",0)==0)item.thumbnail="https:"+item.thumbnail;
            item.thumbnail=compactVideoThumbnail(item.id,item.thumbnail);
            if(!item.title.empty())out.push_back(std::move(item));
        }
    }
    for(const char* key:{"videoRenderer","compactVideoRenderer","gridVideoRenderer","richGridVideoRenderer"}){
        auto* video=json_object_get(node,key);if(!json_is_object(video))continue;
        const std::string id=field(video,"videoId");
        if(id.empty()||std::any_of(out.begin(),out.end(),[&](const auto& item){return item.id==id;}))continue;
        YouTubeItem item;item.id=id;item.title=textValue(json_object_get(video,"title"));item.duration=rendererDuration(video);
        item.channel=textValue(json_object_get(video,"ownerText"));
        item.thumbnail=thumbnail(json_object_get(video,"thumbnail"));
        item.thumbnail=compactVideoThumbnail(item.id,item.thumbnail);
        if(!item.title.empty())out.push_back(std::move(item));
    }
    const char* key;json_t* value;
    json_object_foreach(node,key,value)gatherVideos(value,out,depth+1);
}
std::string encode(const std::string& value){
    static const char digits[]="0123456789ABCDEF";std::string out;
    for(unsigned char ch:value){if((ch>='A'&&ch<='Z')||(ch>='a'&&ch<='z')||(ch>='0'&&ch<='9')||ch=='-'||ch=='_'||ch=='.'||ch=='~')out+=(char)ch;
        else{out+='%';out+=digits[ch>>4];out+=digits[ch&15];}}
    return out;
}
void gatherChannels(json_t* node,std::vector<YouTubeItem>& out,int depth){
    if(!node||depth>30||out.size()>=500)return;
    if(json_is_array(node)){for(size_t i=0;i<json_array_size(node);++i)gatherChannels(json_array_get(node,i),out,depth+1);return;}
    if(!json_is_object(node))return;
    auto* channel=json_object_get(node,"channelRenderer");
    const auto id=field(channel,"channelId");
    if(!id.empty()&&std::none_of(out.begin(),out.end(),[&](const auto& item){return item.id==id;})){
        YouTubeItem item;item.id=id;item.title=textValue(json_object_get(channel,"title"));item.thumbnail=thumbnail(json_object_get(channel,"thumbnail"));item.isChannel=true;
        if(item.thumbnail.rfind("//",0)==0)item.thumbnail="https:"+item.thumbnail;
        out.push_back(std::move(item));
    }
    const char* key;json_t* value;json_object_foreach(node,key,value)gatherChannels(value,out,depth+1);
}
net::HttpRequest form(const char* url,const std::string& body,const std::atomic<bool>* cancel){
    net::HttpRequest req;req.url=url;req.method="POST";req.body=body;req.cancel=cancel;req.maxResponseBytes=2*1024*1024;
    req.headers.push_back({"Content-Type","application/x-www-form-urlencoded"});return req;
}
json_t* parse(const std::string& body){json_error_t error{};return json_loadb(body.data(),body.size(),0,&error);}
std::string responseError(const net::HttpResponse& response){
    auto* root=parse(response.body);std::string error=field(root,"error_description");
    if(error.empty()){auto* detail=json_object_get(root,"error");error=json_is_string(detail)?json_string_value(detail):field(detail,"message");}
    if(root)json_decref(root);
    return error.empty()?(response.error.empty()?"HTTP "+std::to_string(response.statusCode):response.error):error;
}
std::string redirectLocation(const net::HttpResponse& response){
    for(const auto& header:response.headers){
        std::string name=header.name;
        std::transform(name.begin(),name.end(),name.begin(),[](unsigned char ch){return std::tolower(ch);});
        if(name=="location")return header.value;
    }
    return {};
}
std::string youtubeRedirect(const std::string& location){
    constexpr const char* origin="https://www.youtube.com";
    if(location.size()>1&&location[0]=='/'&&location[1]!='/')return std::string(origin)+location;
    if(location.rfind(origin,0)==0&&
        (location.size()==std::char_traits<char>::length(origin)||
         location[std::char_traits<char>::length(origin)]=='/'||
         location[std::char_traits<char>::length(origin)]=='?'))return location;
    return {};
}
bool saveSession(const std::string& refresh,bool browseScopeDenied=false){
    if(mkdir("sdmc:/switch",0777)!=0&&errno!=EEXIST)return false;
    if(mkdir("sdmc:/switch/OKSwitch",0777)!=0&&errno!=EEXIST)return false;
    auto* root=json_pack("{s:s,s:b}","refresh_token",refresh.c_str(),
        "tv_browse_scope_denied",browseScopeDenied);
    const std::string temporary=std::string(sessionPath)+".tmp";
    const bool ok=root&&json_dump_file(root,temporary.c_str(),JSON_INDENT(2))==0;
    if(root)json_decref(root);
    if(!ok)return false;
    const std::string backup=std::string(sessionPath)+".bak";
    std::remove(backup.c_str());
    if(std::rename(sessionPath,backup.c_str())!=0&&errno!=ENOENT)return false;
    if(std::rename(temporary.c_str(),sessionPath)==0){std::remove(backup.c_str());return true;}
    std::rename(backup.c_str(),sessionPath);return false;
}
bool saveTvSession(const std::string& refresh){
    if(mkdir("sdmc:/switch",0777)!=0&&errno!=EEXIST)return false;
    if(mkdir("sdmc:/switch/OKSwitch",0777)!=0&&errno!=EEXIST)return false;
    auto* root=json_pack("{s:s}","refresh_token",refresh.c_str());
    const std::string temporary=std::string(tvSessionPath)+".tmp";
    const bool ok=root&&json_dump_file(root,temporary.c_str(),JSON_INDENT(2))==0;
    if(root)json_decref(root);
    if(!ok)return false;
    const std::string backup=std::string(tvSessionPath)+".bak";
    std::remove(backup.c_str());
    if(std::rename(tvSessionPath,backup.c_str())!=0&&errno!=ENOENT)return false;
    if(std::rename(temporary.c_str(),tvSessionPath)==0){std::remove(backup.c_str());return true;}
    std::rename(backup.c_str(),tvSessionPath);return false;
}
}
YouTubeProvider::YouTubeProvider(net::HttpClient& http,std::string proxyUrl,int maxHeight):http_(http),proxyUrl_(std::move(proxyUrl)),maxHeight_(maxHeight){
    json_error_t error{};auto* config=json_load_file(configPath,0,&error);
    auto* client=json_object_get(config,"installed");if(!json_is_object(client))client=config;
    clientId_=field(client,"client_id");clientSecret_=field(client,"client_secret");if(config)json_decref(config);
    auto* session=json_load_file(sessionPath,0,&error);refreshToken_=field(session,"refresh_token");
    accountBrowseScopeDenied_=json_is_true(json_object_get(session,"tv_browse_scope_denied"));
    if(session)json_decref(session);
    session=json_load_file(tvSessionPath,0,&error);tvRefreshToken_=field(session,"refresh_token");
    if(session)json_decref(session);
    authenticated_.store(!refreshToken_.empty()||!tvRefreshToken_.empty());login_.authenticated=authenticated_.load();login_.message=login_.authenticated?"已保存 YouTube 登录":"尚未登录";
    if(authenticated_.load())profileImageUrl_=readProfileUrl();
    mutex_=SDL_CreateMutex();condition_=SDL_CreateCond();if(mutex_&&condition_)thread_=SDL_CreateThread(entry,"youtube",this);
}
YouTubeProvider::~YouTubeProvider(){shutdown();}
void YouTubeProvider::shutdown(){
    profileCancel_.store(true);
    if(mutex_){SDL_LockMutex(mutex_);stopping_=true;cancel_.store(true);if(condition_)SDL_CondSignal(condition_);SDL_UnlockMutex(mutex_);}
    if(thread_){SDL_WaitThread(thread_,nullptr);thread_=nullptr;}
    if(profileThread_){SDL_WaitThread(profileThread_,nullptr);profileThread_=nullptr;}
    if(condition_){SDL_DestroyCond(condition_);condition_=nullptr;}if(mutex_){SDL_DestroyMutex(mutex_);mutex_=nullptr;}
}
void YouTubeProvider::cancel(){cancel_.store(true);if(mutex_){SDL_LockMutex(mutex_);++serial_;pending_=false;ready_=false;login_.running=false;SDL_UnlockMutex(mutex_);}}
void YouTubeProvider::search(const std::string& value){if(!mutex_)return;SDL_LockMutex(mutex_);++serial_;action_=Action::Search;value_=value;pending_=true;ready_=false;cancel_.store(true);SDL_CondSignal(condition_);SDL_UnlockMutex(mutex_);}
void YouTubeProvider::subscriptions(){if(!mutex_)return;SDL_LockMutex(mutex_);++serial_;action_=Action::Subscriptions;value_.clear();pending_=true;ready_=false;cancel_.store(true);SDL_CondSignal(condition_);SDL_UnlockMutex(mutex_);}
void YouTubeProvider::accountHistory(){if(!mutex_)return;SDL_LockMutex(mutex_);++serial_;action_=Action::AccountHistory;value_.clear();pending_=true;ready_=false;cancel_.store(true);SDL_CondSignal(condition_);SDL_UnlockMutex(mutex_);}
void YouTubeProvider::home(){if(!mutex_)return;SDL_LockMutex(mutex_);++serial_;action_=Action::Home;value_.clear();pending_=true;ready_=false;cancel_.store(true);SDL_CondSignal(condition_);SDL_UnlockMutex(mutex_);}
void YouTubeProvider::durations(const std::vector<YouTubeItem>& items){
    std::string ids;
    for(const auto& item:items){if(item.isChannel||!item.duration.empty()||item.id.empty())continue;
        if(!ids.empty())ids+=',';
        ids+=item.id;
        if(ids.size()>900)break;
    }
    if(ids.empty()||!mutex_||!signedIn())return;
    SDL_LockMutex(mutex_);++serial_;action_=Action::Durations;value_=std::move(ids);pending_=true;ready_=false;
    // Let the profile request finish first; this metadata request is optional.
    SDL_CondSignal(condition_);SDL_UnlockMutex(mutex_);
}
void YouTubeProvider::channelVideos(const std::string& value){if(!mutex_)return;SDL_LockMutex(mutex_);++serial_;action_=Action::ChannelVideos;value_=value;pending_=true;ready_=false;cancel_.store(true);SDL_CondSignal(condition_);SDL_UnlockMutex(mutex_);}
void YouTubeProvider::playback(const std::string& value){if(!mutex_)return;SDL_LockMutex(mutex_);++serial_;action_=Action::Playback;value_=value;pending_=true;ready_=false;cancel_.store(true);SDL_CondSignal(condition_);SDL_UnlockMutex(mutex_);}
void YouTubeProvider::beginLogin(){if(!mutex_)return;SDL_LockMutex(mutex_);++serial_;action_=Action::Login;value_.clear();pending_=true;ready_=false;login_={};login_.running=true;login_.message="正在申请配对码...";profileImageUrl_.clear();profileAttempted_=false;cancel_.store(true);SDL_CondSignal(condition_);SDL_UnlockMutex(mutex_);std::remove(profilePath);}
void YouTubeProvider::beginTvLoginProbe(){if(!mutex_)return;SDL_LockMutex(mutex_);++serial_;action_=Action::TvLoginProbe;value_.clear();pending_=true;ready_=false;login_={};login_.running=true;login_.message="正在申请电视端实验配对码...";profileImageUrl_.clear();profileAttempted_=false;cancel_.store(true);SDL_CondSignal(condition_);SDL_UnlockMutex(mutex_);std::remove(profilePath);}
void YouTubeProvider::signOut(){cancel();profileCancel_.store(true);authenticated_.store(false);refreshToken_.clear();token_.clear();tokenExpires_=0;tvRefreshToken_.clear();tvToken_.clear();tvTokenExpires_=0;SDL_LockMutex(mutex_);cookie_.clear();rejectedCookie_.clear();profileImageUrl_.clear();profileAttempted_=false;accountBrowseScopeDenied_=false;listCache_.clear();SDL_UnlockMutex(mutex_);std::remove(cookiePath);std::remove(sessionPath);std::remove(tvSessionPath);std::remove(profilePath);setLoginState({false,false,"","","已退出 YouTube 账号"});}
bool YouTubeProvider::poll(YouTubeResult& value){if(!mutex_)return false;SDL_LockMutex(mutex_);bool ready=ready_;if(ready){value=std::move(result_);ready_=false;}SDL_UnlockMutex(mutex_);return ready;}
YouTubeLoginState YouTubeProvider::loginState(){if(!mutex_)return {};SDL_LockMutex(mutex_);auto value=login_;SDL_UnlockMutex(mutex_);return value;}
std::string YouTubeProvider::profileImage(){if(!mutex_)return {};SDL_LockMutex(mutex_);auto value=profileImageUrl_;SDL_UnlockMutex(mutex_);return value;}
std::string YouTubeProvider::profileStatus(){if(!mutex_)return {};SDL_LockMutex(mutex_);auto value=profileStatus_;SDL_UnlockMutex(mutex_);return value;}
bool YouTubeProvider::signedIn() const{return authenticated_.load();}
bool YouTubeProvider::configured() const{return (!clientId_.empty()&&!clientSecret_.empty())||!readCookie().empty();}
void YouTubeProvider::reloadConfig(){
    json_error_t error{};auto* config=json_load_file(configPath,0,&error);
    if(config){auto* client=json_object_get(config,"installed");if(!json_is_object(client))client=config;
        clientId_=field(client,"client_id");clientSecret_=field(client,"client_secret");json_decref(config);clearListCache();}
}
std::string YouTubeProvider::cookie(){SDL_LockMutex(mutex_);auto value=cookie_;SDL_UnlockMutex(mutex_);return value;}
void YouTubeProvider::setProxy(const std::string& url){if(!mutex_)return;SDL_LockMutex(mutex_);if(proxyUrl_!=url){proxyUrl_=url;listCache_.clear();}SDL_UnlockMutex(mutex_);}
void YouTubeProvider::clearListCache(){if(!mutex_)return;SDL_LockMutex(mutex_);listCache_.clear();SDL_UnlockMutex(mutex_);}
std::string YouTubeProvider::proxy(){if(!mutex_)return {};SDL_LockMutex(mutex_);auto value=proxyUrl_;SDL_UnlockMutex(mutex_);return value;}
void YouTubeProvider::setLoginState(const YouTubeLoginState& value){if(!mutex_)return;SDL_LockMutex(mutex_);login_=value;SDL_UnlockMutex(mutex_);}
int YouTubeProvider::entry(void* self){return static_cast<YouTubeProvider*>(self)->run();}
int YouTubeProvider::run(){
    for(;;){SDL_LockMutex(mutex_);while(!pending_&&!stopping_)SDL_CondWait(condition_,mutex_);
        if(stopping_){SDL_UnlockMutex(mutex_);return 0;}const Action action=action_;const auto value=value_;const unsigned serial=serial_;
        pending_=false;cancel_.store(false);SDL_UnlockMutex(mutex_);
        auto result=perform(action,value);SDL_LockMutex(mutex_);
        if(!stopping_&&serial==serial_){result_=std::move(result);ready_=true;}
        const bool loadProfile=action==Action::Home&&authenticated_.load()&&!clientId_.empty()
            &&(!profileAttempted_||(profileImageUrl_.empty()&&SDL_GetTicks()-profileLastAttempt_>=60000));
        if(loadProfile){profileAttempted_=true;profileLastAttempt_=SDL_GetTicks();profileStatus_="正在读取账号头像";}
        SDL_UnlockMutex(mutex_);
        // Profile lookup must not hold the request worker: the user may open a
        // video immediately after the home feed arrives.
        if(loadProfile&&!cancel_.load()){
            if(profileThread_&&profileDone_.load()){SDL_WaitThread(profileThread_,nullptr);profileThread_=nullptr;}
            if(!profileThread_){
                profileToken_=std::time(nullptr)<tokenExpires_?token_:std::string();
                profileRefreshToken_=refreshToken_;profileClientId_=clientId_;
                profileClientSecret_=clientSecret_;profileProxy_=proxy();
                profileTvToken_=std::time(nullptr)<tvTokenExpires_?tvToken_:std::string();
                profileTvRefreshToken_=tvRefreshToken_;
                if(!profileTvToken_.empty()||!profileTvRefreshToken_.empty()
                    ||!profileToken_.empty()||!profileRefreshToken_.empty()){
                    profileCancel_.store(false);profileDone_.store(false);
                    profileThread_=SDL_CreateThread([](void* context)->int{
                        auto* self=static_cast<YouTubeProvider*>(context);
                        std::string tvToken=self->profileTvToken_;
                        if(tvToken.empty()&&!self->profileTvRefreshToken_.empty()
                            &&!self->profileCancel_.load()){
                            auto* body=json_pack("{s:s,s:s,s:s,s:s}",
                                "refresh_token",self->profileTvRefreshToken_.c_str(),
                                "client_id",self->profileClientId_.c_str(),
                                "client_secret",self->profileClientSecret_.c_str(),
                                "grant_type","refresh_token");
                            if(body){
                                char* raw=json_dumps(body,JSON_COMPACT);json_decref(body);
                                if(raw){net::HttpRequest req;req.url="https://www.youtube.com/o/oauth2/token";
                                    req.method="POST";req.body=raw;free(raw);
                                    req.headers={{"Content-Type","application/json"}};
                                    req.proxyUrl=self->profileProxy_;req.cancel=&self->profileCancel_;
                                    req.maxResponseBytes=256*1024;req.timeoutSeconds=12;
                                    const auto response=self->http_.get(req);
                                    if(response.succeeded){auto* root=parse(response.body);
                                        tvToken=field(root,"access_token");if(root)json_decref(root);}
                                }
                            }
                        }
                        bool loaded=!tvToken.empty()&&!self->profileCancel_.load()
                            &&self->loadTvProfileNow(tvToken,self->profileProxy_);
                        std::string token=self->profileToken_;
                        if(!loaded&&token.empty()&&!self->profileRefreshToken_.empty()
                            &&!self->profileCancel_.load()){
                            auto req=form("https://oauth2.googleapis.com/token",
                                "client_id="+encode(self->profileClientId_)+"&client_secret="
                                +encode(self->profileClientSecret_)+"&refresh_token="
                                +encode(self->profileRefreshToken_)+"&grant_type=refresh_token",
                                &self->profileCancel_);
                            req.proxyUrl=self->profileProxy_;
                            const auto response=self->http_.get(req);
                            if(response.succeeded){auto* root=parse(response.body);
                                token=field(root,"access_token");if(root)json_decref(root);}
                        }
                        if(!loaded&&!token.empty())self->loadProfileNow(token,self->profileProxy_);
                        self->profileDone_.store(true);return 0;
                    },"youtube-profile",this);
                    if(!profileThread_)profileDone_.store(true);
                }
            }
        }
    }
}
YouTubeResult YouTubeProvider::perform(Action action,const std::string& value){
    const Uint32 ttl=action==Action::Search?10*60*1000u:
        action==Action::Subscriptions||action==Action::ChannelVideos?5*60*1000u:
        action==Action::Home||action==Action::AccountHistory?2*60*1000u:0;
    if(ttl){
        SDL_LockMutex(mutex_);
        const Uint32 now=SDL_GetTicks();
        for(auto it=listCache_.begin();it!=listCache_.end();){
            if(now-it->createdAt>=10*60*1000u)it=listCache_.erase(it);
            else ++it;
        }
        for(const auto& entry:listCache_)if(entry.action==action&&entry.value==value&&now-entry.createdAt<ttl){
            auto cached=entry.result;SDL_UnlockMutex(mutex_);return cached;
        }
        SDL_UnlockMutex(mutex_);
    }
    YouTubeResult result;
    switch(action){case Action::Home:result=homeNow();break;case Action::Search:result=searchNow(value);break;
        case Action::Subscriptions:result=subscriptionsNow();break;case Action::ChannelVideos:result=channelVideosNow(value);break;
        case Action::Playback:result=playbackNow(value);break;case Action::Login:result=loginNow();break;
        case Action::TvLoginProbe:result=tvLoginProbeNow();break;
        case Action::AccountHistory:result=personalBrowseNow("FEhistory","/feed/history","账号观看历史");break;
        case Action::Durations:result=durationsNow(value);break;default:break;}
    if(ttl&&result.succeeded&&!cancel_.load()){
        SDL_LockMutex(mutex_);
        if(!cancel_.load()){
            for(auto it=listCache_.begin();it!=listCache_.end();){
                if(it->action==action&&it->value==value)it=listCache_.erase(it);else ++it;
            }
            if(listCache_.size()>=24)listCache_.pop_front();
            listCache_.push_back({action,value,SDL_GetTicks(),result});
        }
        SDL_UnlockMutex(mutex_);
    }
    return result;
}
YouTubeResult YouTubeProvider::homeNow(){
    auto webCookie=cookie();if(webCookie.empty())webCookie=readCookie();
    if((!clientId_.empty()&&(!refreshToken_.empty()||!tvRefreshToken_.empty()||!tvToken_.empty()))||!webCookie.empty()){
        auto personal=personalBrowseNow("default","/","账号推荐");
        if(personal.succeeded||cancel_.load())return personal;
        if(!clientId_.empty()&&!refreshToken_.empty()){
            auto channels=subscriptionsNow(1);
            if(channels.succeeded){channels.message="推荐暂不可用 · 已显示订阅频道";return channels;}
        }
        if(personal.error.empty())personal.error="账号推荐暂不可用";
        if(accountBrowseScopeDenied_&&tvRefreshToken_.empty()&&tvToken_.empty())
            personal.error="现有配对权限无法读取推荐 · 请在设置的 YouTube 账号按 + 测试电视端配对";
        return personal;
    }
    if(signedIn()){
        YouTubeResult result;result.error="个性化首页需要网页 Cookie，请在设置中导入；仍可查看订阅";return result;
    }
    auto result=searchNow("热门视频");
    if(!result.succeeded&&!cancel_.load()&&proxy().empty())
        result.error+=" · 请检查直连网络，或在设置中选择代理节点";
    return result;
}
YouTubeResult YouTubeProvider::personalBrowseNow(const char* browseId,const char* path,const char* label){
    YouTubeResult result;
    const bool hasDeviceLogin=!clientId_.empty()&&(!refreshToken_.empty()||!tvRefreshToken_.empty()||!tvToken_.empty());
    if(hasDeviceLogin){
        result=accountBrowseNow(browseId,label);
        if(result.succeeded||cancel_.load())return result;
    }
    auto webCookie=cookie();if(webCookie.empty())webCookie=readCookie();
    if(!webCookie.empty()&&webCookie!=rejectedCookie_&&!cancel_.load()){
        auto web=cookieBrowse(webCookie,path,false);
        if(web.succeeded&&!cancel_.load()){
            SDL_LockMutex(mutex_);cookie_=webCookie;SDL_UnlockMutex(mutex_);
            authenticated_.store(true);
            web.message=std::string(label)+" · "+std::to_string(web.items.size())+" 个视频";
            return web;
        }
        if(web.error.find("Cookie 未被 YouTube 识别")!=std::string::npos
            ||web.error.find("重新导入网页 Cookie")!=std::string::npos)
            rejectedCookie_=webCookie;
        if(hasDeviceLogin)return result;
        return web;
    }
    if(hasDeviceLogin&&!rejectedCookie_.empty()&&result.error.empty())result.error="账号数据不可用";
    else if(!hasDeviceLogin&&!rejectedCookie_.empty())result.error="网页 Cookie 已失效，请重新导入";
    if(result.error.empty())result.error="请先登录 YouTube 账号";
    return result;
}
YouTubeResult YouTubeProvider::accountBrowseNow(const char* browseId,const char* label){
    YouTubeResult result;
    const bool hasTvSession=!tvRefreshToken_.empty()||!tvToken_.empty();
    if(!hasTvSession&&accountBrowseScopeDenied_){result.error="当前授权范围不支持电视端推荐和历史";return result;}
    const auto token=hasTvSession?tvBrowseToken():accessToken();
    if(token.empty()){result.error="配对登录已失效";return result;}
    auto* body=json_pack("{s:{s:{s:s,s:s,s:s,s:s,s:s},s:{s:b,s:b}},s:{s:s,s:b},s:b,s:b,s:s}",
        "context","client","clientName","TVHTML5","clientVersion",tvVersion,"clientScreen","WATCH",
        "userAgent",tvAgent,"hl","zh-CN","user","enableSafetyMode",0,"lockedSafetyMode",0,
        "tvAppInfo","appQuality","TV_APP_QUALITY_FULL_ANIMATION","zylonLeftNav",1,
        "webpSupport",0,"animatedWebpSupport",1,"browseId",browseId);
    if(!body){result.error="无法建立账号浏览请求";return result;}
    char* raw=json_dumps(body,JSON_COMPACT);json_decref(body);
    if(!raw){result.error="无法编码账号浏览请求";return result;}
    net::HttpRequest req;req.url="https://www.youtube.com/youtubei/v1/browse?prettyPrint=false";
    req.method="POST";req.body=raw;free(raw);req.cancel=&cancel_;req.proxyUrl=proxy();req.maxResponseBytes=6*1024*1024;
    req.connectTimeoutSeconds=6;req.timeoutSeconds=12;
    req.headers={{"Content-Type","application/json"},{"Authorization","Bearer "+token},
        {"User-Agent",tvAgent},{"Referer","https://www.youtube.com/tv"},
        {"X-Youtube-Client-Name","7"},{"X-Youtube-Client-Version",tvVersion}};
    auto response=http_.get(req);if(!response.succeeded){
        result.error=responseError(response);
        if(!hasTvSession&&response.statusCode==403&&result.error.find("insufficient authentication scopes")!=std::string::npos){
            accountBrowseScopeDenied_=true;
            if(!cancel_.load()&&!refreshToken_.empty())saveSession(refreshToken_,true);
        }
        return result;
    }
    auto* root=parse(response.body);
    auto* tracking=json_object_get(json_object_get(root,"responseContext"),"serviceTrackingParams");
    bool verified=false,anonymous=false;
    if(json_is_array(tracking))for(size_t i=0;i<json_array_size(tracking);++i){
        auto* params=json_object_get(json_array_get(tracking,i),"params");
        if(json_is_array(params))for(size_t j=0;j<json_array_size(params);++j){
            auto* param=json_array_get(params,j);
            if(field(param,"key")=="logged_in"){
                if(field(param,"value")=="1")verified=true;
                if(field(param,"value")=="0")anonymous=true;
            }
        }
    }
    if(!anonymous)gatherVideos(root,result.items,0);
    if(root)json_decref(root);
    if(anonymous){result.error="电视端接口返回游客内容，请重新配对登录";return result;}
    if(!verified){result.error="电视端接口未确认账号身份";return result;}
    if(result.items.empty()){result.error="接口没有返回可显示的视频";return result;}
    result.succeeded=true;result.message=std::string(label)+" · "+std::to_string(result.items.size())+" 个视频";
    return result;
}
bool YouTubeProvider::loadTvProfileNow(const std::string& token,const std::string& proxyUrl){
    if(token.empty()||profileCancel_.load())return false;
    // Match the TV browse context that already succeeds for recommendations.
    auto* body=json_pack("{s:{s:{s:s,s:s,s:s,s:s,s:s},s:{s:b,s:b}},s:{s:s,s:b},s:b,s:b}",
        "context","client","clientName","TVHTML5","clientVersion",tvVersion,
        "clientScreen","WATCH","userAgent",tvAgent,"hl","zh-CN",
        "user","enableSafetyMode",0,"lockedSafetyMode",0,
        "tvAppInfo","appQuality","TV_APP_QUALITY_FULL_ANIMATION","zylonLeftNav",1,
        "webpSupport",0,"animatedWebpSupport",1);
    if(!body)return false;
    auto* mask=json_pack("{s:b,s:b,s:b}","returnOwner",1,
        "returnBrandAccounts",1,"returnPersonaAccounts",0);
    if(!mask){json_decref(body);return false;}
    json_object_set_new(body,"accountReadMask",mask);
    char* raw=json_dumps(body,JSON_COMPACT);json_decref(body);if(!raw)return false;
    net::HttpRequest req;req.url="https://www.youtube.com/youtubei/v1/account/accounts_list";
    req.method="POST";req.body=raw;free(raw);req.cancel=&profileCancel_;
    req.proxyUrl=proxyUrl;req.maxResponseBytes=512*1024;
    req.connectTimeoutSeconds=6;req.timeoutSeconds=12;
    req.headers={{"Content-Type","application/json"},{"Authorization","Bearer "+token},
        {"User-Agent",tvAgent},{"Referer","https://www.youtube.com/tv"},
        {"X-Youtube-Client-Name","7"},{"X-Youtube-Client-Version",tvVersion}};
    const auto response=http_.get(req);
    if(!response.succeeded||profileCancel_.load()){
        if(!profileCancel_.load()){SDL_LockMutex(mutex_);profileStatus_=response.statusCode>0
            ?"电视端头像 HTTP "+std::to_string(response.statusCode):"电视端头像网络请求失败";SDL_UnlockMutex(mutex_);}
        return false;
    }
    auto* root=parse(response.body);std::string selectedAvatar,firstAvatar;
    std::function<void(json_t*,int)> scan=[&](json_t* node,int depth){
        if(!node||depth>24)return;
        if(json_is_array(node)){
            for(size_t i=0;i<json_array_size(node);++i)scan(json_array_get(node,i),depth+1);
            return;
        }
        if(!json_is_object(node))return;
        const auto avatar=thumbnail(json_object_get(json_object_get(node,"accountPhoto"),"thumbnails"));
        if(avatar.rfind("https://",0)==0){
            if(firstAvatar.empty())firstAvatar=avatar;
            if(json_is_true(json_object_get(node,"isSelected")))selectedAvatar=avatar;
        }
        const char* key;json_t* value;
        json_object_foreach(node,key,value)scan(value,depth+1);
    };
    scan(root,0);
    if(root)json_decref(root);
    const auto avatar=selectedAvatar.empty()?firstAvatar:selectedAvatar;
    if(avatar.empty()){
        SDL_LockMutex(mutex_);profileStatus_="账号列表未返回头像";SDL_UnlockMutex(mutex_);
        return false;
    }
    SDL_LockMutex(mutex_);
    const bool accepted=!stopping_&&!profileCancel_.load()&&authenticated_.load();
    if(accepted){profileImageUrl_=avatar;profileStatus_="头像地址已获取";saveProfileUrl(avatar);}
    SDL_UnlockMutex(mutex_);
    return accepted;
}
void YouTubeProvider::loadProfileNow(const std::string& token,const std::string& proxyUrl){
    if(token.empty()||profileCancel_.load())return;
    net::HttpRequest req;req.url="https://www.googleapis.com/youtube/v3/channels?part=snippet&mine=true&maxResults=1";
    req.headers={{"Authorization","Bearer "+token}};req.cancel=&profileCancel_;req.proxyUrl=proxyUrl;
    req.maxResponseBytes=256*1024;req.timeoutSeconds=12;
    const auto response=http_.get(req);if(!response.succeeded||profileCancel_.load())return;
    auto* root=parse(response.body);auto* channel=json_array_get(json_object_get(root,"items"),0);
    const auto avatar=apiThumbnail(json_object_get(json_object_get(channel,"snippet"),"thumbnails"));
    if(root)json_decref(root);
    if(!avatar.empty()&&avatar.rfind("https://",0)==0){
        SDL_LockMutex(mutex_);
        if(!stopping_&&!profileCancel_.load()&&authenticated_.load()){
            profileImageUrl_=avatar;
            saveProfileUrl(avatar);
        }
        SDL_UnlockMutex(mutex_);
    }
}
YouTubeResult YouTubeProvider::subscriptionFeedNow(){
    YouTubeResult result;
    const auto token=accessToken();
    if(token.empty()){result.error="YouTube 登录已过期，请在设置中重新配对";return result;}
    auto request=[&](const std::string& url,size_t limit){
        net::HttpRequest req;req.url=url;req.headers={{"Authorization","Bearer "+token}};
        req.cancel=&cancel_;req.proxyUrl=proxy();req.maxResponseBytes=limit;
        return http_.get(req);
    };
    auto subscriptions=request("https://www.googleapis.com/youtube/v3/subscriptions?part=snippet&mine=true&maxResults=20&order=relevance",1024*1024);
    if(!subscriptions.succeeded){result.error=responseError(subscriptions);return result;}
    auto* root=parse(subscriptions.body);auto* list=json_object_get(root,"items");
    std::vector<std::string> channels;
    if(json_is_array(list))for(size_t i=0;i<json_array_size(list)&&channels.size()<12;++i){
        auto* snippet=json_object_get(json_array_get(list,i),"snippet");
        auto id=field(json_object_get(snippet,"resourceId"),"channelId");
        if(!id.empty())channels.push_back(std::move(id));
    }
    if(root)json_decref(root);
    if(channels.empty()){result.succeeded=true;result.message="暂无订阅频道 · 可用 X 查看订阅";return result;}
    std::string ids;
    for(const auto& id:channels){if(!ids.empty())ids+=",";ids+=id;}
    auto channelResponse=request("https://www.googleapis.com/youtube/v3/channels?part=contentDetails&id="+encode(ids),1024*1024);
    if(!channelResponse.succeeded){result.error=responseError(channelResponse);return result;}
    root=parse(channelResponse.body);list=json_object_get(root,"items");
    std::vector<std::string> playlists;
    if(json_is_array(list))for(size_t i=0;i<json_array_size(list);++i){
        auto* details=json_object_get(json_array_get(list,i),"contentDetails");
        auto id=field(json_object_get(details,"relatedPlaylists"),"uploads");
        if(!id.empty())playlists.push_back(std::move(id));
    }
    if(root)json_decref(root);
    struct DatedItem{YouTubeItem item;std::string published;};
    std::vector<DatedItem> dated;
    std::vector<std::vector<DatedItem>> byPlaylist(playlists.size());
    std::atomic<size_t> nextPlaylist{0};
    auto fetchPlaylists=[&](){
        for(;;){
            const size_t index=nextPlaylist.fetch_add(1);
            if(index>=playlists.size()||cancel_.load())break;
            auto videos=request("https://www.googleapis.com/youtube/v3/playlistItems?part=snippet&maxResults=4&playlistId="+encode(playlists[index]),768*1024);
            if(!videos.succeeded)continue;
            auto* responseRoot=parse(videos.body);auto* entries=json_object_get(responseRoot,"items");
            if(json_is_array(entries))for(size_t i=0;i<json_array_size(entries);++i){
                auto* snippet=json_object_get(json_array_get(entries,i),"snippet");
                DatedItem entry;entry.item.id=field(json_object_get(snippet,"resourceId"),"videoId");
                entry.item.title=field(snippet,"title");entry.item.channel=field(snippet,"channelTitle");
                entry.item.thumbnail=apiThumbnail(json_object_get(snippet,"thumbnails"));
                entry.published=field(snippet,"publishedAt");
                if(!entry.item.id.empty()&&entry.item.title!="Private video"&&entry.item.title!="Deleted video")byPlaylist[index].push_back(std::move(entry));
            }
            if(responseRoot)json_decref(responseRoot);
        }
    };
    using FetchPlaylists=decltype(fetchPlaylists);
    SDL_Thread* workers[2]{};
    for(auto& worker:workers)worker=SDL_CreateThread(+[](void* task)->int{(*static_cast<FetchPlaylists*>(task))();return 0;},"youtube-feed",&fetchPlaylists);
    fetchPlaylists();
    for(auto* worker:workers)if(worker)SDL_WaitThread(worker,nullptr);
    if(cancel_.load())return {};
    for(auto& group:byPlaylist)for(auto& entry:group)dated.push_back(std::move(entry));
    std::stable_sort(dated.begin(),dated.end(),[](const auto& a,const auto& b){return a.published>b.published;});
    for(auto& entry:dated){
        if(std::none_of(result.items.begin(),result.items.end(),[&](const auto& item){return item.id==entry.item.id;}))
            result.items.push_back(std::move(entry.item));
        if(result.items.size()>=60)break;
    }
    result.succeeded=true;
    result.message="订阅更新 · "+std::to_string(result.items.size())+" 个视频（按发布时间）";
    return result;
}
YouTubeResult YouTubeProvider::searchNow(const std::string& query){
    YouTubeResult result;auto* body=json_pack("{s:{s:{s:s,s:s}},s:s}","context","client","clientName","ANDROID","clientVersion","20.10.38","query",query.c_str());
    if(!body){result.error="无法建立 YouTube 搜索请求";return result;}
    char* raw=json_dumps(body,JSON_COMPACT);json_decref(body);if(!raw){result.error="无法编码 YouTube 搜索请求";return result;}
    net::HttpRequest req;req.url="https://www.youtube.com/youtubei/v1/search?prettyPrint=false";req.method="POST";req.body=raw;free(raw);
    req.cancel=&cancel_;req.proxyUrl=proxy();req.maxResponseBytes=4*1024*1024;
    req.connectTimeoutSeconds=6;req.timeoutSeconds=12;
    req.headers={{"Content-Type","application/json"},{"User-Agent",androidAgent},{"X-Youtube-Client-Name","3"},{"X-Youtube-Client-Version","20.10.38"}};
    const auto response=http_.get(req);if(!response.succeeded){result.error=responseError(response);return result;}
    auto* root=parse(response.body);if(root)gatherVideos(root,result.items,0);if(root)json_decref(root);
    result.succeeded=true;result.message=result.items.empty()?"没有找到视频":"找到 "+std::to_string(result.items.size())+" 个视频";return result;
}
YouTubeResult YouTubeProvider::durationsNow(const std::string& ids){
    YouTubeResult result;result.durationOnly=true;
    const auto token=accessToken();if(token.empty())return result;
    net::HttpRequest request;
    request.url="https://www.googleapis.com/youtube/v3/videos?part=contentDetails&id="+encode(ids);
    request.headers={{"Authorization","Bearer "+token}};
    request.proxyUrl=proxy();request.cancel=&cancel_;request.maxResponseBytes=512*1024;
    request.connectTimeoutSeconds=5;request.timeoutSeconds=9;
    const auto response=http_.get(request);if(!response.succeeded)return result;
    auto* root=parse(response.body);auto* items=json_object_get(root,"items");
    if(json_is_array(items))for(size_t i=0;i<json_array_size(items);++i){
        auto* value=json_array_get(items,i);YouTubeItem item;
        item.id=field(value,"id");
        item.duration=isoDuration(field(json_object_get(value,"contentDetails"),"duration"));
        if(!item.id.empty()&&!item.duration.empty())result.items.push_back(std::move(item));
    }
    if(root)json_decref(root);
    result.succeeded=true;return result;
}
bool YouTubeProvider::refreshToken(){
    if(clientId_.empty()||refreshToken_.empty())return false;
    token_.clear();
    auto req=form("https://oauth2.googleapis.com/token","client_id="+encode(clientId_)+"&client_secret="+encode(clientSecret_)+"&refresh_token="+encode(refreshToken_)+"&grant_type=refresh_token",&cancel_);
    req.proxyUrl=proxy();
    const auto response=http_.get(req);if(!response.succeeded)return false;
    auto* root=parse(response.body);token_=field(root,"access_token");
    tokenExpires_=std::time(nullptr)+(json_is_integer(json_object_get(root,"expires_in"))?json_integer_value(json_object_get(root,"expires_in")):3600)-60;
    if(root)json_decref(root);
    return !token_.empty();
}
std::string YouTubeProvider::accessToken(){if(token_.empty()||std::time(nullptr)>=tokenExpires_)refreshToken();return token_;}
std::string YouTubeProvider::tvBrowseToken(){
    if(!tvToken_.empty()&&std::time(nullptr)<tvTokenExpires_)return tvToken_;
    if(tvRefreshToken_.empty()||clientId_.empty()||clientSecret_.empty())return {};
    tvToken_.clear();
    auto* body=json_pack("{s:s,s:s,s:s,s:s}","refresh_token",tvRefreshToken_.c_str(),
        "client_id",clientId_.c_str(),"client_secret",clientSecret_.c_str(),"grant_type","refresh_token");
    if(!body)return {};
    char* raw=json_dumps(body,JSON_COMPACT);json_decref(body);if(!raw)return {};
    net::HttpRequest req;req.url="https://www.youtube.com/o/oauth2/token";req.method="POST";
    req.body=raw;free(raw);req.proxyUrl=proxy();req.cancel=&cancel_;
    req.headers={{"Content-Type","application/json"}};req.maxResponseBytes=256*1024;
    const auto response=http_.get(req);
    if(!response.succeeded)return {};
    auto* root=parse(response.body);tvToken_=field(root,"access_token");
    const auto duration=json_integer_value(json_object_get(root,"expires_in"));
    tvTokenExpires_=std::time(nullptr)+std::max<json_int_t>(60,duration)-60;
    if(root)json_decref(root);
    return tvToken_;
}
YouTubeResult YouTubeProvider::subscriptionsNow(int maxPages){
    const auto webCookie=cookie();if(clientId_.empty()&&!webCookie.empty())return cookieBrowse(webCookie,"/feed/channels",true);
    YouTubeResult result;const auto token=accessToken();if(token.empty()){result.error="请在 − 设置中使用配对码登录 YouTube";return result;}
    std::string next;
    for(int page=0;page<maxPages&&!cancel_.load();++page){
        net::HttpRequest req;req.url="https://www.googleapis.com/youtube/v3/subscriptions?part=snippet&mine=true&maxResults=50"+(next.empty()?std::string():"&pageToken="+encode(next));
        req.headers={{"Authorization","Bearer "+token}};req.cancel=&cancel_;req.proxyUrl=proxy();req.maxResponseBytes=2*1024*1024;
        auto response=http_.get(req);if(!response.succeeded){result.error=responseError(response);return result;}
        auto* root=parse(response.body);auto* list=json_object_get(root,"items");
        if(json_is_array(list))for(size_t i=0;i<json_array_size(list);++i){auto* snippet=json_object_get(json_array_get(list,i),"snippet");auto* resource=json_object_get(snippet,"resourceId");
            YouTubeItem item;item.id=field(resource,"channelId");item.title=field(snippet,"title");item.thumbnail=apiThumbnail(json_object_get(snippet,"thumbnails"));item.isChannel=true;
            if(!item.id.empty())result.items.push_back(std::move(item));}
        next=field(root,"nextPageToken");if(root)json_decref(root);if(next.empty())break;
    }
    result.succeeded=true;result.message="订阅频道 "+std::to_string(result.items.size())+" 个";return result;
}
YouTubeResult YouTubeProvider::channelVideosNow(const std::string& channelId){
    const auto webCookie=cookie();if(!webCookie.empty())return cookieBrowse(webCookie,"/channel/"+encode(channelId)+"/videos",false);
    YouTubeResult result;const auto token=accessToken();if(token.empty()){result.error="YouTube 登录已过期，请重新配对";return result;}
    net::HttpRequest channel;channel.url="https://www.googleapis.com/youtube/v3/channels?part=contentDetails&id="+encode(channelId);channel.headers={{"Authorization","Bearer "+token}};channel.cancel=&cancel_;channel.proxyUrl=proxy();channel.maxResponseBytes=256*1024;
    auto response=http_.get(channel);if(!response.succeeded){result.error=responseError(response);return result;}
    auto* root=parse(response.body);auto* first=json_array_get(json_object_get(root,"items"),0);const auto playlist=field(json_object_get(json_object_get(first,"contentDetails"),"relatedPlaylists"),"uploads");if(root)json_decref(root);
    if(playlist.empty()){result.error="频道没有可读取的上传列表";return result;}
    net::HttpRequest request;request.url="https://www.googleapis.com/youtube/v3/playlistItems?part=snippet&maxResults=50&playlistId="+encode(playlist);request.headers={{"Authorization","Bearer "+token}};request.cancel=&cancel_;request.proxyUrl=proxy();request.maxResponseBytes=2*1024*1024;
    response=http_.get(request);if(!response.succeeded){result.error=responseError(response);return result;}
    root=parse(response.body);auto* list=json_object_get(root,"items");
    if(json_is_array(list))for(size_t i=0;i<json_array_size(list);++i){auto* snippet=json_object_get(json_array_get(list,i),"snippet");auto* resource=json_object_get(snippet,"resourceId");YouTubeItem item;
        item.id=field(resource,"videoId");item.title=field(snippet,"title");item.channel=field(snippet,"channelTitle");item.thumbnail=apiThumbnail(json_object_get(snippet,"thumbnails"));if(!item.id.empty())result.items.push_back(std::move(item));}
    if(root)json_decref(root);
    result.succeeded=true;result.message="频道视频 "+std::to_string(result.items.size())+" 个";return result;
}
YouTubeResult YouTubeProvider::playbackNow(const std::string& videoId){
    YouTubeResult result;auto* body=json_pack("{s:{s:{s:s,s:s}},s:s,s:b}","context","client","clientName","ANDROID","clientVersion","20.10.38","videoId",videoId.c_str(),"contentCheckOk",1);
    if(!body){result.error="无法建立播放请求";return result;}char* raw=json_dumps(body,JSON_COMPACT);json_decref(body);if(!raw){result.error="无法编码播放请求";return result;}
    net::HttpRequest req;req.url="https://www.youtube.com/youtubei/v1/player?prettyPrint=false";req.method="POST";req.body=raw;free(raw);req.cancel=&cancel_;req.proxyUrl=proxy();req.maxResponseBytes=2*1024*1024;
    req.headers={{"Content-Type","application/json"},{"User-Agent",androidAgent},{"X-Youtube-Client-Name","3"},{"X-Youtube-Client-Version","20.10.38"}};
    const auto response=http_.get(req);if(!response.succeeded){result.error=responseError(response);return result;}
    auto* root=parse(response.body);auto* streaming=json_object_get(root,"streamingData");
    auto* formats=json_object_get(streaming,"formats");
    auto* adaptive=json_object_get(streaming,"adaptiveFormats");
    int progressiveHeight=0;
    if(json_is_array(formats))for(size_t i=0;i<json_array_size(formats);++i){
        auto* format=json_array_get(formats,i);
        const std::string mime=field(format,"mimeType"),url=field(format,"url");
        int height=(int)json_integer_value(json_object_get(format,"height"));
        const int itag=(int)json_integer_value(json_object_get(format,"itag"));
        if(height==0&&itag==18)height=360;
        if(height==0&&itag==22)height=720;
        if(!url.empty()&&height>progressiveHeight&&height<=maxHeight_
            &&mime.find("video/mp4")!=std::string::npos&&mime.find("avc1")!=std::string::npos){
            result.stream.url=url;progressiveHeight=height;
        }
    }
    // Higher resolutions are delivered as separate video and audio MP4 files.
    int videoHeight=0;std::string videoUrl,audioUrl;long long audioBitrate=0;
    if(json_is_array(adaptive))for(size_t i=0;i<json_array_size(adaptive);++i){
        auto* format=json_array_get(adaptive,i);
        const std::string mime=field(format,"mimeType"),url=field(format,"url");
        if(url.empty())continue;
        const int height=(int)json_integer_value(json_object_get(format,"height"));
        if(mime.find("video/mp4")!=std::string::npos&&mime.find("avc1")!=std::string::npos
            &&height>videoHeight&&height<=maxHeight_){videoHeight=height;videoUrl=url;}
        if(mime.find("audio/mp4")!=std::string::npos&&mime.find("mp4a")!=std::string::npos){
            const long long bitrate=json_integer_value(json_object_get(format,"bitrate"));
            if(bitrate>audioBitrate){audioBitrate=bitrate;audioUrl=url;}
        }
    }
    int selectedHeight=progressiveHeight;
    if(!videoUrl.empty()&&!audioUrl.empty()&&videoHeight>progressiveHeight){
        if(!result.stream.url.empty()){
            result.fallbackStream.url=result.stream.url;
            result.fallbackHeight=progressiveHeight;
        }
        result.stream.url=videoUrl;result.stream.audioUrl=audioUrl;selectedHeight=videoHeight;
    }
    if(result.stream.url.empty()){auto* status=json_object_get(root,"playabilityStatus");result.error=field(status,"reason");if(result.error.empty())result.error="当前视频没有可播放的 H.264/AAC 视频流";}
    else{result.videoHeight=selectedHeight;result.stream.proxyUrl=proxy();
        // Separate video and audio each open range requests; keep the total
        // concurrency within the bandwidth of a typical Switch proxy node.
        result.stream.parallelFetches=2;
        result.stream.headers.push_back({"User-Agent",androidAgent});
        if(!result.fallbackStream.url.empty()){
            result.fallbackStream.proxyUrl=result.stream.proxyUrl;
            result.fallbackStream.parallelFetches=2;
            result.fallbackStream.headers=result.stream.headers;
        }
        result.succeeded=true;result.message="正在播放 YouTube · "+std::to_string(selectedHeight)+"p";}
    if(root)json_decref(root);
    return result;
}
YouTubeResult YouTubeProvider::tvLoginProbeNow(){
    YouTubeResult result;
    if(clientId_.empty()||clientSecret_.empty()){
        result.error="请先配置电视设备客户端 ID 和密钥";
        setLoginState({false,false,"","",result.error});return result;
    }
    auto send=[&](const std::string& url,json_t* body){
        net::HttpRequest req;req.url=url;req.method="POST";req.proxyUrl=proxy();req.cancel=&cancel_;
        req.headers={{"Content-Type","application/json"}};req.maxResponseBytes=2*1024*1024;
        char* raw=json_dumps(body,JSON_COMPACT);if(raw){req.body=raw;free(raw);}
        return http_.get(req);
    };
    auto* body=json_pack("{s:s,s:s,s:s,s:s}","client_id",clientId_.c_str(),
        "device_id","okswitch-tv","model_name","ytlr::","scope","http://gdata.youtube.com");
    auto response=send("https://www.youtube.com/o/oauth2/device/code",body);json_decref(body);
    auto* root=parse(response.body);const auto code=field(root,"device_code"),userCode=field(root,"user_code");
    std::string url=field(root,"verification_url");if(url.empty())url="https://www.youtube.com/activate";
    int interval=(int)json_integer_value(json_object_get(root,"interval"));
    int expires=(int)json_integer_value(json_object_get(root,"expires_in"));
    const auto firstError=field(root,"error");if(root)json_decref(root);
    if(code.empty()||userCode.empty()){
        result.error=firstError.empty()?responseError(response):firstError;
        setLoginState({false,false,"","",result.error});return result;
    }
    interval=std::max(5,interval);const auto deadline=SDL_GetTicks()+(Uint32)std::max(60,expires)*1000;
    setLoginState({true,false,url,userCode,"电视端实验 · 请在手机上输入配对码"});
    std::string access,refresh;
    int tokenDuration=0;
    while(!cancel_.load()&&!SDL_TICKS_PASSED(SDL_GetTicks(),deadline)){
        for(int t=0;t<interval*10&&!cancel_.load();++t)SDL_Delay(100);
        if(cancel_.load())break;
        body=json_pack("{s:s,s:s,s:s,s:s}","code",code.c_str(),"client_id",clientId_.c_str(),
            "client_secret",clientSecret_.c_str(),"grant_type","http://oauth.net/grant_type/device/1.0");
        response=send("https://www.youtube.com/o/oauth2/token",body);json_decref(body);
        root=parse(response.body);access=field(root,"access_token");refresh=field(root,"refresh_token");
        tokenDuration=(int)json_integer_value(json_object_get(root,"expires_in"));
        const auto error=field(root,"error");
        if(root)json_decref(root);
        if(!access.empty())break;
        if(error=="slow_down"){interval+=5;continue;}
        if(error=="authorization_pending")continue;
        result.error=error.empty()?responseError(response):error;break;
    }
    if(access.empty()){
        if(result.error.empty())result.error=cancel_.load()?"已取消电视端实验":"电视端配对已超时";
        setLoginState({false,false,"","",result.error});return result;
    }
    int successes=0;
    std::string details;
    for(const char* browseId:{"default","FEhistory"}){
        body=json_pack("{s:{s:{s:s,s:s}},s:s}","context","client","clientName","TVHTML5",
            "clientVersion",tvVersion,"browseId",browseId);
        net::HttpRequest req;req.url="https://www.youtube.com/youtubei/v1/browse";req.method="POST";
        char* raw=json_dumps(body,JSON_COMPACT);json_decref(body);if(raw){req.body=raw;free(raw);}
        req.proxyUrl=proxy();req.cancel=&cancel_;req.maxResponseBytes=6*1024*1024;
        req.headers={{"Content-Type","application/json"},{"Authorization","Bearer "+access},
            {"User-Agent",tvAgent},{"Referer","https://www.youtube.com/tv"},
            {"X-Youtube-Client-Name","7"},{"X-Youtube-Client-Version",tvVersion}};
        const auto browse=http_.get(req);
        if(browse.succeeded){auto* data=parse(browse.body);std::vector<YouTubeItem> items;
            bool verified=false,anonymous=false;
            auto* tracking=json_object_get(json_object_get(data,"responseContext"),"serviceTrackingParams");
            if(json_is_array(tracking))for(size_t i=0;i<json_array_size(tracking);++i){
                auto* params=json_object_get(json_array_get(tracking,i),"params");
                if(json_is_array(params))for(size_t j=0;j<json_array_size(params);++j){
                    auto* param=json_array_get(params,j);
                    if(field(param,"key")=="logged_in"){
                        if(field(param,"value")=="1")verified=true;
                        if(field(param,"value")=="0")anonymous=true;
                    }
                }
            }
            if(verified&&!anonymous)gatherVideos(data,items,0);
            if(data)json_decref(data);
            if(!items.empty())++successes;
            details+=std::string(browseId)+": "+(anonymous?"游客":!verified?"身份未确认":items.empty()?"无视频":"可读取")+"; ";
        }else details+=std::string(browseId)+": HTTP "+std::to_string(browse.statusCode)+"; ";
    }
    if(successes>0&&cancel_.load()){result.error="已取消电视端实验";setLoginState({false,false,"","",result.error});return result;}
    if(successes>0){
        tvToken_=access;tvTokenExpires_=std::time(nullptr)+std::max(60,tokenDuration)-60;
        if(!refresh.empty()&&saveTvSession(refresh)){
            tvRefreshToken_=refresh;authenticated_.store(true);clearListCache();
            details+="电视端登录已保存";
        }else details+="电视端数据可读，但令牌未保存";
    }
    result.succeeded=true;
    result.message=successes==2?"电视端实验：推荐和历史均可读取":
        successes==1?"电视端实验：仅一个页面返回账号视频":"电视端实验：配对成功，账号推荐和历史未返回视频";
    result.message+=" · "+details;
    setLoginState({false,successes>0,"","",result.message});return result;
}
YouTubeResult YouTubeProvider::loginNow(){
    const auto webCookie=readCookie();
    if(clientId_.empty()&&!webCookie.empty()){
        setLoginState({true,false,"","","正在验证网页 Cookie..."});
        auto result=cookieBrowse(webCookie,"/feed/channels",true);
        if(result.succeeded&&!cancel_.load()){
            clearListCache();
            SDL_LockMutex(mutex_);cookie_=webCookie;SDL_UnlockMutex(mutex_);
            authenticated_.store(true);result.message="网页 Cookie 登录成功 · 订阅频道 "+std::to_string(result.items.size())+" 个";
            setLoginState({false,true,"","",result.message});
        }else{result.succeeded=false;if(result.error.empty())result.error="已取消登录";setLoginState({false,false,"","",result.error});}
        return result;
    }
    YouTubeResult result;if(clientId_.empty()){result.error="请先在 SD 卡配置 YouTube TV 客户端 ID";setLoginState({false,false,"","",result.error});return result;}
    auto req=form("https://oauth2.googleapis.com/device/code","client_id="+encode(clientId_)+"&scope="+encode("https://www.googleapis.com/auth/youtube.readonly"),&cancel_);
    req.proxyUrl=proxy();
    auto response=http_.get(req);if(!response.succeeded){result.error=responseError(response);setLoginState({false,false,"","",result.error});return result;}
    auto* root=parse(response.body);const auto code=field(root,"device_code"),userCode=field(root,"user_code"),url=field(root,"verification_url");
    int interval=(int)json_integer_value(json_object_get(root,"interval"));int expires=(int)json_integer_value(json_object_get(root,"expires_in"));if(root)json_decref(root);
    if(code.empty()||userCode.empty()||url.empty()){result.error="配对码响应不完整";setLoginState({false,false,"","",result.error});return result;}
    interval=std::max(5,interval);const auto deadline=SDL_GetTicks()+(Uint32)std::max(60,expires)*1000;
    setLoginState({true,false,url,userCode,"请用手机打开地址并输入配对码"});
    while(!cancel_.load()&&!SDL_TICKS_PASSED(SDL_GetTicks(),deadline)){
        for(int t=0;t<interval*10&&!cancel_.load();++t)SDL_Delay(100);
        if(cancel_.load())break;
        req=form("https://oauth2.googleapis.com/token","client_id="+encode(clientId_)+"&client_secret="+encode(clientSecret_)+"&device_code="+encode(code)+"&grant_type="+encode("urn:ietf:params:oauth:grant-type:device_code"),&cancel_);
        req.proxyUrl=proxy();
        response=http_.get(req);root=parse(response.body);
        const auto access=field(root,"access_token"),refresh=field(root,"refresh_token"),error=field(root,"error");
        const int duration=(int)json_integer_value(json_object_get(root,"expires_in"));if(root)json_decref(root);
        if(!access.empty()&&!refresh.empty()){
            token_=access;refreshToken_=refresh;tokenExpires_=std::time(nullptr)+std::max(60,duration)-60;
            if(!saveSession(refresh)){result.error="登录成功，但无法保存到 SD 卡";setLoginState({false,false,url,userCode,result.error});return result;}
            clearListCache();
            authenticated_.store(true);
            accountBrowseScopeDenied_=false;
            result.succeeded=true;result.message="YouTube 配对登录成功";setLoginState({false,true,"","",result.message});return result;
        }
        if(error=="slow_down"){interval+=5;continue;}
        if(error=="authorization_pending")continue;
        if(!error.empty()){result.error=error;break;}
        if(!response.succeeded){result.error=responseError(response);break;}
    }
    if(result.error.empty())result.error=cancel_.load()?"已取消配对":"配对码已过期";
    setLoginState({false,false,url,userCode,result.error});return result;
}
YouTubeResult YouTubeProvider::cookieBrowse(const std::string& webCookie,const std::string& path,bool channels){
    YouTubeResult result;net::HttpRequest request;
    request.url="https://www.youtube.com"+path;request.proxyUrl=proxy();request.cancel=&cancel_;
    request.followRedirects=false;
    request.maxResponseBytes=12*1024*1024;request.connectTimeoutSeconds=6;request.timeoutSeconds=15;
    request.headers={{"Cookie",webCookie},{"User-Agent","Mozilla/5.0"},{"Accept-Language","zh-CN,zh;q=0.9"}};
    net::HttpResponse response;
    for(int redirects=0;redirects<4;++redirects){
        response=http_.get(request);
        if(response.statusCode<300||response.statusCode>=400)break;
        const auto location=redirectLocation(response);
        const auto next=youtubeRedirect(location);
        if(next.empty()){
            result.error=location.find("consent.youtube.com")!=std::string::npos
                ?"YouTube 要求先在浏览器完成同意确认，再重新导入 Cookie"
                :location.find("accounts.google.com")!=std::string::npos
                ?"YouTube 跳转到 Google 登录页，请重新导入网页 Cookie"
                :"YouTube 网页跳转到其他站点，已停止发送 Cookie（HTTP "+std::to_string(response.statusCode)+"）";
            return result;
        }
        if(redirects==3){result.error="YouTube 网页跳转次数过多，请检查 Cookie 或网络代理";return result;}
        request.url=next;
    }
    if(!response.succeeded){result.error="读取 YouTube 网页失败："+response.error;return result;}
    // Credentials are sent only to the fixed YouTube origin; never follow a redirect with them.
    const bool logged=response.body.find("\"LOGGED_IN\":true")!=std::string::npos||response.body.find("\"LOGGED_IN\": true")!=std::string::npos;
    if(!logged){result.error="网页 Cookie 未被 YouTube 识别；请在浏览器确认账号可正常访问，再重新导入";return result;}
    size_t start=std::string::npos;
    for(const char* marker:{"var ytInitialData = ","window[\"ytInitialData\"] = "}){
        const auto at=response.body.find(marker);if(at!=std::string::npos){start=at+std::char_traits<char>::length(marker);break;}
    }
    json_error_t error{};json_t* root=start==std::string::npos?nullptr:json_loadb(response.body.data()+start,response.body.size()-start,JSON_DISABLE_EOF_CHECK,&error);
    if(!root){result.error="YouTube 网页数据格式暂不支持";return result;}
    if(channels)gatherChannels(root,result.items,0);else gatherVideos(root,result.items,0);
    json_decref(root);result.succeeded=true;
    result.message=std::string(channels?"网页订阅频道 ":"频道视频 ")+std::to_string(result.items.size())+" 个";
    return result;
}
}
