#include "okswitch/cloud/BaiduLogin.hpp"
#include <png.h>
#include <jansson.h>
#include <switch.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <chrono>
#include <ctime>
#include <map>
#include <sys/stat.h>

namespace okswitch::cloud {
namespace {
constexpr const char* sessionPath="sdmc:/switch/OKSwitch/baidu-session-v2.json";
constexpr const char* legacySessionPath="sdmc:/switch/OKSwitch/baidu-session.json";
constexpr const char* userAgent="com.android.chrome/131.0.6778.200 (Linux;Android 10) AndroidXMedia3/1.5.1";
using Cookies=std::map<std::string,std::string>;

std::string encode(const std::string& value){
    static const char* hex="0123456789ABCDEF";std::string result;
    for(unsigned char c:value){if(std::isalnum(c)||c=='-'||c=='_'||c=='.'||c=='~')result+=c;else{result+='%';result+=hex[c>>4];result+=hex[c&15];}}
    return result;
}
json_t* jsonBody(const net::HttpResponse& response){json_error_t error{};return response.body.empty()?nullptr:json_loadb(response.body.data(),response.body.size(),0,&error);}
json_t* jsonPayload(const std::string& body){
    if(body.empty())return nullptr;
    size_t first=body.find('('),last=body.rfind(')');
    const char* data=body.data();size_t size=body.size();if(first!=std::string::npos&&last>first){data+=first+1;size=last-first-1;}
    json_error_t error{};return json_loadb(data,size,0,&error);
}
std::string jsonString(json_t* object,const char* key){json_t* value=json_is_object(object)?json_object_get(object,key):nullptr;return json_is_string(value)?json_string_value(value):"";}
long jsonInteger(json_t* object,const char* key,long fallback=-1){json_t* value=json_is_object(object)?json_object_get(object,key):nullptr;return json_is_integer(value)?(long)json_integer_value(value):fallback;}
void collect(const net::HttpResponse& response,Cookies& cookies){
    for(const auto& header:response.headers){std::string name=header.name;for(char& c:name)c=(char)std::tolower((unsigned char)c);if(name!="set-cookie")continue;
        size_t semicolon=header.value.find(';'),equal=header.value.find('=');if(equal==std::string::npos||equal>semicolon)continue;
        std::string key=header.value.substr(0,equal);if(key.empty())continue;
        std::string value=header.value.substr(equal+1,semicolon==std::string::npos?std::string::npos:semicolon-equal-1);
        if(!value.empty())cookies[key]=std::move(value);
    }
}
std::string headerValue(const net::HttpResponse& response,const std::string& wanted){for(const auto& header:response.headers){std::string name=header.name;for(char& c:name)c=(char)std::tolower((unsigned char)c);if(name==wanted)return header.value;}return {};}
void parseCookie(const std::string& value,Cookies& cookies){size_t pos=0;while(pos<value.size()){size_t end=value.find(';',pos);std::string part=value.substr(pos,end==std::string::npos?std::string::npos:end-pos);while(!part.empty()&&std::isspace((unsigned char)part.front()))part.erase(0,1);size_t eq=part.find('=');if(eq!=std::string::npos&&!part.substr(0,eq).empty())cookies[part.substr(0,eq)]=part.substr(eq+1);if(end==std::string::npos)break;pos=end+1;}}
std::string join(const Cookies& cookies){std::string value;for(const auto& pair:cookies){if(!value.empty())value+="; ";value+=pair.first+"="+pair.second;}return value;}
bool hasCookie(const Cookies& cookies,const char* key){auto it=cookies.find(key);return it!=cookies.end()&&!it->second.empty();}
void normalizeLoginCookies(Cookies& cookies){if(!hasCookie(cookies,"BDUSS")&&hasCookie(cookies,"BDUSS_BFESS"))cookies["BDUSS"]=cookies["BDUSS_BFESS"];}
void mirrorModernCookies(Cookies& cookies){
    if(hasCookie(cookies,"BDUSS"))cookies["BDUSS_BFESS"]=cookies["BDUSS"];
    if(hasCookie(cookies,"STOKEN"))cookies["STOKEN_BFESS"]=cookies["STOKEN"];
    if(hasCookie(cookies,"PTOKEN"))cookies["PTOKEN_BFESS"]=cookies["PTOKEN"];
    if(hasCookie(cookies,"UBI"))cookies["UBI_BFESS"]=cookies["UBI"];
    if(!hasCookie(cookies,"BAIDUID")&&hasCookie(cookies,"BAIDUID_BFESS"))cookies["BAIDUID"]=cookies["BAIDUID_BFESS"];
    if(hasCookie(cookies,"BAIDUID")&&!hasCookie(cookies,"BAIDUID_BFESS"))cookies["BAIDUID_BFESS"]=cookies["BAIDUID"];
    if(hasCookie(cookies,"BDUSS"))cookies["newlogin"]="1";
}
std::string timestampWeb(){return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()*1000);}
std::string makeGid(){
    unsigned char bytes[16];randomGet(bytes,sizeof(bytes));bytes[6]=(bytes[6]&0x0f)|0x40;bytes[8]=(bytes[8]&0x3f)|0x80;
    static const char hex[]="0123456789ABCDEF";std::string out;out.reserve(36);
    for(size_t i=0;i<sizeof(bytes);++i){if(i==4||i==6||i==8||i==10)out+='-';out+=hex[bytes[i]>>4];out+=hex[bytes[i]&15];}return out;
}
std::string looseQuotedValue(const std::string& body,const std::string& key){
    for(char quote:{'\"','\''}){std::string marker;marker+=quote;marker+=key;marker+=quote;size_t p=0;
        while((p=body.find(marker,p))!=std::string::npos){p+=marker.size();p=body.find(':',p);if(p==std::string::npos)break;++p;while(p<body.size()&&std::isspace((unsigned char)body[p]))++p;
            if(p>=body.size()||(body[p]!='\"'&&body[p]!='\''))continue;
            char valueQuote=body[p++];size_t end=body.find(valueQuote,p);if(end==std::string::npos)break;if(end>p)return body.substr(p,end-p);p=end+1;
        }
    }return {};
}

bool decodeQrPng(const std::string& bytes,std::vector<unsigned char>& modules,int& size){
    png_image image{};image.version=PNG_IMAGE_VERSION;
    if(bytes.empty()||!png_image_begin_read_from_memory(&image,bytes.data(),bytes.size()))return false;
    image.format=PNG_FORMAT_RGBA;std::vector<unsigned char> pixels(PNG_IMAGE_SIZE(image));
    if(!png_image_finish_read(&image,nullptr,pixels.data(),0,nullptr)){png_image_free(&image);return false;}
    if(image.width!=image.height||image.width<21){png_image_free(&image);return false;}
    auto dark=[&](unsigned x,unsigned y){const size_t p=((size_t)y*image.width+x)*4;return pixels[p]<128&&pixels[p+1]<128&&pixels[p+2]<128;};
    unsigned firstRun=0;while(firstRun<image.width&&dark(firstRun,0))++firstRun;
    unsigned scale=firstRun>=7&&firstRun%7==0?firstRun/7:0;
    if(scale==0||image.width%scale!=0){png_image_free(&image);return false;}
    size=(int)(image.width/scale);if(size<21||size>177){png_image_free(&image);return false;}
    modules.assign((size_t)size*size,0);
    for(int y=0;y<size;++y)for(int x=0;x<size;++x)modules[(size_t)y*size+x]=dark((unsigned)x*scale+scale/2,(unsigned)y*scale+scale/2);
    png_image_free(&image);return true;
}
}

