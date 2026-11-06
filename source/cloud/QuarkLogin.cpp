#include "okswitch/cloud/QuarkLogin.hpp"
#include "okswitch/cloud/QuarkLoginConfig.hpp"
#include "okswitch/cloud/qrcodegen.h"
#include <switch.h>
#include <jansson.h>
#include <map>
#include <cctype>
#include <cstdio>
#include <sys/stat.h>
#include <cerrno>
namespace okswitch::cloud {
namespace {
const char* sessionPath="sdmc:/switch/OKSwitch/quark-session.json";
std::string encode(const std::string& value){
    static const char* hex="0123456789ABCDEF";std::string result;
    for(unsigned char c:value){if(std::isalnum(c)||c=='-'||c=='_'||c=='.'||c=='~')result+=c;else{result+='%';result+=hex[c>>4];result+=hex[c&15];}}return result;
}
std::string replace(std::string value,const std::string& key,const std::string& data){auto at=value.find(key);if(at!=std::string::npos)value.replace(at,key.size(),encode(data));return value;}
std::string requestId(){unsigned char bytes[16];randomGet(bytes,sizeof(bytes));bytes[6]=(bytes[6]&15)|64;bytes[8]=(bytes[8]&63)|128;std::string output;for(int i=0;i<16;++i){if(i==4||i==6||i==8||i==10)output+="-";char pair[3];std::snprintf(pair,sizeof(pair),"%02x",bytes[i]);output+=pair;}return output;}
json_t* body(const net::HttpResponse& response){json_error_t error{};return json_loadb(response.body.data(),response.body.size(),0,&error);}
std::string member(json_t* root,const char* key){auto value=json_object_get(json_object_get(json_object_get(root,"data"),"members"),key);return json_is_string(value)?json_string_value(value):"";}
long businessStatus(json_t* root){auto value=json_object_get(root,"status");return json_is_integer(value)?(long)json_integer_value(value):0;}
using Cookies=std::map<std::string,std::string>;
void collect(const net::HttpResponse& response,Cookies& cookies){
    for(const auto& header:response.headers){
        std::string name=header.name;for(char& c:name)c=(char)std::tolower((unsigned char)c);if(name!="set-cookie")continue;
        auto end=header.value.find(';'),eq=header.value.find('=');if(eq==std::string::npos||eq>end)continue;
        auto key=header.value.substr(0,eq);if(key!="__pus"&&key!="__uid"&&key!="__kp"&&key!="__puus")continue;
        cookies[key]=header.value.substr(eq+1,end==std::string::npos?end:end-eq-1);
    }
}
std::string join(const Cookies& cookies){std::string value;for(const auto& item:cookies){if(!value.empty())value+="; ";value+=item.first+"="+item.second;}return value;}
}
QuarkLogin::QuarkLogin(net::HttpClient& http):http_(http){
    mutex_=SDL_CreateMutex();auto loaded=QuarkSessionStore(sessionPath).load();
    if(loaded.status==SessionLoadStatus::Loaded){saved_=loaded.session;view_.message="登录状态已保存，进入详情后验证";}
}
QuarkLogin::~QuarkLogin(){shutdown();}
void QuarkLogin::cancel(){cancel_.store(true);if(thread_){SDL_WaitThread(thread_,nullptr);thread_=nullptr;}SDL_LockMutex(mutex_);view_.running=false;view_.modules.clear();view_.size=0;SDL_UnlockMutex(mutex_);}
bool QuarkLogin::logout(){
    cancel_.store(true);if(thread_){SDL_WaitThread(thread_,nullptr);thread_=nullptr;}
    std::string error;const bool ok=QuarkSessionStore(sessionPath).clear(error);
    saved_={};SDL_LockMutex(mutex_);view_={};view_.message="夸克未登录";SDL_UnlockMutex(mutex_);return ok;
}
void QuarkLogin::shutdown(){cancel_.store(true);if(thread_){SDL_WaitThread(thread_,nullptr);thread_=nullptr;}if(mutex_){SDL_DestroyMutex(mutex_);mutex_=nullptr;}}
LoginView QuarkLogin::snapshot(){if(!mutex_){LoginView v;v.message="登录组件初始化失败";return v;}SDL_LockMutex(mutex_);auto v=view_;SDL_UnlockMutex(mutex_);return v;}
void QuarkLogin::publish(const std::string& message,bool finished){SDL_LockMutex(mutex_);view_.message=message;if(finished)view_.running=false;SDL_UnlockMutex(mutex_);}
void QuarkLogin::start(){
    if(!mutex_)return;
    auto current=snapshot();if(current.running||current.authenticated)return;
    if(thread_){SDL_WaitThread(thread_,nullptr);thread_=nullptr;}
    cancel_.store(false);SDL_LockMutex(mutex_);view_.running=true;view_.modules.clear();view_.size=0;view_.message="正在准备夸克登录...";SDL_UnlockMutex(mutex_);
    thread_=SDL_CreateThread(entry,"quark-login",this);if(!thread_)publish("无法启动登录线程，按 A 重试",true);
}
int QuarkLogin::entry(void* self){return static_cast<QuarkLogin*>(self)->run();}
bool QuarkLogin::store(const QuarkSession& session){
    if(mkdir("sdmc:/switch",0777)!=0&&errno!=EEXIST)return false;
    if(mkdir("sdmc:/switch/OKSwitch",0777)!=0&&errno!=EEXIST)return false;
    std::string temporary=std::string(sessionPath)+".tmp",error;
    return QuarkSessionStore(temporary).save(session,error)&&std::rename(temporary.c_str(),sessionPath)==0;
}
int QuarkLogin::run(){
    auto get=[&](std::string url,const std::string& cookie="",bool desktop=false){
        net::HttpRequest request;request.url=url;request.cancel=&cancel_;request.closeConnection=true;request.timeoutSeconds=10;
        request.headers.push_back({"User-Agent",desktop?loginConfig::userAgent:"okhttp/5.3.2"});
        if(!cookie.empty())request.headers.push_back({"Cookie",cookie});
        if(desktop)request.headers.push_back({"Referer","https://pan.quark.cn/"});
        return http_.get(request);
    };
    auto finish=[&](QuarkSession session){
        const bool persisted=store(session);
        saved_=session;SDL_LockMutex(mutex_);view_.authenticated=true;view_.session=session;view_.running=false;view_.modules.clear();view_.size=0;view_.message=persisted?"夸克已登录 · 播放器待接入":"夸克已登录（本次有效，登录状态未保存）";SDL_UnlockMutex(mutex_);
    };
    if(saved_.hasCredentials()){
        publish("正在验证已保存的登录状态...");auto response=get(loginConfig::flushUrl,saved_.cookie,true);auto root=body(response);
        bool valid=response.succeeded&&json_integer_value(json_object_get(root,"status"))==200&&json_integer_value(json_object_get(root,"code"))==0;json_decref(root);
        if(valid){Cookies cookies;size_t pos=0;while(pos<saved_.cookie.size()){size_t end=saved_.cookie.find(';',pos);auto part=saved_.cookie.substr(pos,end==std::string::npos?end:end-pos);while(!part.empty()&&part.front()==' ')part.erase(0,1);auto eq=part.find('=');if(eq!=std::string::npos)cookies[part.substr(0,eq)]=part.substr(eq+1);if(end==std::string::npos)break;pos=end+1;}collect(response,cookies);finish({join(cookies),loginConfig::userAgent});return 0;}
        if(!response.succeeded&&response.statusCode!=401&&response.statusCode!=403){publish("登录验证网络失败，按 A 重试",true);return 0;}
        saved_={};
    }
    if(cancel_.load())return 0;
    const std::string loginRequestId=requestId();
    net::HttpResponse response;json_t* root=nullptr;std::string token;
    for(int attempt=0;attempt<3&&!cancel_.load();++attempt){
        response=get(replace(loginConfig::tokenUrl,"REQUEST",loginRequestId));root=body(response);
        token=member(root,"token");json_decref(root);
        if(!response.succeeded||token.empty())continue;
        response=get(replace(replace(loginConfig::pollUrl,"REQUEST",loginRequestId),"TOKEN",token));root=body(response);
        long status=businessStatus(root);json_decref(root);
        if(response.succeeded&&status==50004001)break;
        token.clear();
    }
    if(cancel_.load())return 0;
    if(token.empty()){publish("夸克未返回有效二维码，按 A 重试",true);return 0;}
    std::string url=replace(loginConfig::qrUrl,"TOKEN",token);
    std::vector<uint8_t> scratch(qrcodegen_BUFFER_LEN_MAX),qr(qrcodegen_BUFFER_LEN_MAX);
    if(!qrcodegen_encodeText(url.c_str(),scratch.data(),qr.data(),qrcodegen_Ecc_MEDIUM,1,20,qrcodegen_Mask_AUTO,true)){publish("二维码生成失败",true);return 0;}
    int size=qrcodegen_getSize(qr.data());std::vector<unsigned char> modules(size*size);
    for(int y=0;y<size;++y)for(int x=0;x<size;++x)modules[y*size+x]=qrcodegen_getModule(qr.data(),x,y);
    SDL_LockMutex(mutex_);view_.size=size;view_.modules=std::move(modules);view_.message="请用夸克 App 扫码确认";SDL_UnlockMutex(mutex_);
    const Uint64 started=SDL_GetTicks64();std::string ticket;long pollStatus=50004001;
    while(!cancel_.load()&&SDL_GetTicks64()-started<180000){
        for(int i=0;i<20&&!cancel_.load();++i)SDL_Delay(100);
        if(cancel_.load())return 0;
        response=get(replace(replace(loginConfig::pollUrl,"REQUEST",loginRequestId),"TOKEN",token));root=body(response);
        ticket=member(root,"service_ticket");pollStatus=businessStatus(root);json_decref(root);
        if(!ticket.empty())break;
        if(pollStatus==50004002||pollStatus==50004003||pollStatus==50004004)break;
    }
    if(cancel_.load())return 0;
    if(ticket.empty()){SDL_LockMutex(mutex_);view_.modules.clear();view_.size=0;SDL_UnlockMutex(mutex_);publish(pollStatus==50004004?"登录已取消，按 A 刷新":"二维码已失效，按 A 刷新",true);return 0;}
    publish("扫码已确认，正在保存登录...");response=get(replace(loginConfig::loginUrl,"TICKET",ticket));root=body(response);
    bool success=response.succeeded&&json_is_true(json_object_get(root,"success"));json_decref(root);
    Cookies cookies;collect(response,cookies);
    if(!success||cookies["__pus"].empty()||cookies["__uid"].empty()){publish("登录凭据获取失败，按 A 重试",true);return 0;}
    response=get(loginConfig::flushUrl,join(cookies),true);root=body(response);
    success=response.succeeded&&json_integer_value(json_object_get(root,"status"))==200&&json_integer_value(json_object_get(root,"code"))==0;json_decref(root);collect(response,cookies);
    if(!success||cookies["__puus"].empty()){publish("登录认证未完成，按 A 重试",true);return 0;}
    finish({join(cookies),loginConfig::userAgent});return 0;
}
}
