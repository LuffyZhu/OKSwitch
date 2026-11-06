#include "okswitch/net/LanTextInput.hpp"
#include <switch.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <cctype>
#include <algorithm>

namespace okswitch::net {
namespace {
std::string decode(const std::string& input){
    std::string out;out.reserve(input.size());
    auto digit=[](char c)->int{return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;};
    for(size_t i=0;i<input.size();++i){
        if(input[i]=='%'&&i+2<input.size()){
            const int a=digit(input[i+1]),b=digit(input[i+2]);
            if(a>=0&&b>=0){out.push_back((char)(a*16+b));i+=2;continue;}
        }
        out.push_back(input[i]=='+'?' ':input[i]);
    }
    return out;
}
void sendAll(int fd,const std::string& data){
    size_t sent=0;while(sent<data.size()){
        const ssize_t count=send(fd,data.data()+sent,data.size()-sent,0);
        if(count<=0)break;sent+=(size_t)count;
    }
}
void respond(int fd,int status,const std::string& body,const char* type="text/html; charset=utf-8"){
    const std::string header="HTTP/1.1 "+std::to_string(status)+(status==200?" OK":" Bad Request")
        +"\r\nContent-Type: "+type+"\r\nContent-Length: "+std::to_string(body.size())
        +"\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nContent-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; form-action 'self'\r\nConnection: close\r\n\r\n";
    sendAll(fd,header+body);
}
std::string localIp(){
    const int fd=socket(AF_INET,SOCK_DGRAM,0);if(fd<0)return {};
    sockaddr_in remote{};remote.sin_family=AF_INET;remote.sin_port=htons(53);
    inet_pton(AF_INET,"8.8.8.8",&remote.sin_addr);
    std::string ip;
    if(connect(fd,(sockaddr*)&remote,sizeof(remote))==0){
        sockaddr_in local{};socklen_t size=sizeof(local);
        char text[INET_ADDRSTRLEN]{};
        if(getsockname(fd,(sockaddr*)&local,&size)==0&&inet_ntop(AF_INET,&local.sin_addr,text,sizeof(text)))ip=text;
    }
    close(fd);return ip;
}
}
bool LanTextInput::start(std::string& error){
    stop();completed_.store(false);stopping_.store(false);value_.clear();url_.clear();
    const std::string ip=localIp();if(ip.empty()){error="无法取得 Switch 的局域网地址";return false;}
    unsigned char random[16];randomGet(random,sizeof(random));
    char hex[33]{};for(int i=0;i<16;++i)std::snprintf(hex+i*2,3,"%02x",random[i]);token_=hex;
    listener_=socket(AF_INET,SOCK_STREAM,0);if(listener_<0){error="无法打开局域网输入服务";return false;}
    int reuse=1;setsockopt(listener_,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_ANY);address.sin_port=0;
    if(bind(listener_,(sockaddr*)&address,sizeof(address))!=0||listen(listener_,2)!=0){
        error="局域网输入服务启动失败";stop();return false;
    }
    socklen_t size=sizeof(address);
    if(getsockname(listener_,(sockaddr*)&address,&size)!=0){error="无法获取服务端口";stop();return false;}
    url_="http://"+ip+":"+std::to_string(ntohs(address.sin_port))+"/s/"+token_;
    thread_=SDL_CreateThread(entry,"okswitch-lan-input",this);
    if(!thread_){error="无法启动局域网输入线程";stop();return false;}
    return true;
}
void LanTextInput::stop(){
    stopping_.store(true);
    if(listener_>=0){shutdown(listener_,SHUT_RDWR);close(listener_);listener_=-1;}
    if(thread_){SDL_WaitThread(thread_,nullptr);thread_=nullptr;}
}
int LanTextInput::entry(void* context){
    auto& self=*static_cast<LanTextInput*>(context);
    while(!self.stopping_.load()&&!self.completed_.load()){
        const int listener=self.listener_;
        if(listener<0)break;
        fd_set readSet;FD_ZERO(&readSet);FD_SET(listener,&readSet);
        timeval timeout{};timeout.tv_sec=0;timeout.tv_usec=200000;
        if(select(listener+1,&readSet,nullptr,nullptr,&timeout)<=0)continue;
        const int client=accept(listener,nullptr,nullptr);
        if(client>=0){self.serve(client);close(client);}
    }
    return 0;
}
void LanTextInput::serve(int client){
    timeval timeout{};timeout.tv_sec=2;
    setsockopt(client,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    setsockopt(client,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    std::string request;char buffer[1024];size_t headerEnd=std::string::npos;
    while(request.size()<8192&&!stopping_.load()){
        const ssize_t count=recv(client,buffer,sizeof(buffer),0);if(count<=0)break;
        request.append(buffer,(size_t)count);headerEnd=request.find("\r\n\r\n");
        if(headerEnd!=std::string::npos)break;
    }
    if(headerEnd==std::string::npos)return;
    const std::string path="/s/"+token_;
    const bool get=request.compare(0,4,"GET ")==0,post=request.compare(0,5,"POST ")==0;
    const size_t pathStart=get?4:post?5:0;
    const size_t pathEnd=request.find(' ',pathStart);
    if((!get&&!post)||pathEnd==std::string::npos||request.substr(pathStart,pathEnd-pathStart)!=path){respond(client,404,"Not found","text/plain");return;}
    if(get){
        const std::string page=std::string("<!doctype html><html lang=zh><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'><title>OKSwitch 局域网输入</title>")+
            "<style>body{font:18px system-ui;background:#121826;color:#f6f8fa;max-width:620px;margin:8vh auto;padding:22px}input,button{font:inherit;box-sizing:border-box;width:100%;padding:15px;border-radius:8px;margin:12px 0}input{background:#fff;border:0}button{background:#32b294;color:white;border:0}</style>"
            +(oauthConfig_?"<h2>OKSwitch · YouTube 客户端</h2><p>粘贴 Google Cloud 下载的电视设备客户端 JSON。只在此页面打开时接收。</p><form method=post><textarea name=url required maxlength=4096 rows=9 style='width:100%'></textarea><button>发送到 Switch</button></form>"
                         :"<h2>OKSwitch · 机场订阅</h2><p>手机和 Switch 请连接同一局域网。在下方粘贴完整的 HTTPS 订阅链接。</p><form method=post><input name=url type=url required maxlength=2048 placeholder='https://...' autocomplete=off><button>发送到 Switch</button></form>")
            +"<p>页面仅在 Switch 显示二维码时有效。</p></html>";
        respond(client,200,page);return;
    }
    std::string headers=request.substr(0,headerEnd);std::transform(headers.begin(),headers.end(),headers.begin(),[](unsigned char c){return (char)std::tolower(c);});
    const size_t pos=headers.find("\r\ncontent-length:");
    if(pos==std::string::npos){respond(client,400,"Missing length","text/plain");return;}
    size_t lengthStart=pos+17;
    while(lengthStart<headers.size()&&(headers[lengthStart]==' '||headers[lengthStart]=='\t'))++lengthStart;
    size_t length=0;
    while(lengthStart+length<headers.size()&&headers[lengthStart+length]>='0'&&headers[lengthStart+length]<='9')++length;
    if(length==0){respond(client,400,"Invalid length","text/plain");return;}
    const size_t bodySize=std::strtoul(headers.c_str()+lengthStart,nullptr,10);
    if(bodySize==0||bodySize>8192){respond(client,400,"Input too long","text/plain");return;}
    std::string body=request.substr(headerEnd+4);
    while(body.size()<bodySize&&!stopping_.load()){
        const ssize_t count=recv(client,buffer,std::min(sizeof(buffer),bodySize-body.size()),0);
        if(count<=0)break;body.append(buffer,(size_t)count);
    }
    if(body.size()<bodySize)return;
    body.resize(bodySize);
    const size_t field=body.find("url=");
    if(field==std::string::npos||(field>0&&body[field-1]!='&')){respond(client,400,"Missing URL","text/plain");return;}
    const size_t end=body.find('&',field);
    const std::string value=decode(body.substr(field+4,end==std::string::npos?std::string::npos:end-field-4));
    if(!oauthConfig_&&(value.size()>2048||value.rfind("https://",0)!=0||value.find_first_of("\r\n\t ")!=std::string::npos)){
        respond(client,400,"请输入完整的 HTTPS 订阅链接","text/plain; charset=utf-8");return;
    }
    value_=value;
    respond(client,200,"<!doctype html><html lang=zh><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'><title>OKSwitch</title><h2>已发送到 Switch</h2><p>请回到 Switch 选择节点。</p></html>");
    completed_.store(true,std::memory_order_release);
}
}