BaiduLogin::BaiduLogin(net::HttpClient& http):http_(http){mutex_=SDL_CreateMutex();std::string ignored;QuarkSessionStore(legacySessionPath).clear(ignored);auto loaded=QuarkSessionStore(sessionPath).load();if(loaded.status==SessionLoadStatus::Loaded){saved_=loaded.session;view_.message="百度登录状态已保存，使用时验证";}}
BaiduLogin::~BaiduLogin(){shutdown();}
void BaiduLogin::cancel(){cancel_.store(true);if(thread_){SDL_WaitThread(thread_,nullptr);thread_=nullptr;}SDL_LockMutex(mutex_);view_.running=false;view_.modules.clear();view_.size=0;SDL_UnlockMutex(mutex_);}
bool BaiduLogin::logout(){
    cancel_.store(true);if(thread_){SDL_WaitThread(thread_,nullptr);thread_=nullptr;}
    std::string error;const bool current=QuarkSessionStore(sessionPath).clear(error);
    const bool legacy=QuarkSessionStore(legacySessionPath).clear(error);
    saved_={};SDL_LockMutex(mutex_);view_={};view_.message="百度网盘未登录";SDL_UnlockMutex(mutex_);return current&&legacy;
}
void BaiduLogin::shutdown(){cancel_.store(true);if(thread_){SDL_WaitThread(thread_,nullptr);thread_=nullptr;}if(mutex_){SDL_DestroyMutex(mutex_);mutex_=nullptr;}}
BaiduLoginView BaiduLogin::snapshot(){if(!mutex_){BaiduLoginView view;view.message="百度登录组件初始化失败";return view;}SDL_LockMutex(mutex_);auto view=view_;SDL_UnlockMutex(mutex_);return view;}
void BaiduLogin::publish(const std::string& message,bool finished){SDL_LockMutex(mutex_);view_.message=message;if(finished)view_.running=false;SDL_UnlockMutex(mutex_);}
void BaiduLogin::invalidate(){
    cancel_.store(true);if(thread_){SDL_WaitThread(thread_,nullptr);thread_=nullptr;}
    std::string ignored;QuarkSessionStore(sessionPath).clear(ignored);saved_={};
    SDL_LockMutex(mutex_);view_.authenticated=false;view_.session={};view_.running=false;view_.modules.clear();view_.size=0;view_.message="百度登录已失效，正在刷新二维码...";SDL_UnlockMutex(mutex_);
}
void BaiduLogin::start(){if(!mutex_)return;auto current=snapshot();if(current.running||current.authenticated)return;if(thread_){SDL_WaitThread(thread_,nullptr);thread_=nullptr;}cancel_.store(false);SDL_LockMutex(mutex_);view_.running=true;view_.modules.clear();view_.size=0;view_.message="正在准备百度网盘登录...";SDL_UnlockMutex(mutex_);thread_=SDL_CreateThread(entry,"baidu-login",this);if(!thread_)publish("无法启动百度登录线程，按 A 重试",true);}
int BaiduLogin::entry(void* self){return static_cast<BaiduLogin*>(self)->run();}
bool BaiduLogin::store(const QuarkSession& session){
    if(mkdir("sdmc:/switch",0777)!=0&&errno!=EEXIST)return false;
    if(mkdir("sdmc:/switch/OKSwitch",0777)!=0&&errno!=EEXIST)return false;
    const std::string temporary=std::string(sessionPath)+".tmp";std::string error;
    std::remove(temporary.c_str());
    if(!QuarkSessionStore(temporary).save(session,error))return false;
    if(std::remove(sessionPath)!=0&&errno!=ENOENT){std::remove(temporary.c_str());return QuarkSessionStore(sessionPath).save(session,error);}
    if(std::rename(temporary.c_str(),sessionPath)==0)return true;
    const bool saved=QuarkSessionStore(sessionPath).save(session,error);std::remove(temporary.c_str());return saved;
}

