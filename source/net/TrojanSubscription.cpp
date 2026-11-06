#include "okswitch/net/TrojanSubscription.hpp"
#include "okswitch/net/HttpClient.hpp"
#include "okswitch/net/VmessAead.hpp"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <array>
#include <unordered_map>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <curl/curl.h>
#include <switch.h>
#include <mbedtls/md5.h>
#include <mbedtls/md.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/gcm.h>
#include <mbedtls/aes.h>
#include <mbedtls/chachapoly.h>
#include <mbedtls/sha256.h>
#include <jansson.h>
extern "C" {
#include <libavutil/base64.h>
#include <libavutil/sha.h>
#include <libavutil/mem.h>
}

namespace okswitch::net {
namespace {
std::string decodePercent(const std::string& input){
    std::string out;out.reserve(input.size());
    for(size_t i=0;i<input.size();++i){
        if(input[i]=='%'&&i+2<input.size()&&std::isxdigit((unsigned char)input[i+1])&&std::isxdigit((unsigned char)input[i+2])){
            auto hex=[](char c){return c<='9'?c-'0':(std::tolower((unsigned char)c)-'a'+10);};
            out+=(char)((hex(input[i+1])<<4)|hex(input[i+2]));i+=2;
        }else out+=input[i];
    }
    return out;
}
std::string queryValue(const std::string& query,const char* key){
    const std::string wanted=std::string(key)+"=";
    for(size_t pos=0;pos<query.size();){
        const size_t end=query.find('&',pos);const std::string part=query.substr(pos,end==std::string::npos?end:end-pos);
        if(part.rfind(wanted,0)==0)return decodePercent(part.substr(wanted.size()));
        if(end==std::string::npos)break;
        pos=end+1;
    }
    return {};
}
bool decodeBase64Url(std::string input,std::string& output){
    if(input.size()>8192)return false;
    for(char& c:input){if(c=='-')c='+';else if(c=='_')c='/';}
    while(input.size()%4)input+='=';
    std::vector<unsigned char> decoded(input.size()*3/4+4);
    const int count=av_base64_decode(decoded.data(),input.c_str(),(int)decoded.size());
    if(count<=0)return false;
    output.assign((char*)decoded.data(),(size_t)count);
    return true;
}
bool parseNode(const std::string& line,TrojanNode& node){
    if(line.rfind("trojan://",0)!=0)return false;
    const size_t fragment=line.find('#');
    const size_t question=line.find('?');
    const size_t authorityEnd=std::min(fragment==std::string::npos?line.size():fragment,
                                        question==std::string::npos?line.size():question);
    const std::string authority=line.substr(9,authorityEnd-9);
    const size_t at=authority.rfind('@');if(at==std::string::npos)return false;
    node.password=decodePercent(authority.substr(0,at));
    const std::string hostport=authority.substr(at+1);
    const size_t colon=hostport.rfind(':');if(colon==std::string::npos)return false;
    node.server=hostport.substr(0,colon);
    const std::string portString=hostport.substr(colon+1);
    if(portString.empty()||portString.size()>5||!std::all_of(portString.begin(),portString.end(),[](unsigned char c){return std::isdigit(c);}))return false;
    const unsigned long port=std::strtoul(portString.c_str(),nullptr,10);
    if(node.password.empty()||node.server.empty()||port==0||port>65535)return false;
    node.port=(unsigned short)port;
    const std::string query=question!=std::string::npos&&(fragment==std::string::npos||question<fragment)
        ?line.substr(question+1,(fragment==std::string::npos?line.size():fragment)-question-1):"";
    const std::string transport=queryValue(query,"type");
    if(!transport.empty()&&transport!="tcp")return false;
    node.sni=queryValue(query,"sni");if(node.sni.empty())node.sni=queryValue(query,"peer");
    if(node.sni.empty())node.sni=node.server;
    const std::string insecure=queryValue(query,"allowInsecure");
    node.allowInsecure=insecure=="1"||insecure=="true";
    node.name=fragment==std::string::npos?"Trojan 节点":decodePercent(line.substr(fragment+1));
    if(node.name.empty())node.name="Trojan 节点";
    if(node.name.size()>120)node.name.resize(120);
    // Reject control characters and unsafe hostnames before using them with curl.
    for(const auto& part:{node.server,node.sni})if(part.find_first_of("\r\n\t /:@")!=std::string::npos)return false;
    return true;
}
bool parseVlessNode(const std::string& line,TrojanNode& node){
    if(line.rfind("vless://",0)!=0)return false;
    const size_t fragment=line.find('#'),question=line.find('?');
    const size_t authorityEnd=std::min(fragment==std::string::npos?line.size():fragment,
        question==std::string::npos?line.size():question);
    const std::string authority=line.substr(8,authorityEnd-8);
    const size_t at=authority.rfind('@');if(at==std::string::npos)return false;
    const std::string uuid=authority.substr(0,at);
    if(uuid.size()!=36||uuid[8]!='-'||uuid[13]!='-'||uuid[18]!='-'||uuid[23]!='-')return false;
    for(size_t i=0;i<uuid.size();++i)
        if(i!=8&&i!=13&&i!=18&&i!=23&&!std::isxdigit((unsigned char)uuid[i]))return false;
    const std::string hostport=authority.substr(at+1);
    const size_t colon=hostport.rfind(':');if(colon==std::string::npos)return false;
    const std::string portString=hostport.substr(colon+1);
    if(portString.empty()||portString.size()>5||!std::all_of(portString.begin(),portString.end(),
        [](unsigned char c){return std::isdigit(c);}))return false;
    const unsigned long port=std::strtoul(portString.c_str(),nullptr,10);
    if(port==0||port>65535)return false;
    node.server=hostport.substr(0,colon);node.port=(unsigned short)port;
    const std::string query=question!=std::string::npos&&(fragment==std::string::npos||question<fragment)
        ?line.substr(question+1,(fragment==std::string::npos?line.size():fragment)-question-1):"";
    const std::string security=queryValue(query,"security");
    const std::string transport=queryValue(query,"type");
    if(security!="tls"||(!transport.empty()&&transport!="tcp")
        ||!queryValue(query,"flow").empty()||!queryValue(query,"path").empty())return false;
    if(!queryValue(query,"encryption").empty()&&queryValue(query,"encryption")!="none")return false;
    node.protocol=TrojanNode::Protocol::Vless;node.password=uuid;
    node.sni=queryValue(query,"sni");if(node.sni.empty())node.sni=queryValue(query,"host");
    if(node.sni.empty())node.sni=node.server;
    const std::string insecure=queryValue(query,"allowInsecure");
    node.allowInsecure=insecure=="1"||insecure=="true";
    node.name=fragment==std::string::npos?"VLESS 节点":decodePercent(line.substr(fragment+1));
    if(node.name.empty())node.name="VLESS 节点";
    if(node.name.size()>120)node.name.resize(120);
    if(node.server.empty())return false;
    for(const auto& part:{node.server,node.sni})
        if(part.find_first_of("\r\n\t /:@")!=std::string::npos)return false;
    return true;
}
bool parseShadowsocksNode(const std::string& line,TrojanNode& node){
    if(line.rfind("ss://",0)!=0)return false;
    const size_t fragment=line.find('#'),question=line.find('?');
    if(question!=std::string::npos&&(fragment==std::string::npos||question<fragment)
        &&!queryValue(line.substr(question+1,(fragment==std::string::npos?line.size():fragment)-question-1),
            "plugin").empty())return false;
    const size_t end=std::min(fragment==std::string::npos?line.size():fragment,
        question==std::string::npos?line.size():question);
    const std::string authority=line.substr(5,end-5);
    std::string credentials,hostport;
    const size_t at=authority.rfind('@');
    if(at==std::string::npos){
        std::string decoded;if(!decodeBase64Url(authority,decoded))return false;
        const size_t decodedAt=decoded.rfind('@');if(decodedAt==std::string::npos)return false;
        credentials=decoded.substr(0,decodedAt);hostport=decoded.substr(decodedAt+1);
    }else{
        hostport=authority.substr(at+1);
        const std::string userInfo=decodePercent(authority.substr(0,at));
        if(userInfo.find(':')!=std::string::npos)credentials=userInfo;
        else if(!decodeBase64Url(userInfo,credentials))return false;
    }
    const size_t split=credentials.find(':');if(split==std::string::npos)return false;
    node.method=credentials.substr(0,split);
    node.password=credentials.substr(split+1);
    if((node.method!="aes-128-gcm"&&node.method!="aes-256-gcm"
        &&node.method!="chacha20-ietf-poly1305")||node.password.empty())return false;
    const size_t colon=hostport.rfind(':');if(colon==std::string::npos)return false;
    node.server=hostport.substr(0,colon);
    const std::string portString=hostport.substr(colon+1);
    if(portString.empty()||portString.size()>5||!std::all_of(portString.begin(),portString.end(),
        [](unsigned char c){return std::isdigit(c);}))return false;
    const unsigned long port=std::strtoul(portString.c_str(),nullptr,10);
    if(node.server.empty()||port==0||port>65535)return false;
    node.port=(unsigned short)port;node.protocol=TrojanNode::Protocol::Shadowsocks;
    node.name=fragment==std::string::npos?"SS 节点":decodePercent(line.substr(fragment+1));
    if(node.name.empty())node.name="SS 节点";
    if(node.name.size()>120)node.name.resize(120);
    if(node.server.find_first_of("\r\n\t /:@")!=std::string::npos)return false;
    return true;
}
bool parseShadowsocksRNode(const std::string& line,TrojanNode& node){
    if(line.rfind("ssr://",0)!=0)return false;
    std::string decoded;if(!decodeBase64Url(line.substr(6),decoded))return false;
    const size_t slash=decoded.find('/');
    const std::string parts=decoded.substr(0,slash);
    const size_t first=parts.find(':');if(first==std::string::npos)return false;
    const size_t second=parts.find(':',first+1);if(second==std::string::npos)return false;
    const size_t third=parts.find(':',second+1);if(third==std::string::npos)return false;
    const size_t fourth=parts.find(':',third+1);if(fourth==std::string::npos)return false;
    const size_t fifth=parts.find(':',fourth+1);if(fifth==std::string::npos)return false;
    node.server=parts.substr(0,first);
    const std::string portString=parts.substr(first+1,second-first-1);
    if(portString.empty()||portString.size()>5||!std::all_of(portString.begin(),portString.end(),
        [](unsigned char c){return std::isdigit(c);}))return false;
    const unsigned long port=std::strtoul(portString.c_str(),nullptr,10);
    if(node.server.empty()||port==0||port>65535)return false;
    if(parts.substr(second+1,third-second-1)!="origin")return false;
    node.method=parts.substr(third+1,fourth-third-1);
    if(node.method!="aes-128-cfb"&&node.method!="aes-256-cfb")return false;
    if(parts.substr(fourth+1,fifth-fourth-1)!="plain")return false;
    if(!decodeBase64Url(parts.substr(fifth+1),node.password)||node.password.empty())return false;
    node.port=(unsigned short)port;node.protocol=TrojanNode::Protocol::ShadowsocksR;
    node.name="SSR 节点";
    if(slash!=std::string::npos){
        const size_t query=decoded.find('?',slash);
        if(query!=std::string::npos){
            const std::string remarks=queryValue(decoded.substr(query+1),"remarks");
            std::string name;if(!remarks.empty()&&decodeBase64Url(remarks,name)&&!name.empty())
                node.name=name.substr(0,120);
        }
    }
    if(node.server.find_first_of("\r\n\t /:@")!=std::string::npos)return false;
    return true;
}
bool parseVmessNode(const std::string& line,TrojanNode& node){
    if(line.rfind("vmess://",0)!=0)return false;
    std::string decoded;if(!decodeBase64Url(line.substr(8),decoded))return false;
    json_error_t error{};
    json_t* root=json_loadb(decoded.data(),decoded.size(),JSON_REJECT_DUPLICATES,&error);
    if(!root)return false;
    auto value=[&](const char* key)->std::string{
        json_t* field=json_object_get(root,key);
        if(json_is_string(field))return json_string_value(field);
        if(json_is_integer(field))return std::to_string(json_integer_value(field));
        return {};
    };
    const std::string server=value("add"),portString=value("port"),uuid=value("id");
    const std::string network=value("net"),tls=value("tls"),alterId=value("aid");
    const std::string type=value("type"),path=value("path"),security=value("security");
    const bool supported=(network.empty()||network=="tcp")&&(tls.empty()||tls=="tls")
        &&(alterId.empty()||alterId=="0")&&(type.empty()||type=="none")
        &&path.empty()&&(security.empty()||security=="auto"||security=="aes-128-gcm")
        &&portString.size()<=5&&!portString.empty()
        &&std::all_of(portString.begin(),portString.end(),[](unsigned char c){return std::isdigit(c);});
    const unsigned long port=supported?std::strtoul(portString.c_str(),nullptr,10):0;
    VmessAead probe(uuid);
    node.server=server;node.port=(unsigned short)port;node.password=uuid;
    node.sni=value("sni");if(node.sni.empty())node.sni=value("host");
    if(node.sni.empty())node.sni=server;
    node.name=value("ps");if(node.name.empty())node.name="VMess 节点";
    if(node.name.size()>120)node.name.resize(120);
    node.protocol=TrojanNode::Protocol::Vmess;node.tls=tls=="tls";
    json_decref(root);
    if(!supported||!probe.valid()||server.empty()||port==0||port>65535)return false;
    for(const auto& part:{node.server,node.sni})
        if(part.find_first_of("\r\n\t /:@")!=std::string::npos)return false;
    return true;
}
std::string trimYaml(std::string value){
    const size_t first=value.find_first_not_of(" \t\r\n");
    if(first==std::string::npos)return {};
    const size_t last=value.find_last_not_of(" \t\r\n");
    value=value.substr(first,last-first+1);
    if(value.size()>=2&&((value.front()=='\''&&value.back()=='\'')
        ||(value.front()=='"'&&value.back()=='"')))value=value.substr(1,value.size()-2);
    return value;
}
bool parseInlineYaml(const std::string& line,std::unordered_map<std::string,std::string>& fields){
    const size_t start=line.find('{'),end=line.rfind('}');
    if(start==std::string::npos||end<=start)return false;
    size_t part=start+1;char quote=0;unsigned depth=0;
    for(size_t i=part;i<=end;++i){
        if(i<end){
            const char c=line[i];
            if(quote){
                if(c==quote){if(c=='\''&&i+1<end&&line[i+1]=='\'')++i;else quote=0;}
            }else if(c=='\''||c=='"')quote=c;
            else if(c=='['||c=='{')++depth;
            else if((c==']'||c=='}')&&depth) --depth;
        }
        if(i==end||(line[i]==','&&!quote&&depth==0)){
            const std::string item=line.substr(part,i-part);
            const size_t colon=item.find(':');
            if(colon!=std::string::npos)fields[trimYaml(item.substr(0,colon))]=trimYaml(item.substr(colon+1));
            part=i+1;
        }
    }
    return !fields.empty();
}
bool parseAnytlsYamlNode(const std::string& line,TrojanNode& node){
    if(line.find("- {")==std::string::npos)return false;
    std::unordered_map<std::string,std::string> fields;
    if(!parseInlineYaml(line,fields)||fields["type"]!="anytls")return false;
    const std::string portString=fields["port"];
    if(portString.empty()||portString.size()>5||!std::all_of(portString.begin(),portString.end(),
        [](unsigned char c){return std::isdigit(c);}))return false;
    const unsigned long port=std::strtoul(portString.c_str(),nullptr,10);
    node.server=fields["server"];node.password=fields["password"];
    node.sni=fields["sni"].empty()?node.server:fields["sni"];
    node.name=fields["name"].empty()?"AnyTLS 节点":fields["name"];
    if(node.name.size()>120)node.name.resize(120);
    node.port=(unsigned short)port;node.allowInsecure=fields["skip-cert-verify"]=="true";
    node.protocol=TrojanNode::Protocol::Anytls;
    if(node.server.empty()||node.password.empty()||port==0||port>65535)return false;
    for(const auto& part:{node.server,node.sni})
        if(part.find_first_of("\r\n\t /:@")!=std::string::npos)return false;
    return true;
}
bool decodeSubscription(const std::string& body,std::string& clear){
    if(body.size()>2*1024*1024)return false;
    if(body.find("\nproxies:")!=std::string::npos||body.rfind("proxies:",0)==0){clear=body;return true;}
    if(body.find("trojan://")!=std::string::npos||body.find("vless://")!=std::string::npos
        ||body.find("ss://")!=std::string::npos||body.find("ssr://")!=std::string::npos
        ||body.find("vmess://")!=std::string::npos){clear=body;return true;}
    std::string encoded;encoded.reserve(body.size());
    for(unsigned char c:body){
        if(std::isspace(c))continue;
        encoded+=(char)(c=='-'?'+':c=='_'?'/':c);
    }
    if(encoded.empty())return false;
    while(encoded.size()%4)encoded+='=';
    std::vector<unsigned char> decoded(encoded.size()*3/4+4);
    const int length=av_base64_decode(decoded.data(),encoded.c_str(),(int)decoded.size());
    if(length<=0)return false;
    clear.assign((char*)decoded.data(),(size_t)length);
    return true;
}
bool receiveExact(int fd,unsigned char* out,size_t count,int seconds,const std::atomic<bool>& stop){
    size_t received=0;
    while(received<count&&!stop.load()){
        fd_set set;FD_ZERO(&set);FD_SET(fd,&set);timeval wait{seconds,0};
        if(select(fd+1,&set,nullptr,nullptr,&wait)<=0)return false;
        const ssize_t n=recv(fd,out+received,count-received,0);
        if(n<=0)return false;
        received+=(size_t)n;
    }
    return received==count;
}
bool sendLocal(int fd,const unsigned char* data,size_t length,const std::atomic<bool>& stop){
    size_t sent=0;
    while(sent<length&&!stop.load()){
        const ssize_t n=send(fd,data+sent,length-sent,0);
        if(n<=0)return false;
        sent+=(size_t)n;
    }
    return sent==length;
}
bool sendTls(CURL* curl,const unsigned char* data,size_t length,const std::atomic<bool>& stop){
    size_t sent=0;unsigned waits=0;
    while(sent<length&&!stop.load()){
        size_t n=0;const CURLcode result=curl_easy_send(curl,data+sent,length-sent,&n);
        if(result==CURLE_AGAIN){if(++waits>3000)return false;SDL_Delay(10);continue;}
        if(result!=CURLE_OK||n==0)return false;
        waits=0;sent+=n;
    }
    return sent==length;
}
std::string passwordDigest(const std::string& password){
    AVSHA* sha=av_sha_alloc();if(!sha)return {};
    unsigned char digest[32]{};std::string out;
    if(av_sha_init(sha,224)==0){av_sha_update(sha,(const uint8_t*)password.data(),password.size());av_sha_final(sha,digest);
        static constexpr char hex[]="0123456789abcdef";out.reserve(56);
        for(unsigned i=0;i<28;++i){out+=hex[digest[i]>>4];out+=hex[digest[i]&15];}}
    av_free(sha);return out;
}
class ShadowsocksAead {
    std::array<unsigned char,32> master_{};
    std::array<unsigned char,12> sendNonce_{},recvNonce_{};
    mbedtls_gcm_context send_{},recv_{};
    mbedtls_chachapoly_context sendChaCha_{},recvChaCha_{};
    std::vector<unsigned char> pending_;
    size_t keyLength_=0,expected_=0;
    bool sendReady_=false,recvReady_=false;
    bool chacha_=false;
    static void advance(std::array<unsigned char,12>& nonce){
        for(auto& value:nonce)if(++value!=0)break;
    }
    bool subkey(mbedtls_gcm_context& gcm,const unsigned char* salt){
        unsigned char key[32]{};
        static constexpr unsigned char info[]="ss-subkey";
        const mbedtls_md_info_t* sha1=mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
        const bool derived=sha1&&mbedtls_hkdf(sha1,salt,keyLength_,master_.data(),keyLength_,
            info,sizeof(info)-1,key,keyLength_)==0;
        const bool ok=derived&&(chacha_
            ?mbedtls_chachapoly_setkey(&((&gcm==&send_)?sendChaCha_:recvChaCha_),key)==0
            :mbedtls_gcm_setkey(&gcm,MBEDTLS_CIPHER_ID_AES,key,keyLength_*8)==0);
        std::memset(key,0,sizeof(key));return ok;
    }
    bool seal(const unsigned char* plain,size_t length,std::vector<unsigned char>& output){
        const size_t begin=output.size();output.resize(begin+length+16);
        const int rc=chacha_
            ?mbedtls_chachapoly_encrypt_and_tag(&sendChaCha_,length,sendNonce_.data(),
                nullptr,0,plain,output.data()+begin,output.data()+begin+length)
            :mbedtls_gcm_crypt_and_tag(&send_,MBEDTLS_GCM_ENCRYPT,length,
                sendNonce_.data(),sendNonce_.size(),nullptr,0,plain,output.data()+begin,
                16,output.data()+begin+length);
        if(rc!=0)return false;
        advance(sendNonce_);return true;
    }
    bool open(const unsigned char* cipher,size_t length,unsigned char* plain){
        const int rc=chacha_
            ?mbedtls_chachapoly_auth_decrypt(&recvChaCha_,length,recvNonce_.data(),
                nullptr,0,cipher+length,cipher,plain)
            :mbedtls_gcm_auth_decrypt(&recv_,length,recvNonce_.data(),recvNonce_.size(),
                nullptr,0,cipher+length,16,cipher,plain);
        if(rc!=0)return false;
        advance(recvNonce_);return true;
    }
public:
    ShadowsocksAead(const std::string& password,const std::string& method){
        mbedtls_gcm_init(&send_);mbedtls_gcm_init(&recv_);
        mbedtls_chachapoly_init(&sendChaCha_);mbedtls_chachapoly_init(&recvChaCha_);
        chacha_=method=="chacha20-ietf-poly1305";
        keyLength_=method=="aes-128-gcm"?16:method=="aes-256-gcm"||chacha_?32:0;
        if(!keyLength_)return;
        unsigned char previous[16]{};size_t filled=0;
        while(filled<keyLength_){
            std::string input((char*)previous,filled?16:0);input+=password;
            if(mbedtls_md5_ret((const unsigned char*)input.data(),input.size(),previous)!=0){keyLength_=0;return;}
            const size_t n=std::min<size_t>(16,keyLength_-filled);
            std::memcpy(master_.data()+filled,previous,n);filled+=n;
        }
        std::memset(previous,0,sizeof(previous));
    }
    ~ShadowsocksAead(){mbedtls_gcm_free(&send_);mbedtls_gcm_free(&recv_);
        mbedtls_chachapoly_free(&sendChaCha_);mbedtls_chachapoly_free(&recvChaCha_);}
    bool valid() const{return keyLength_!=0;}
    bool encode(const unsigned char* data,size_t length,std::vector<unsigned char>& output){
        output.clear();if(!valid())return false;
        if(!sendReady_){
            std::array<unsigned char,32> salt{};randomGet(salt.data(),keyLength_);
            if(!subkey(send_,salt.data()))return false;
            output.insert(output.end(),salt.begin(),salt.begin()+keyLength_);
            sendReady_=true;
        }
        for(size_t offset=0;offset<length;){
            const size_t count=std::min<size_t>(0x3fff,length-offset);
            const unsigned char size[2]={(unsigned char)(count>>8),(unsigned char)count};
            if(!seal(size,2,output)||!seal(data+offset,count,output))return false;
            offset+=count;
        }
        return true;
    }
    bool decode(const unsigned char* data,size_t length,std::vector<unsigned char>& output){
        output.clear();if(!valid()||pending_.size()+length>128*1024)return false;
        pending_.insert(pending_.end(),data,data+length);
        if(!recvReady_){
            if(pending_.size()<keyLength_)return true;
            if(!subkey(recv_,pending_.data()))return false;
            pending_.erase(pending_.begin(),pending_.begin()+keyLength_);
            recvReady_=true;
        }
        for(;;){
            if(expected_==0){
                if(pending_.size()<18)break;
                unsigned char size[2]{};if(!open(pending_.data(),2,size))return false;
                expected_=((size_t)size[0]<<8)|size[1];
                if(expected_==0||expected_>0x3fff)return false;
                pending_.erase(pending_.begin(),pending_.begin()+18);
            }
            if(pending_.size()<expected_+16)break;
            const size_t start=output.size();output.resize(start+expected_);
            if(!open(pending_.data(),expected_,output.data()+start))return false;
            pending_.erase(pending_.begin(),pending_.begin()+expected_+16);
            expected_=0;
        }
        return true;
    }
};
class ShadowsocksStream {
    mbedtls_aes_context send_{},recv_{};
    std::array<unsigned char,16> sendIv_{},recvIv_{};
    std::vector<unsigned char> pendingIv_;
    size_t sendOffset_=0,recvOffset_=0,keyLength_=0;
    bool sendReady_=false,recvReady_=false;
public:
    ShadowsocksStream(const std::string& password,const std::string& method){
        mbedtls_aes_init(&send_);mbedtls_aes_init(&recv_);
        keyLength_=method=="aes-128-cfb"?16:method=="aes-256-cfb"?32:0;
        if(!keyLength_)return;
        std::array<unsigned char,32> key{};
        unsigned char previous[16]{};size_t filled=0;
        while(filled<keyLength_){
            std::string input((char*)previous,filled?16:0);input+=password;
            if(mbedtls_md5_ret((const unsigned char*)input.data(),input.size(),previous)!=0){keyLength_=0;return;}
            const size_t n=std::min<size_t>(16,keyLength_-filled);
            std::memcpy(key.data()+filled,previous,n);filled+=n;
        }
        if(mbedtls_aes_setkey_enc(&send_,key.data(),keyLength_*8)!=0
            ||mbedtls_aes_setkey_enc(&recv_,key.data(),keyLength_*8)!=0)keyLength_=0;
        std::memset(previous,0,sizeof(previous));key.fill(0);
    }
    ~ShadowsocksStream(){mbedtls_aes_free(&send_);mbedtls_aes_free(&recv_);}
    bool valid() const{return keyLength_!=0;}
    bool encode(const unsigned char* data,size_t length,std::vector<unsigned char>& output){
        output.clear();if(!valid())return false;
        if(!sendReady_){randomGet(sendIv_.data(),sendIv_.size());
            output.insert(output.end(),sendIv_.begin(),sendIv_.end());sendReady_=true;}
        const size_t start=output.size();output.resize(start+length);
        return mbedtls_aes_crypt_cfb128(&send_,MBEDTLS_AES_ENCRYPT,length,&sendOffset_,
            sendIv_.data(),data,output.data()+start)==0;
    }
    bool decode(const unsigned char* data,size_t length,std::vector<unsigned char>& output){
        output.clear();if(!valid())return false;
        if(!recvReady_){
            const size_t n=std::min<size_t>(16-pendingIv_.size(),length);
            pendingIv_.insert(pendingIv_.end(),data,data+n);data+=n;length-=n;
            if(pendingIv_.size()<16)return true;
            std::copy(pendingIv_.begin(),pendingIv_.end(),recvIv_.begin());
            pendingIv_.clear();recvReady_=true;
        }
        output.resize(length);
        return mbedtls_aes_crypt_cfb128(&recv_,MBEDTLS_AES_DECRYPT,length,&recvOffset_,
            recvIv_.data(),data,output.data())==0;
    }
};
class AnytlsStream {
    std::vector<unsigned char> pending_;
    bool acked_=false,streamClosed_=false,sessionClosed_=false;
    uint32_t streamId_=0,packet_=0,stopPadding_=8;
    std::string scheme_,updatedScheme_;
    std::unordered_map<uint32_t,std::vector<std::pair<int,int>>> padding_;
    static void appendFrame(std::vector<unsigned char>& output,unsigned char command,
                            uint32_t stream,const unsigned char* data,size_t length){
        output.push_back(command);
        output.push_back((unsigned char)(stream>>24));output.push_back((unsigned char)(stream>>16));
        output.push_back((unsigned char)(stream>>8));output.push_back((unsigned char)stream);
        output.push_back((unsigned char)(length>>8));output.push_back((unsigned char)length);
        if(length)output.insert(output.end(),data,data+length);
    }
    static bool parseScheme(const std::string& raw,uint32_t& stop,
                            std::unordered_map<uint32_t,std::vector<std::pair<int,int>>>& rows){
        if(raw.empty()||raw.size()>4096)return false;
        const auto number=[](const std::string& value,int& result)->bool{
            if(value.empty()||value.size()>5)return false;
            int n=0;for(char c:value){if(c<'0'||c>'9')return false;n=n*10+c-'0';}
            result=n;return true;
        };
        rows.clear();bool haveStop=false;
        for(size_t pos=0;pos<raw.size();){
            const size_t end=raw.find('\n',pos);
            const std::string line=raw.substr(pos,end==std::string::npos?end:end-pos);
            const size_t equal=line.find('=');if(equal==std::string::npos)return false;
            const std::string key=line.substr(0,equal),value=line.substr(equal+1);
                if(key=="stop"){
                    int n=0;if(!number(value,n)||n>32)return false;
                    stop=(uint32_t)n;haveStop=true;
                }else{
                    int index=0;if(!number(key,index)||index>31)return false;
                    auto& row=rows[(uint32_t)index];
                    for(size_t at=0;at<value.size();){
                        const size_t comma=value.find(',',at);
                        const std::string item=value.substr(at,comma==std::string::npos?comma:comma-at);
                        if(item=="c")row.emplace_back(-1,-1);
                        else {
                            const size_t dash=item.find('-');if(dash==std::string::npos)return false;
                            int lo=0,hi=0;
                            if(!number(item.substr(0,dash),lo)||!number(item.substr(dash+1),hi))return false;
                            if(lo<1||hi<1||lo>16384||hi>16384)return false;
                            row.emplace_back(std::min(lo,hi),std::max(lo,hi));
                        }
                        if(row.size()>16)return false;
                        if(comma==std::string::npos)break;
                        at=comma+1;
                    }
                }
            if(end==std::string::npos)break;
            pos=end+1;
        }
        return haveStop;
    }
    bool sendPacket(CURL* curl,const std::vector<unsigned char>& payload,const std::atomic<bool>& stop){
        ++packet_;
        size_t offset=0;
        if(packet_<stopPadding_){
            const auto found=padding_.find(packet_);
            if(found!=padding_.end())for(const auto& range:found->second){
                if(range.first<0){if(offset==payload.size())break;continue;}
                unsigned char entropy[4]{};randomGet(entropy,sizeof(entropy));
                const int size=range.first+(int)(((uint32_t)entropy[0]<<24|(uint32_t)entropy[1]<<16|
                    (uint32_t)entropy[2]<<8|entropy[3])%(uint32_t)(range.second-range.first+1));
                const size_t available=payload.size()-offset;
                if(available>(size_t)size){
                    if(!sendTls(curl,payload.data()+offset,size,stop))return false;
                    offset+=size;
                }else{
                    std::vector<unsigned char> record(payload.begin()+offset,payload.end());
                    const int waste=available?size-(int)available-7:size;
                    if(waste>0){
                        appendFrame(record,0,0,nullptr,0);
                        record[record.size()-2]=(unsigned char)(waste>>8);
                        record[record.size()-1]=(unsigned char)waste;
                        record.insert(record.end(),(size_t)waste,0);
                    }
                    if(!record.empty()&&!sendTls(curl,record.data(),record.size(),stop))return false;
                    offset=payload.size();
                }
            }
        }
        return offset==payload.size()||sendTls(curl,payload.data()+offset,payload.size()-offset,stop);
    }
public:
    static std::string defaultScheme(){return "stop=8\n0=30-30\n1=100-400\n"
        "2=400-500,c,500-1000,c,500-1000,c,500-1000,c,500-1000\n"
        "3=9-9,500-1000\n4=500-1000\n5=500-1000\n6=500-1000\n7=500-1000";}
    bool closed() const{return streamClosed_||sessionClosed_;}
    bool reusable() const{return !sessionClosed_&&streamId_<UINT32_MAX;}
    std::string takeUpdatedScheme(){return std::move(updatedScheme_);}
    bool finish(CURL* curl,const std::atomic<bool>& stop){
        if(!reusable()||!streamId_)return false;
        std::vector<unsigned char> fin;appendFrame(fin,3,streamId_,nullptr,0);
        return sendPacket(curl,fin,stop);
    }
    bool start(CURL* curl,const std::string& password,const std::vector<unsigned char>& target,
               const std::atomic<bool>& stop,const std::string& rawScheme){
        if(target.empty()||target.size()>65535)return false;
        scheme_=rawScheme.empty()?defaultScheme():rawScheme;
        if(!parseScheme(scheme_,stopPadding_,padding_))return false;
        unsigned char passwordHash[32],paddingHash[16];
        if(mbedtls_sha256_ret((const unsigned char*)password.data(),password.size(),passwordHash,0)!=0
            ||mbedtls_md5_ret((const unsigned char*)scheme_.data(),scheme_.size(),paddingHash)!=0)return false;
        std::array<unsigned char,64> auth{};
        std::copy(passwordHash,passwordHash+32,auth.begin());auth[33]=30;
        if(!sendTls(curl,auth.data(),auth.size(),stop))return false;
        static constexpr char hex[]="0123456789abcdef";
        std::string settings="v=2\nclient=OKSwitch/1.0\npadding-md5=";
        for(unsigned char c:paddingHash){settings+=hex[c>>4];settings+=hex[c&15];}
        settings+='\n';
        std::vector<unsigned char> packet;
        appendFrame(packet,4,0,(const unsigned char*)settings.data(),settings.size());
        streamId_=1;appendFrame(packet,1,streamId_,nullptr,0);
        appendFrame(packet,2,streamId_,target.data(),target.size());
        if(!sendPacket(curl,packet,stop))return false;
        return waitAck(curl,stop,6000);
    }
    bool open(CURL* curl,const std::vector<unsigned char>& target,const std::atomic<bool>& stop){
        if(!reusable()||target.empty()||target.size()>65535)return false;
        ++streamId_;acked_=false;streamClosed_=false;
        std::vector<unsigned char> packet;
        appendFrame(packet,1,streamId_,nullptr,0);
        appendFrame(packet,2,streamId_,target.data(),target.size());
        if(!sendPacket(curl,packet,stop))return false;
        // A reused session may have gone stale while idle. The caller retries
        // with a fresh TLS connection, so do not outwait the HTTP client.
        return waitAck(curl,stop,2000);
    }
    bool waitAck(CURL* curl,const std::atomic<bool>& stop,Uint64 timeoutMs){
        const Uint64 deadline=SDL_GetTicks64()+timeoutMs;
        while(!stop.load()&&!closed()&&SDL_GetTicks64()<deadline){
            unsigned char buffer[4096];size_t n=0;
            const CURLcode result=curl_easy_recv(curl,buffer,sizeof(buffer),&n);
            if(result==CURLE_AGAIN){SDL_Delay(10);continue;}
            if(result!=CURLE_OK||n==0)return false;
            std::vector<unsigned char> output,reply;
            if(!decode(buffer,n,output,reply))return false;
            if(!reply.empty()&&!sendTls(curl,reply.data(),reply.size(),stop))return false;
            if(acked_&&!closed())return true;
        }
        return false;
    }
    bool encode(const unsigned char* data,size_t length,std::vector<unsigned char>& output){
        output.clear();if(!acked_||closed())return false;
        for(size_t offset=0;offset<length;){
            const size_t count=std::min<size_t>(65535,length-offset);
            appendFrame(output,2,streamId_,data+offset,count);offset+=count;
        }
        return true;
    }
    bool sendData(CURL* curl,const unsigned char* data,size_t length,const std::atomic<bool>& stop){
        std::vector<unsigned char> packet;
        return encode(data,length,packet)&&sendPacket(curl,packet,stop);
    }
    bool decode(const unsigned char* data,size_t length,std::vector<unsigned char>& output,
                std::vector<unsigned char>& reply){
        output.clear();reply.clear();
        if(pending_.size()+length>256*1024)return false;
        pending_.insert(pending_.end(),data,data+length);
        size_t offset=0;
        while(pending_.size()-offset>=7){
            const unsigned char command=pending_[offset];
            const uint32_t stream=((uint32_t)pending_[offset+1]<<24)
                |((uint32_t)pending_[offset+2]<<16)|((uint32_t)pending_[offset+3]<<8)|pending_[offset+4];
            const size_t size=((size_t)pending_[offset+5]<<8)|pending_[offset+6];
            if(pending_.size()-offset<7+size)break;
            const unsigned char* payload=pending_.data()+offset+7;
            if(command==2&&stream==streamId_)output.insert(output.end(),payload,payload+size);
            else if(command==7&&stream==streamId_){if(size)streamClosed_=true;else acked_=true;}
            else if(command==3&&stream==streamId_)streamClosed_=true;
            else if(command==5)sessionClosed_=true;
            else if(command==6){
                std::string raw((const char*)payload,size);
                uint32_t parsedStop=0;std::unordered_map<uint32_t,std::vector<std::pair<int,int>>> parsed;
                if(parseScheme(raw,parsedStop,parsed))updatedScheme_=std::move(raw);
            }
            else if(command==8)appendFrame(reply,9,stream,nullptr,0);
            else if(command!=0&&command!=10)return false;
            offset+=7+size;
        }
        if(offset)pending_.erase(pending_.begin(),pending_.begin()+offset);
        return true;
    }
};
}

bool loadTrojanSubscription(HttpClient& http,const std::string& url,std::vector<TrojanNode>& nodes,std::string& error,const std::atomic<bool>* cancel){
    nodes.clear();error.clear();
    if(url.rfind("https://",0)!=0||url.find_first_of("\r\n\t ")!=std::string::npos||url.size()>2048){error="订阅地址必须是 HTTPS";return false;}
    HttpRequest request;request.url=url;request.connectTimeoutSeconds=12;request.timeoutSeconds=25;
    request.cancel=cancel;
    request.maxResponseBytes=2*1024*1024;request.headers.push_back({"User-Agent","v2rayN"});
    HttpResponse response=http.get(request);
    if(!response.succeeded&&response.statusCode==0&&(!cancel||!cancel->load())){
        // A stale pooled TLS connection can fail before any HTTP response.
        // Retry once with a fresh connection, keeping the same certificate checks.
        request.closeConnection=true;
        request.connectTimeoutSeconds=18;
        request.timeoutSeconds=30;
        response=http.get(request);
    }
    if(response.succeeded&&response.statusCode==200&&response.body.empty()
        &&(!cancel||!cancel->load())){
        // Some subscription services return an empty 200 to generic clients.
        request.headers.clear();request.headers.push_back({"User-Agent","Shadowrocket/2.2.60"});
        request.closeConnection=true;
        response=http.get(request);
    }
    if(!response.succeeded||response.statusCode!=200){
        error="订阅下载失败";
        if(response.statusCode>0)error+="（HTTP "+std::to_string(response.statusCode)+"）";
        else error+="："+(response.error.empty()?std::string("连接未收到响应"):response.error);
        return false;
    }
    if(response.body.empty()){error="订阅服务器返回空内容，未提供节点配置";return false;}
    std::string clear;if(!decodeSubscription(response.body,clear)){error="订阅格式不受支持";return false;}
    size_t pos=0;unsigned unsupported=0,candidates=0;
    while(pos<clear.size()){
        const size_t end=clear.find('\n',pos);std::string line=clear.substr(pos,end==std::string::npos?end:end-pos);
        if(!line.empty()&&line.back()=='\r')line.pop_back();
        TrojanNode node;if(parseNode(line,node)||parseVlessNode(line,node)
            ||parseShadowsocksNode(line,node)||parseShadowsocksRNode(line,node)
            ||parseVmessNode(line,node)||parseAnytlsYamlNode(line,node))nodes.push_back(std::move(node));
        else if(line.find("://")!=std::string::npos){
            ++unsupported;
            if(line.rfind("trojan://",0)==0||line.rfind("vless://",0)==0
                ||line.rfind("ss://",0)==0||line.rfind("ssr://",0)==0
                ||line.rfind("vmess://",0)==0)++candidates;
        }
        if(end==std::string::npos)break;
        pos=end+1;
    }
    if(nodes.empty()){
        error=candidates?"订阅节点的传输或加密参数暂不支持"
            :unsupported?"订阅中的节点协议暂不支持"
            :clear.rfind("STATUS=",0)==0?"订阅只返回流量状态，没有服务器节点；请试用其他订阅格式"
            :"订阅没有返回可连接的节点；请试用其他订阅格式";
        return false;
    }
    return true;
}

NodeLatency::NodeLatency(const std::vector<TrojanNode>& nodes):nodes_(nodes),values_(nodes.size(),-1){
    mutex_=SDL_CreateMutex();
    if(mutex_)for(auto& thread:threads_)thread=SDL_CreateThread(entry,"node-latency",this);
}
NodeLatency::~NodeLatency(){
    stop_.store(true);
    for(auto* thread:threads_)if(thread)SDL_WaitThread(thread,nullptr);
    if(mutex_)SDL_DestroyMutex(mutex_);
}
std::vector<int> NodeLatency::snapshot(){
    if(!mutex_)return std::vector<int>(nodes_.size(),-4);
    SDL_LockMutex(mutex_);auto result=values_;SDL_UnlockMutex(mutex_);return result;
}
int NodeLatency::entry(void* context){
    auto& self=*static_cast<NodeLatency*>(context);
    while(!self.stop_.load()){
        size_t index=self.nodes_.size();
        SDL_LockMutex(self.mutex_);
        for(size_t offset=0;offset<self.nodes_.size();++offset){
            const size_t candidate=(self.focus_.load()+offset)%self.nodes_.size();
            if(self.values_[candidate]==-1){index=candidate;self.values_[index]=-2;break;}
        }
        SDL_UnlockMutex(self.mutex_);
        if(index==self.nodes_.size())break;
        const auto& node=self.nodes_[index];
        int value=-4;CURL* curl=curl_easy_init();
        if(curl){
            const bool plain=node.protocol==TrojanNode::Protocol::Shadowsocks
                ||node.protocol==TrojanNode::Protocol::ShadowsocksR||!node.tls;
            const std::string host=plain?node.server:(node.sni.empty()?node.server:node.sni);
            const std::string address=(plain?"http://":"https://")+host+":"+std::to_string(node.port)+"/";
            const std::string mapping=host+":"+std::to_string(node.port)+":"+node.server+":"+std::to_string(node.port);
            curl_slist* connectTo=curl_slist_append(nullptr,mapping.c_str());
            curl_easy_setopt(curl,CURLOPT_URL,address.c_str());curl_easy_setopt(curl,CURLOPT_CONNECT_TO,connectTo);
            curl_easy_setopt(curl,CURLOPT_CONNECT_ONLY,1L);curl_easy_setopt(curl,CURLOPT_PROXY,"");
            curl_easy_setopt(curl,CURLOPT_CAINFO,"romfs:/cacert.pem");
            curl_easy_setopt(curl,CURLOPT_SSL_VERIFYPEER,node.allowInsecure?0L:1L);
            curl_easy_setopt(curl,CURLOPT_SSL_VERIFYHOST,node.allowInsecure?0L:2L);
            curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT_MS,4000L);curl_easy_setopt(curl,CURLOPT_TIMEOUT_MS,4000L);
            curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);curl_easy_setopt(curl,CURLOPT_IPRESOLVE,CURL_IPRESOLVE_V4);
            curl_easy_setopt(curl,CURLOPT_NOPROGRESS,0L);
            curl_easy_setopt(curl,CURLOPT_XFERINFOFUNCTION,+[](void* p,curl_off_t,curl_off_t,curl_off_t,curl_off_t)->int{return static_cast<NodeLatency*>(p)->stop_.load()?1:0;});
            curl_easy_setopt(curl,CURLOPT_XFERINFODATA,&self);
            const Uint64 started=SDL_GetTicks64();const CURLcode result=curl_easy_perform(curl);
            value=result==CURLE_OK?std::max(1,(int)(SDL_GetTicks64()-started)):result==CURLE_OPERATION_TIMEDOUT?-3:-4;
            curl_easy_cleanup(curl);curl_slist_free_all(connectTo);
        }
        SDL_LockMutex(self.mutex_);self.values_[index]=value;SDL_UnlockMutex(self.mutex_);
    }
    return 0;
}

