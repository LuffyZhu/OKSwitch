#include "okswitch/media/MiguLogin.hpp"
#include <switch.h>
#include <jansson.h>
extern "C" {
#include <libavutil/base64.h>
}
#include <cstdio>
#include <chrono>
#include <cerrno>
#include <sys/stat.h>
#include <algorithm>

namespace okswitch::media {
namespace {
constexpr const char* path="sdmc:/switch/OKSwitch/migu-session.json";
constexpr const char* clientId="27fb3129-5a54-45bc-8af1-7dc8f1155501";
constexpr const char* agent="Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/130.0.0.0 Safari/537.36";
std::string stringField(json_t* obj,const char* key){auto* v=json_object_get(obj,key);return json_is_string(v)?json_string_value(v):"";}
std::string uuid(){unsigned char b[16];randomGet(b,sizeof(b));b[6]=(b[6]&15)|64;b[8]=(b[8]&63)|128;char s[37];std::snprintf(s,sizeof(s),"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15]);return s;}
json_t* parse(const net::HttpResponse& r){json_error_t e{};return json_loadb(r.body.data(),r.body.size(),0,&e);}
std::string persistentDeviceId(){
    const char* file="sdmc:/switch/OKSwitch/migu-device-id.txt";
    char value[64]{};FILE* in=std::fopen(file,"rb");
    if(in){const size_t n=std::fread(value,1,sizeof(value)-1,in);std::fclose(in);
        std::string id(value,n);if(id.size()==36&&id.find_first_not_of("0123456789abcdefABCDEF-")==std::string::npos)return id;}
    if(mkdir("sdmc:/switch",0777)!=0&&errno!=EEXIST)return {};
    if(mkdir("sdmc:/switch/OKSwitch",0777)!=0&&errno!=EEXIST)return {};
    const std::string id=uuid();FILE* out=std::fopen(file,"wb");if(!out)return {};
    const bool written=std::fwrite(id.data(),1,id.size(),out)==id.size();const bool closed=std::fclose(out)==0;
    return written&&closed?id:std::string();
}
std::string loginFailure(const net::HttpResponse& response,const std::string& code,bool parsed){
    if(!response.succeeded)return "登录凭据请求未完成 · HTTP "+std::to_string(response.statusCode)+" · 请按 A 重新扫码";
    if(response.statusCode!=200)return "咪咕登录接口 HTTP "+std::to_string(response.statusCode);
    if(!parsed)return "咪咕登录响应不完整或格式错误，请按 A 重试";
    if(code=="DEVICE_EXCEED_LIMIT")return "咪咕提示登录设备数量超限，请在咪咕 App 管理登录设备";
    if(code=="SHORT_RISK")return "咪咕暂时限制设备登录，请稍后再试";
    if(code=="LONG_RISK"||code=="YOU_ARE_BLACK")return "咪咕限制了本次登录，请在官方 App 检查账号状态";
    // Only display a bounded code, never the response body or account tokens.
    if(!code.empty()&&code.size()<=48&&code.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-")==std::string::npos)
        return "咪咕登录未完成 · "+code+" · A 重新扫码";
    return "咪咕响应缺少登录凭据，请按 A 重新扫码";
}
MiguSession load(){MiguSession s;json_error_t e{};json_t* root=json_load_file(path,0,&e);if(!root)return s;s.userId=stringField(root,"userId");s.userToken=stringField(root,"userToken");s.sign=stringField(root,"sign");s.userInfoJson=stringField(root,"userInfoJson");s.clientId=stringField(root,"clientId");json_decref(root);return s;}
bool save(const MiguSession& s){if(mkdir("sdmc:/switch",0777)!=0&&errno!=EEXIST)return false;if(mkdir("sdmc:/switch/OKSwitch",0777)!=0&&errno!=EEXIST)return false;json_t* root=json_object();json_object_set_new(root,"userId",json_string(s.userId.c_str()));json_object_set_new(root,"userToken",json_string(s.userToken.c_str()));json_object_set_new(root,"sign",json_string(s.sign.c_str()));json_object_set_new(root,"userInfoJson",json_string(s.userInfoJson.c_str()));json_object_set_new(root,"clientId",json_string(s.clientId.c_str()));std::string tmp=std::string(path)+".tmp";bool ok=json_dump_file(root,tmp.c_str(),JSON_COMPACT)==0;json_decref(root);
    if(ok&&std::rename(tmp.c_str(),path)!=0){
        const std::string backup=std::string(path)+".bak";std::remove(backup.c_str());
        if(std::rename(path,backup.c_str())!=0)ok=false;
        else if(std::rename(tmp.c_str(),path)==0)std::remove(backup.c_str());
        else{std::rename(backup.c_str(),path);ok=false;}
    }
    return ok;}
std::string cookie(const net::HttpResponse& r){for(const auto& h:r.headers)if(h.name=="Set-Cookie"||h.name=="set-cookie"){auto n=h.value.find("JSESSIONID=");if(n!=std::string::npos){auto end=h.value.find(';',n);return h.value.substr(n,end==std::string::npos?end-n:end-n);}}return {};}
}
MiguLogin::MiguLogin(net::HttpClient& http):http_(http){mutex_=SDL_CreateMutex();saved_=load();view_.message=saved_.valid()?"咪咕登录已保存":"准备咪咕扫码登录";}
MiguLogin::~MiguLogin(){cancel();if(mutex_)SDL_DestroyMutex(mutex_);}
bool MiguLogin::logout(){
    cancel();const bool ok=std::remove(path)==0||errno==ENOENT;
    SDL_LockMutex(mutex_);saved_={};view_={};view_.message="咪咕未登录";SDL_UnlockMutex(mutex_);return ok;
}
void MiguLogin::cancel(){cancel_.store(true);if(thread_){SDL_WaitThread(thread_,nullptr);thread_=nullptr;}}
MiguLoginView MiguLogin::snapshot() const{SDL_LockMutex(mutex_);auto v=view_;SDL_UnlockMutex(mutex_);return v;}
void MiguLogin::publish(const std::string& message,bool finished){SDL_LockMutex(mutex_);view_.message=message;if(finished)view_.running=false;SDL_UnlockMutex(mutex_);}
void MiguLogin::start(){if(snapshot().running)return;cancel();cancel_.store(false);SDL_LockMutex(mutex_);view_={};view_.running=true;view_.message="正在获取咪咕官方二维码...";SDL_UnlockMutex(mutex_);thread_=SDL_CreateThread(entry,"migu-login",this);if(!thread_)publish("登录线程启动失败",true);}
int MiguLogin::entry(void* self){return static_cast<MiguLogin*>(self)->run();}
int MiguLogin::run(){
    auto request=[&](std::string url,std::string method="GET",std::string body="",std::string sessionCookie=""){
        net::HttpRequest q;q.url=std::move(url);q.method=std::move(method);q.body=std::move(body);q.cancel=&cancel_;q.timeoutSeconds=12;q.connectTimeoutSeconds=6;q.maxResponseBytes=256*1024;q.headers={{"User-Agent",agent},{"Referer","https://www.miguvideo.com/"}};if(!sessionCookie.empty())q.headers.push_back({"Cookie",sessionCookie});if(q.method=="POST")q.headers.push_back({"Content-Type","application/x-www-form-urlencoded"});return http_.get(q);
    };
    const std::string deviceId=persistentDeviceId();
    if(deviceId.empty()){publish("无法保存咪咕设备标识，请检查 SD 卡后按 A 重试",true);return 0;}
    net::HttpResponse response;json_t* root=nullptr;json_t* data=nullptr;
    const Uint64 started=SDL_GetTicks64();std::string token;
    while(!cancel_.load()&&token.empty()&&SDL_GetTicks64()-started<360000){
        response=request("https://passport.migu.cn/api/qrcWeb/qrcLogin?sourceID=203058","POST","isAsync=true&sourceid=203058");
        root=parse(response);data=json_object_get(root,"result");
        const std::string qr=stringField(data,"qrcUrl"),sessionId=stringField(data,"qrc_sessionid");
        if(root)json_decref(root);
        if(cancel_.load())return 0;
        if(!response.succeeded||sessionId.empty()||qr.empty()){publish("咪咕二维码获取失败，按 A 重试",true);return 0;}
        auto comma=qr.find(',');const std::string encoded=comma==std::string::npos?qr:qr.substr(comma+1);
        std::vector<unsigned char> jpeg(encoded.size()*3/4+4);
        int length=av_base64_decode(jpeg.data(),encoded.c_str(),jpeg.size());
        if(length<=0){publish("咪咕二维码解码失败，按 A 重试",true);return 0;}jpeg.resize(length);
        SDL_LockMutex(mutex_);view_.qrJpeg=std::move(jpeg);++view_.qrRevision;
        view_.message="请用咪咕视频 App 扫码，未扫码约 90 秒后换码";SDL_UnlockMutex(mutex_);
        const std::string sessionCookie=cookie(response);const Uint64 qrStarted=SDL_GetTicks64();bool scanned=false;
        while(!cancel_.load()&&token.empty()&&SDL_GetTicks64()-qrStarted<(scanned?180000u:90000u)
              &&SDL_GetTicks64()-started<360000){
            for(int i=0;i<20&&!cancel_.load();++i)SDL_Delay(100);
            if(cancel_.load())return 0;
            response=request("https://passport.migu.cn/api/qrcWeb/qrcquery?isAsync=true&sourceid=203058&qrc_sessionid="+sessionId,"GET","",sessionCookie);
            root=parse(response);const int status=(int)json_integer_value(json_object_get(root,"status"));
            data=json_object_get(root,"result");token=stringField(data,"token");
            if(root)json_decref(root);
            if(status==2000&&!token.empty())break;
            if(status==4073){scanned=true;publish("已扫码，请在手机上确认登录");}
            if(status==4072)break;
        }
    }
    if(cancel_.load())return 0;
    if(token.empty()){publish("扫码未完成或已超时",true);return 0;}
    publish("正在完成咪咕登录...");
    json_t* payload=json_object();json_object_set_new(payload,"miguToken",json_string(token.c_str()));json_object_set_new(payload,"deviceId",json_string(deviceId.c_str()));const auto now=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();json_object_set_new(payload,"timestamp",json_integer((json_int_t)now));json_object_set_new(payload,"loginType",json_string("QRCSSO"));char* json=json_dumps(payload,JSON_COMPACT);json_decref(payload);
    net::HttpRequest q;q.url=std::string("https://webapi.miguvideo.com/userCenterLogin/login/migutokenforencrypt?sign=abcde&signType=AES&clientId=")+clientId;q.method="POST";q.body=json?json:"";if(json)free(json);q.cancel=&cancel_;q.closeConnection=true;q.connectTimeoutSeconds=7;q.timeoutSeconds=25;q.maxResponseBytes=256*1024;q.headers={{"User-Agent",agent},{"Referer","https://www.miguvideo.com/"},{"Content-Type","application/json"},{"Accept-Encoding","identity"},{"sourceId","203021"},{"appType","2"},{"version","2.0"}};
    response=http_.get(q);root=parse(response);const bool parsed=json_is_object(root);
    data=json_object_get(root,"userInfo");MiguSession session;
    session.userId=stringField(data,"userId");session.userToken=stringField(data,"userToken");
    session.sign=stringField(root,"sign");session.clientId=clientId;
    if(json_is_object(data)){char* text=json_dumps(data,JSON_COMPACT);if(text){session.userInfoJson=text;free(text);}}
    const std::string code=stringField(root,"resultCode");
    const bool denied=code=="YOU_ARE_BLACK"||code=="DEVICE_EXCEED_LIMIT"||code=="SHORT_RISK"||code=="LONG_RISK";
    if(root)json_decref(root);
    if(cancel_.load())return 0;
    // The official callback accepts complete credentials unless a risk code
    // explicitly denies login; LOGIN_SUCCESS is not its sole success test.
    if(!response.succeeded||response.statusCode!=200||denied||!session.valid()){
        publish(loginFailure(response,code,parsed),true);return 0;
    }
    const bool stored=save(session);SDL_LockMutex(mutex_);saved_=session;view_.session=session;view_.authenticated=true;view_.running=false;view_.message=stored?"咪咕登录成功":"咪咕登录成功（本次有效）";SDL_UnlockMutex(mutex_);return 0;
}
}