int BaiduLogin::run(){
    Cookies cookies;parseCookie(saved_.cookie,cookies);normalizeLoginCookies(cookies);mirrorModernCookies(cookies);
    auto request=[&](const std::string& url,bool follow=true,const std::string& method="GET",const std::string& body=""){net::HttpRequest req;req.url=url;req.method=method;req.body=body;req.cancel=&cancel_;req.closeConnection=true;req.connectTimeoutSeconds=5;req.timeoutSeconds=12;req.maxResponseBytes=5*1024*1024;req.followRedirects=follow;req.headers.push_back({"User-Agent",userAgent});req.headers.push_back({"Origin","https://pan.baidu.com"});req.headers.push_back({"Referer","https://pan.baidu.com/"});if(method=="POST")req.headers.push_back({"Content-Type","application/x-www-form-urlencoded; charset=UTF-8"});if(!cookies.empty())req.headers.push_back({"Cookie",join(cookies)});auto response=http_.get(req);collect(response,cookies);return response;};
    auto redirectUrl=[](const std::string& current,const std::string& location){
        if(location.rfind("http://",0)==0||location.rfind("https://",0)==0)return location;
        if(location.rfind("//",0)==0)return std::string("https:")+location;
        const size_t scheme=current.find("://"),hostEnd=scheme==std::string::npos?std::string::npos:current.find('/',scheme+3);
        const std::string origin=hostEnd==std::string::npos?current:current.substr(0,hostEnd);
        if(!location.empty()&&location.front()=='/')return origin+location;
        const size_t slash=current.rfind('/');return (slash==std::string::npos?origin+"/":current.substr(0,slash+1))+location;
    };
    auto followWithCookies=[&](std::string url,int limit){net::HttpResponse response;
        for(int step=0;step<limit&&!cancel_.load();++step){response=request(url,false);normalizeLoginCookies(cookies);mirrorModernCookies(cookies);if(response.statusCode<300||response.statusCode>=400)break;const std::string location=headerValue(response,"location");if(location.empty())break;url=redirectUrl(url,location);}
        return response;
    };
    auto finish=[&](){QuarkSession session{join(cookies),userAgent};const bool persisted=store(session);saved_=session;SDL_LockMutex(mutex_);view_.authenticated=true;view_.session=session;view_.running=false;view_.modules.clear();view_.size=0;view_.message=persisted?"百度网盘已登录":"百度网盘已登录（本次有效，状态未保存）";SDL_UnlockMutex(mutex_);};
    if(hasCookie(cookies,"BDUSS")){
        publish("正在验证百度登录状态...");auto response=request("https://pan.baidu.com/api/gettemplatevariable?channel=chunlei&web=1&app_id=250528&clienttype=0",true,"POST","fields=%5B%22bdstoken%22%5D");json_t* root=jsonBody(response);json_t* result=root?json_object_get(root,"result"):nullptr;bool valid=response.succeeded&&jsonInteger(root,"errno") == 0&&!jsonString(result,"bdstoken").empty();if(root)json_decref(root);
        if(!valid){response=request("https://pan.baidu.com/api/gettemplatevariable?channel=chunlei&web=1&app_id=250528&clienttype=0&fields=%5B%22bdstoken%22%5D");root=jsonBody(response);result=root?json_object_get(root,"result"):nullptr;valid=response.succeeded&&jsonInteger(root,"errno")==0&&!jsonString(result,"bdstoken").empty();if(root)json_decref(root);}
        if(valid){finish();return 0;}cookies.clear();saved_={};std::string ignored;QuarkSessionStore(sessionPath).clear(ignored);
    }
    if(cancel_.load())return 0;
    const std::string gid=makeGid(),qrTime=timestampWeb(),initialCallback="tangram_guid_"+qrTime;
    const std::string logPage="traceId:pc_loginv5_"+qrTime.substr(0,13)+",logPage:loginv5";
    auto response=request("https://passport.baidu.com/v2/api/getqrcode?lp=pc&qrloginfrom=pc&gid="+gid+"&callback="+initialCallback+"&apiver=v3&tt="+qrTime+"&tpl=netdisk&logPage="+encode(logPage)+"&_="+qrTime);json_t* root=jsonPayload(response.body);std::string sign=jsonString(root,"sign"),imageUrl=jsonString(root,"imgurl");long error=jsonInteger(root,"errno");if(root)json_decref(root);
    if(!response.succeeded||error!=0||sign.empty()||imageUrl.empty()){publish("百度未返回有效二维码，按 A 重试",true);return 0;}if(imageUrl.rfind("http",0)!=0)imageUrl="https://"+imageUrl;
    response=request(imageUrl);std::vector<unsigned char> modules;int size=0;if(!response.succeeded||!decodeQrPng(response.body,modules,size)){publish("百度二维码图片解析失败，按 A 重试",true);return 0;}
    SDL_LockMutex(mutex_);view_.modules=std::move(modules);view_.size=size;view_.message="请用百度网盘 App 扫码并确认";SDL_UnlockMutex(mutex_);
    const Uint64 started=SDL_GetTicks64();std::string temporaryBduss;const std::string pollCallback="tangram_guid_"+std::to_string(std::time(nullptr));
    while(!cancel_.load()&&SDL_GetTicks64()-started<60000){for(int i=0;i<20&&!cancel_.load();++i)SDL_Delay(100);if(cancel_.load())return 0;const std::string pollTime=timestampWeb();response=request("https://passport.baidu.com/channel/unicast?channel_id="+encode(sign)+"&gid="+gid+"&tpl=netdisk&_sdkFrom=1&callback="+pollCallback+"&apiver=v3&tt="+pollTime+"&_="+pollTime);root=jsonPayload(response.body);std::string channel=jsonString(root,"channel_v");if(root)json_decref(root);if(channel.empty())continue;json_error_t jsonError{};root=json_loadb(channel.data(),channel.size(),0,&jsonError);long status=jsonInteger(root,"status");temporaryBduss=jsonString(root,"v");if(root)json_decref(root);if(status==0&&!temporaryBduss.empty())break;if(status==1)publish("百度已扫码，请在手机上确认登录");}
    if(cancel_.load())return 0;
    if(temporaryBduss.empty()){SDL_LockMutex(mutex_);view_.modules.clear();view_.size=0;SDL_UnlockMutex(mutex_);publish("百度二维码已失效，按 A 刷新",true);return 0;}
    publish("扫码已确认，正在交换百度网盘凭据...");const std::string loginTime=timestampWeb();std::string callbackSuffix=gid.substr(0,6);for(char& c:callbackSuffix)c=(char)std::tolower((unsigned char)c);
    response=request("https://passport.baidu.com/v3/login/main/qrbdusslogin?v="+loginTime+"&bduss="+encode(temporaryBduss)+"&u="+encode("https%3A%2F%2Fpan.baidu.com%2Fdisk%2Fmain")+"&loginVersion=v5&qrcode=1&tpl=netdisk&maskId=&fileId=&apiver=v3&tt="+loginTime+"&traceid=&time="+std::to_string(std::time(nullptr))+"&alg=v3&elapsed=1&callback=bd__cbs__"+callbackSuffix,false);normalizeLoginCookies(cookies);
    const std::string bodyBduss=looseQuotedValue(response.body,"bduss"),bodyStoken=looseQuotedValue(response.body,"stoken");
    if(!bodyBduss.empty())cookies["BDUSS"]=bodyBduss;
    if(!bodyStoken.empty())cookies["STOKEN"]=bodyStoken;
    normalizeLoginCookies(cookies);
    mirrorModernCookies(cookies);
    if(!hasCookie(cookies,"BDUSS")){publish("百度未返回正式登录凭据，按 A 重新扫码",true);return 0;}
    std::string location=headerValue(response,"location");
    if(!location.empty()){
        publish("正在完成百度登录跳转...");response=followWithCookies(redirectUrl("https://passport.baidu.com/v3/login/main/qrbdusslogin",location),8);normalizeLoginCookies(cookies);
    }
    publish("正在激活百度网盘会话...");response=followWithCookies("https://passport.baidu.com/v3/login/api/auth/?return_type=5&tpl=netdisk&u="+encode("https://pan.baidu.com/disk/main"),8);normalizeLoginCookies(cookies);
    publish("正在初始化百度网盘会话...");response=request("https://pan.baidu.com/",true);normalizeLoginCookies(cookies);mirrorModernCookies(cookies);if(!hasCookie(cookies,"PANWEB"))cookies["PANWEB"]="1";
    publish("正在验证百度网盘登录状态...");
    response=request("https://pan.baidu.com/api/gettemplatevariable?channel=chunlei&web=1&app_id=250528&clienttype=0",true,"POST","fields=%5B%22bdstoken%22%5D");
    root=jsonBody(response);json_t* verified=root?json_object_get(root,"result"):nullptr;
    long verifyError=jsonInteger(root,"errno");
    bool valid=response.succeeded&&verifyError==0&&!jsonString(verified,"bdstoken").empty();
    if(root)json_decref(root);
    if(!valid){
        response=request("https://pan.baidu.com/api/gettemplatevariable?channel=chunlei&web=1&app_id=250528&clienttype=0&fields=%5B%22bdstoken%22%5D");
        root=jsonBody(response);verified=root?json_object_get(root,"result"):nullptr;verifyError=jsonInteger(root,"errno");valid=response.succeeded&&verifyError==0&&!jsonString(verified,"bdstoken").empty();if(root)json_decref(root);
    }
    long accountError=-999;
    if(!valid){response=request("https://pan.baidu.com/rest/2.0/xpan/nas?method=uinfo");root=jsonBody(response);accountError=jsonInteger(root,"errno");if(root)json_decref(root);}
    if(!valid){
        const bool hadBduss=hasCookie(cookies,"BDUSS"),hadStoken=hasCookie(cookies,"STOKEN"),hadPanweb=hasCookie(cookies,"PANWEB"),hadPanpsc=hasCookie(cookies,"PANPSC"),hadBaiduid=hasCookie(cookies,"BAIDUID");
        cookies.clear();saved_={};std::string ignored;QuarkSessionStore(sessionPath).clear(ignored);
        SDL_LockMutex(mutex_);view_.authenticated=false;view_.session={};view_.modules.clear();view_.size=0;SDL_UnlockMutex(mutex_);
        const std::string diagnostic="百度验证失败 T"+std::to_string(verifyError)+" U"+std::to_string(accountError)+" · B"+(hadBduss?"有":"无")+" S"+(hadStoken?"有":"无")+" W"+(hadPanweb?"有":"无")+" P"+(hadPanpsc?"有":"无")+" I"+(hadBaiduid?"有":"无")+" · A 重扫";
        publish(diagnostic,true);return 0;
    }
    finish();return 0;
}
}