struct TrojanSocksProxy::Client {
    TrojanSocksProxy* owner=nullptr;
    std::atomic<int> socket{-1};
    SDL_Thread* thread=nullptr;
    std::atomic<bool> done{false};
};
struct TrojanSocksProxy::IdleSession {
    CURL* curl=nullptr;
    curl_slist* connectTo=nullptr;
    std::unique_ptr<AnytlsStream> stream;
    Uint64 idleSince=0;
    ~IdleSession(){if(curl)curl_easy_cleanup(curl);if(connectTo)curl_slist_free_all(connectTo);}
};
TrojanSocksProxy::~TrojanSocksProxy(){stop();}
std::string TrojanSocksProxy::proxyUrl() const {return port_?"socks5h://127.0.0.1:"+std::to_string(port_):"";}
bool TrojanSocksProxy::start(const TrojanNode& node,std::string& error){
    stop();error.clear();
    if(node.server.empty()||node.password.empty()||node.port==0){error="节点配置不完整";return false;}
    listener_=socket(AF_INET,SOCK_STREAM,0);if(listener_<0){error="无法创建本地代理";return false;}
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=0;
    if(bind(listener_,(sockaddr*)&address,sizeof(address))<0||listen(listener_,12)<0){error="无法启动本地代理";close(listener_);listener_=-1;return false;}
    socklen_t size=sizeof(address);if(getsockname(listener_,(sockaddr*)&address,&size)<0){error="无法获取代理端口";close(listener_);listener_=-1;return false;}
    clientsMutex_=SDL_CreateMutex();if(!clientsMutex_){error="代理内存不足";close(listener_);listener_=-1;return false;}
    idleMutex_=SDL_CreateMutex();if(!idleMutex_){error="代理内存不足";stop();return false;}
    paddingScheme_=AnytlsStream::defaultScheme();
    node_=node;port_=ntohs(address.sin_port);stopping_.store(false);
    acceptThread_=SDL_CreateThread(acceptEntry,"trojan-listener",this);
    if(!acceptThread_){error="无法启动代理线程";stop();return false;}
    return true;
}
void TrojanSocksProxy::reap(){
    if(!clientsMutex_)return;
    std::vector<Client*> finished;SDL_LockMutex(clientsMutex_);
    auto it=clients_.begin();while(it!=clients_.end()){
        if((*it)->done.load()){finished.push_back(*it);it=clients_.erase(it);}else ++it;
    }
    SDL_UnlockMutex(clientsMutex_);
    for(Client* client:finished){if(client->thread)SDL_WaitThread(client->thread,nullptr);delete client;}
}
void TrojanSocksProxy::stop(){
    stopping_.store(true);
    if(listener_>=0){shutdown(listener_,SHUT_RDWR);close(listener_);listener_=-1;}
    if(acceptThread_){SDL_WaitThread(acceptThread_,nullptr);acceptThread_=nullptr;}
    if(clientsMutex_){
        SDL_LockMutex(clientsMutex_);for(Client* client:clients_)if(client->socket.load()>=0)shutdown(client->socket.load(),SHUT_RDWR);SDL_UnlockMutex(clientsMutex_);
        for(Client* client:clients_){if(client->thread)SDL_WaitThread(client->thread,nullptr);delete client;}
        clients_.clear();SDL_DestroyMutex(clientsMutex_);clientsMutex_=nullptr;
    }
    if(idleMutex_){
        SDL_LockMutex(idleMutex_);
        for(auto* session:idleSessions_)delete session;
        idleSessions_.clear();paddingScheme_.clear();
        SDL_UnlockMutex(idleMutex_);SDL_DestroyMutex(idleMutex_);idleMutex_=nullptr;
    }
    port_=0;node_={};
}
int TrojanSocksProxy::acceptEntry(void* context){
    auto* self=(TrojanSocksProxy*)context;
    const int listener=self->listener_;
    Uint64 nextIdleSweep=SDL_GetTicks64()+5000;
    while(!self->stopping_.load()){
        fd_set set;FD_ZERO(&set);FD_SET(listener,&set);timeval wait{0,100000};
        const int ready=select(listener+1,&set,nullptr,nullptr,&wait);
        if(ready>0){int incoming=accept(listener,nullptr,nullptr);
            if(incoming>=0){
                SDL_LockMutex(self->clientsMutex_);const bool full=self->clients_.size()>=16;SDL_UnlockMutex(self->clientsMutex_);
                if(full){close(incoming);continue;}
                auto* client=new Client;client->owner=self;client->socket.store(incoming);
                SDL_LockMutex(self->clientsMutex_);self->clients_.push_back(client);SDL_UnlockMutex(self->clientsMutex_);
                client->thread=SDL_CreateThread(clientEntry,"trojan-client",client);
                if(!client->thread){close(incoming);client->socket.store(-1);client->done.store(true);}
            }}
        self->reap();
        if(self->idleMutex_&&SDL_GetTicks64()>=nextIdleSweep){
            std::vector<IdleSession*> expired;
            SDL_LockMutex(self->idleMutex_);
            auto it=self->idleSessions_.begin();
            while(it!=self->idleSessions_.end()){
                if(SDL_GetTicks64()-(*it)->idleSince>30000){expired.push_back(*it);it=self->idleSessions_.erase(it);}
                else ++it;
            }
            SDL_UnlockMutex(self->idleMutex_);
            for(auto* idle:expired)delete idle;
            nextIdleSweep=SDL_GetTicks64()+5000;
        }
    }
    return 0;
}
int TrojanSocksProxy::clientEntry(void* context){
    auto* client=(Client*)context;auto* self=client->owner;const int fd=client->socket.load();
    CURL* curl=nullptr;curl_slist* connectTo=nullptr;
    std::unique_ptr<AnytlsStream> any;
    bool streamActive=false,connectionHealthy=false;
    bool freshYoutubeConnection=false;
    do {
        unsigned char hello[2];if(!receiveExact(fd,hello,2,8,self->stopping_)||hello[0]!=5||hello[1]>32)break;
        std::vector<unsigned char> methods(hello[1]);
        if(!receiveExact(fd,methods.data(),methods.size(),8,self->stopping_)||std::find(methods.begin(),methods.end(),0)==methods.end())break;
        const unsigned char accepted[2]={5,0};if(!sendLocal(fd,accepted,2,self->stopping_))break;
        unsigned char header[4];if(!receiveExact(fd,header,4,8,self->stopping_)||header[0]!=5||header[1]!=1)break;
        std::vector<unsigned char> target;
        if(header[3]==1){target.resize(4);}
        else if(header[3]==4){target.resize(16);}
        else if(header[3]==3){unsigned char length;if(!receiveExact(fd,&length,1,8,self->stopping_)||length==0)break;target.push_back(length);target.resize((size_t)length+1);}
        else break;
        const size_t offset=header[3]==3?1:0;
        if(!receiveExact(fd,target.data()+offset,target.size()-offset,8,self->stopping_))break;
        unsigned char port[2];if(!receiveExact(fd,port,2,8,self->stopping_))break;
        const TrojanNode node=self->node_;
        const bool vmess=node.protocol==TrojanNode::Protocol::Vmess;
        const bool anytls=node.protocol==TrojanNode::Protocol::Anytls;
        const bool shadowsocks=node.protocol==TrojanNode::Protocol::Shadowsocks
            ||node.protocol==TrojanNode::Protocol::ShadowsocksR;
        // These requests determine whether the YouTube page and player can open.
        // Do not risk routing them through an idle session whose server-side
        // stream may have expired after the initial connectivity probe.
        std::string destinationHost;
        if(header[3]==3 && !target.empty() && target.size()>1)
            destinationHost.assign((const char*)target.data()+1,target.size()-1);
        freshYoutubeConnection=destinationHost=="www.youtube.com"
            ||destinationHost=="www.googleapis.com"
            ||destinationHost=="oauth2.googleapis.com"
            ||destinationHost=="googlevideo.com"
            ||(destinationHost.size()>16 && destinationHost.compare(
                destinationHost.size()-16,16,".googlevideo.com")==0);
        if(anytls&&!freshYoutubeConnection){
            std::vector<IdleSession*> stale;
            SDL_LockMutex(self->idleMutex_);
            while(!self->idleSessions_.empty()){
                IdleSession* idle=self->idleSessions_.back();self->idleSessions_.pop_back();
                if(SDL_GetTicks64()-idle->idleSince>30000||!idle->stream->reusable())stale.push_back(idle);
                else {curl=idle->curl;connectTo=idle->connectTo;any=std::move(idle->stream);
                    idle->curl=nullptr;idle->connectTo=nullptr;delete idle;break;}
            }
            SDL_UnlockMutex(self->idleMutex_);
            for(auto* idle:stale)delete idle;
        }
        bool reused=curl!=nullptr;
        if(!curl)curl=curl_easy_init();
        if(!curl)break;
        const bool plain=shadowsocks||!node.tls;
        const std::string host=plain?node.server:(node.sni.empty()?node.server:node.sni);
        const std::string address=(plain?"http://":"https://")+host+":"+std::to_string(node.port)+"/";
        const std::string mapping=host+":"+std::to_string(node.port)+":"+node.server+":"+std::to_string(node.port);
        if(!connectTo)connectTo=curl_slist_append(nullptr,mapping.c_str());
        const auto configure=[&]{
            curl_easy_setopt(curl,CURLOPT_URL,address.c_str());curl_easy_setopt(curl,CURLOPT_CONNECT_TO,connectTo);
            curl_easy_setopt(curl,CURLOPT_CONNECT_ONLY,1L);curl_easy_setopt(curl,CURLOPT_PROXY,"");
            curl_easy_setopt(curl,CURLOPT_CAINFO,"romfs:/cacert.pem");
            curl_easy_setopt(curl,CURLOPT_SSL_VERIFYPEER,node.allowInsecure?0L:1L);
            curl_easy_setopt(curl,CURLOPT_SSL_VERIFYHOST,node.allowInsecure?0L:2L);
            curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT,12L);curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);
            curl_easy_setopt(curl,CURLOPT_IPRESOLVE,CURL_IPRESOLVE_V4);
        };
        configure();
        if(!reused&&curl_easy_perform(curl)!=CURLE_OK)break;
        std::unique_ptr<ShadowsocksAead> ss;
        std::unique_ptr<ShadowsocksStream> ssr;
        std::unique_ptr<VmessAead> vm;
        if(node.protocol==TrojanNode::Protocol::Shadowsocks){
            ss=std::make_unique<ShadowsocksAead>(node.password,node.method);if(!ss->valid())break;
        }else if(node.protocol==TrojanNode::Protocol::ShadowsocksR){
            ssr=std::make_unique<ShadowsocksStream>(node.password,node.method);if(!ssr->valid())break;
        }else if(vmess){
            vm=std::make_unique<VmessAead>(node.password);if(!vm->valid())break;
        }else if(anytls&&!any){
            any=std::make_unique<AnytlsStream>();
        }
        std::string preface;
        if(shadowsocks){
            preface.push_back((char)header[3]);
            preface.append((char*)target.data(),target.size());
            preface.push_back((char)port[0]);preface.push_back((char)port[1]);
        }else if(node.protocol==TrojanNode::Protocol::Trojan){
            preface=passwordDigest(node.password);if(preface.empty())break;
            preface+="\r\n";preface.push_back(1);preface.push_back((char)header[3]);
            preface.append((char*)target.data(),target.size());
            preface.push_back((char)port[0]);preface.push_back((char)port[1]);preface+="\r\n";
        }else if(node.protocol==TrojanNode::Protocol::Vless){
            preface.push_back(0); // Xray VLESS wire version.
            int high=-1;
            for(unsigned char c:node.password){
                if(c=='-')continue;
                const int nibble=c<='9'?c-'0':std::tolower(c)-'a'+10;
                if(high<0)high=nibble;
                else{preface.push_back((char)((high<<4)|nibble));high=-1;}
            }
            if(preface.size()!=17)break;
            preface.push_back(0); // No addons.
            preface.push_back(1); // TCP.
            preface.push_back((char)port[0]);preface.push_back((char)port[1]);
            const unsigned char type=header[3]==3?2:header[3]==4?3:1;
            preface.push_back((char)type);
            preface.append((char*)target.data(),target.size());
        }
        if(any){
            std::vector<unsigned char> destination={header[3]};
            destination.insert(destination.end(),target.begin(),target.end());
            destination.push_back(port[0]);destination.push_back(port[1]);
            bool opened=false;
            if(reused)opened=any->open(curl,destination,self->stopping_);
            if(reused&&!opened&&!self->stopping_.load()){
                curl_easy_cleanup(curl);curl=nullptr;
                curl_slist_free_all(connectTo);connectTo=nullptr;
                any=std::make_unique<AnytlsStream>();
                curl=curl_easy_init();
                if(!curl)break;
                connectTo=curl_slist_append(nullptr,mapping.c_str());
                if(!connectTo)break;
                configure();
                if(curl_easy_perform(curl)!=CURLE_OK)break;
                reused=false;
            }
            if(!reused){
                std::string scheme;
                SDL_LockMutex(self->idleMutex_);scheme=self->paddingScheme_;SDL_UnlockMutex(self->idleMutex_);
                opened=any->start(curl,node.password,destination,self->stopping_,scheme);
            }
            if(!opened)break;
            std::string updated=any->takeUpdatedScheme();
            if(!updated.empty()){
                SDL_LockMutex(self->idleMutex_);self->paddingScheme_=std::move(updated);
                SDL_UnlockMutex(self->idleMutex_);
            }
            streamActive=true;connectionHealthy=true;
        }
        const unsigned char success[]={5,0,0,1,0,0,0,0,0,0};
        if(!sendLocal(fd,success,sizeof(success),self->stopping_))break;
        unsigned char buffer[32768];
        bool started=anytls;
        int vlessResponsePrefix=node.protocol==TrojanNode::Protocol::Vless?-2:0;
        while(!self->stopping_.load()){
            fd_set local;FD_ZERO(&local);FD_SET(fd,&local);timeval wait{0,20000};
            const int ready=select(fd+1,&local,nullptr,nullptr,&wait);
            if(ready<0){connectionHealthy=false;break;}
            if(ready>0){
                const ssize_t n=recv(fd,buffer,sizeof(buffer),0);if(n<=0)break;
                if(!started){
                    std::vector<unsigned char> encoded;
                    bool ok=true;
                    if(vm){
                        std::vector<unsigned char> destination={port[0],port[1],
                            (unsigned char)(header[3]==3?2:header[3]==4?3:1)};
                        destination.insert(destination.end(),target.begin(),target.end());
                        std::vector<unsigned char> body;
                        ok=vm->begin(destination.data(),destination.size(),encoded)
                            &&vm->encode(buffer,(size_t)n,body);
                        if(ok)encoded.insert(encoded.end(),body.begin(),body.end());
                    }else{
                        preface.append((const char*)buffer,(size_t)n);
                        ok=ss?ss->encode((const unsigned char*)preface.data(),preface.size(),encoded)
                            :ssr?ssr->encode((const unsigned char*)preface.data(),preface.size(),encoded):true;
                    }
                    const unsigned char* sendData=shadowsocks||vmess?encoded.data():(const unsigned char*)preface.data();
                    const size_t sendSize=shadowsocks||vmess?encoded.size():preface.size();
                    if(!ok||!sendTls(curl,sendData,sendSize,self->stopping_))break;
                    preface.clear();started=true;
                }else{
                    std::vector<unsigned char> encoded;
                    const bool ok=ss?ss->encode(buffer,(size_t)n,encoded)
                        :ssr?ssr->encode(buffer,(size_t)n,encoded)
                        :vm?vm->encode(buffer,(size_t)n,encoded)
                        :true;
                    if(!ok||(any?!any->sendData(curl,buffer,(size_t)n,self->stopping_)
                        :!sendTls(curl,shadowsocks||vmess?encoded.data():buffer,
                            shadowsocks||vmess?encoded.size():(size_t)n,self->stopping_))){connectionHealthy=false;break;}
                }
            }
            if(!started)continue;
            size_t n=0;const CURLcode result=curl_easy_recv(curl,buffer,sizeof(buffer),&n);
            if(result==CURLE_OK){
                if(n==0){connectionHealthy=false;break;}
                size_t offset=0;
                if(vlessResponsePrefix==-2){
                    if(buffer[offset++]!=0)break;
                    vlessResponsePrefix=-1;
                }
                if(vlessResponsePrefix==-1&&offset<n)
                    vlessResponsePrefix=buffer[offset++];
                if(vlessResponsePrefix>0){
                    const size_t skipped=std::min<size_t>(n-offset,vlessResponsePrefix);
                    offset+=skipped;vlessResponsePrefix-=(int)skipped;
                }
                if(shadowsocks||vmess||anytls){
                    std::vector<unsigned char> decoded,reply;
                    if(!(ss?ss->decode(buffer+offset,n-offset,decoded)
                        :ssr?ssr->decode(buffer+offset,n-offset,decoded)
                        :vm?vm->decode(buffer+offset,n-offset,decoded)
                        :any->decode(buffer+offset,n-offset,decoded,reply))){connectionHealthy=false;break;}
                    if(any){
                        std::string updated=any->takeUpdatedScheme();
                        if(!updated.empty()){
                            SDL_LockMutex(self->idleMutex_);self->paddingScheme_=std::move(updated);
                            SDL_UnlockMutex(self->idleMutex_);
                        }
                    }
                    if(!reply.empty()&&!sendTls(curl,reply.data(),reply.size(),self->stopping_)){connectionHealthy=false;break;}
                    if(!decoded.empty()&&!sendLocal(fd,decoded.data(),decoded.size(),self->stopping_))break;
                    if(any&&any->closed())break;
                }else if(offset<n&&!sendLocal(fd,buffer+offset,n-offset,self->stopping_))break;
            }
            else if(result!=CURLE_AGAIN){connectionHealthy=false;break;}
        }
    } while(false);
    if(any&&self->idleMutex_){
        std::string updated=any->takeUpdatedScheme();
        if(!updated.empty()){
            SDL_LockMutex(self->idleMutex_);self->paddingScheme_=std::move(updated);
            SDL_UnlockMutex(self->idleMutex_);
        }
        if(streamActive&&connectionHealthy&&!freshYoutubeConnection
            &&!self->stopping_.load()&&any->reusable()
            &&any->finish(curl,self->stopping_)){
            auto* idle=new IdleSession;
            idle->curl=curl;idle->connectTo=connectTo;idle->stream=std::move(any);
            idle->idleSince=SDL_GetTicks64();
            SDL_LockMutex(self->idleMutex_);
            if(self->idleSessions_.size()<8&&!self->stopping_.load()){
                self->idleSessions_.push_back(idle);curl=nullptr;connectTo=nullptr;
            }else {idle->curl=nullptr;idle->connectTo=nullptr;delete idle;}
            SDL_UnlockMutex(self->idleMutex_);
        }
    }
    if(connectTo)curl_slist_free_all(connectTo);
    if(curl)curl_easy_cleanup(curl);
    if(fd>=0){shutdown(fd,SHUT_RDWR);close(fd);}client->socket.store(-1);client->done.store(true);return 0;
}
}
