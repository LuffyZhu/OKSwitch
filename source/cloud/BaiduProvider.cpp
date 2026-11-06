#include "okswitch/cloud/BaiduProvider.hpp"
#include "okswitch/net/HttpClient.hpp"
#include <jansson.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <chrono>
#include <ctime>
#include <map>
#include <sstream>

namespace okswitch::cloud {
namespace {
constexpr const char* browserUa="com.android.chrome/131.0.6778.200 (Linux;Android 10) AndroidXMedia3/1.5.1";
constexpr const char* netdiskUa="netdisk;P2SP;2.2.91.136;android-android;";
constexpr const char* mediaUa="com.android.chrome/131.0.6778.200 (Linux;Android 10) AndroidXMedia3/1.5.1";
constexpr const char* androidDeviceId="BB91C9B818963851F99A99261A70E37E|VUFQKX5JL";
constexpr const char* mediaDeviceId="881PVE4CGFCOUAU94P30TGCPNUB6HQP2|U9DL1IZZD";
constexpr const char* androidVersion="11.10.4";
constexpr const char* androidCertMd5="ae5821440fab5e1a61a025f014bd8972";
constexpr const char* sharedownloadKey="B8ec24caf34ef7227c66767d29ffd3fb";
using Cookies=std::map<std::string,std::string>;

std::uint32_t rotateLeft(std::uint32_t value,unsigned count){return (value<<count)|(value>>(32-count));}
std::array<unsigned char,20> sha1(const std::string& input){
    std::vector<unsigned char> bytes(input.begin(),input.end());const std::uint64_t bits=(std::uint64_t)bytes.size()*8;
    bytes.push_back(0x80);while(bytes.size()%64!=56)bytes.push_back(0);
    for(int shift=56;shift>=0;shift-=8)bytes.push_back((unsigned char)(bits>>shift));
    std::uint32_t h0=0x67452301,h1=0xefcdab89,h2=0x98badcfe,h3=0x10325476,h4=0xc3d2e1f0;
    for(size_t offset=0;offset<bytes.size();offset+=64){std::uint32_t w[80]{};
        for(int i=0;i<16;++i){const size_t p=offset+(size_t)i*4;w[i]=((std::uint32_t)bytes[p]<<24)|((std::uint32_t)bytes[p+1]<<16)|((std::uint32_t)bytes[p+2]<<8)|bytes[p+3];}
        for(int i=16;i<80;++i)w[i]=rotateLeft(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
        std::uint32_t a=h0,b=h1,c=h2,d=h3,e=h4;
        for(int i=0;i<80;++i){std::uint32_t f=0,k=0;if(i<20){f=(b&c)|((~b)&d);k=0x5a827999;}else if(i<40){f=b^c^d;k=0x6ed9eba1;}else if(i<60){f=(b&c)|(b&d)|(c&d);k=0x8f1bbcdc;}else{f=b^c^d;k=0xca62c1d6;}const std::uint32_t temp=rotateLeft(a,5)+f+e+k+w[i];e=d;d=c;c=rotateLeft(b,30);b=a;a=temp;}
        h0+=a;h1+=b;h2+=c;h3+=d;h4+=e;
    }
    std::array<unsigned char,20> out{};const std::uint32_t words[]={h0,h1,h2,h3,h4};for(int i=0;i<5;++i)for(int j=0;j<4;++j)out[(size_t)i*4+j]=(unsigned char)(words[i]>>(24-j*8));return out;
}
std::string hexLower(const unsigned char* bytes,size_t count){static const char* hex="0123456789abcdef";std::string out;out.reserve(count*2);for(size_t i=0;i<count;++i){out+=hex[bytes[i]>>4];out+=hex[bytes[i]&15];}return out;}
std::string hmacSha1Hex(const std::string& key,const std::string& data){std::array<unsigned char,64> actual{};if(key.size()>64){auto digest=sha1(key);std::copy(digest.begin(),digest.end(),actual.begin());}else std::copy(key.begin(),key.end(),actual.begin());std::string inner(64,'\0'),outer(64,'\0');for(size_t i=0;i<64;++i){inner[i]=(char)(actual[i]^0x36);outer[i]=(char)(actual[i]^0x5c);}inner+=data;const auto innerHash=sha1(inner);outer.append((const char*)innerHash.data(),innerHash.size());const auto digest=sha1(outer);return hexLower(digest.data(),digest.size());}
bool base64Decode(const std::string& input,std::vector<unsigned char>& output){auto value=[](unsigned char c)->int{if(c>='A'&&c<='Z')return c-'A';if(c>='a'&&c<='z')return c-'a'+26;if(c>='0'&&c<='9')return c-'0'+52;if(c=='+')return 62;if(c=='/')return 63;return -1;};output.clear();unsigned bits=0;int count=0;for(unsigned char c:input){if(c=='=')break;if(std::isspace(c))continue;int v=value(c);if(v<0)return false;bits=(bits<<6)|(unsigned)v;count+=6;if(count>=8){count-=8;output.push_back((unsigned char)(bits>>count));bits&=(1u<<count)-1;}}return !output.empty();}
bool decryptSk(const std::string& encrypted,const std::string& uid,std::string& output){std::vector<unsigned char> data;if(uid.empty()||!base64Decode(encrypted,data))return false;std::array<unsigned char,256> s{};for(size_t i=0;i<s.size();++i)s[i]=(unsigned char)i;unsigned j=0;for(unsigned i=0;i<256;++i){j=(j+s[i]+(unsigned char)uid[(size_t)i%uid.size()])&255;std::swap(s[i],s[j]);}unsigned i=0;j=0;output.resize(data.size());for(size_t p=0;p<data.size();++p){i=(i+1)&255;j=(j+s[i])&255;std::swap(s[i],s[j]);output[p]=(char)(data[p]^s[(s[i]+s[j])&255]);}return true;}

std::string encode(const std::string& value){static const char* hex="0123456789ABCDEF";std::string out;for(unsigned char c:value){if(std::isalnum(c)||c=='-'||c=='_'||c=='.'||c=='~')out+=c;else{out+='%';out+=hex[c>>4];out+=hex[c&15];}}return out;}
int hexValue(char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;}
std::string decodePercent(const std::string& value){std::string out;for(size_t i=0;i<value.size();++i){if(value[i]=='%'&&i+2<value.size()){int a=hexValue(value[i+1]),b=hexValue(value[i+2]);if(a>=0&&b>=0){out+=(char)((a<<4)|b);i+=2;continue;}}out+=value[i]=='+'?' ':value[i];}return out;}
std::string queryValue(const std::string& url,const std::string& key){size_t q=url.find('?');if(q==std::string::npos)return {};for(size_t p=q+1;p<url.size();){size_t e=url.find('&',p);std::string item=url.substr(p,e==std::string::npos?std::string::npos:e-p);size_t eq=item.find('=');if(item.substr(0,eq)==key)return decodePercent(eq==std::string::npos?"":item.substr(eq+1));if(e==std::string::npos)break;p=e+1;}return {};}
bool parseShare(const std::string& url,std::string& surl,std::string& pwd){size_t scheme=url.find("://");if(scheme==std::string::npos)return false;size_t hostStart=scheme+3,hostEnd=url.find_first_of("/?#",hostStart);std::string host=url.substr(hostStart,hostEnd-hostStart);for(char& c:host)c=(char)std::tolower((unsigned char)c);if(host!="pan.baidu.com")return false;size_t marker=url.find("/s/",hostEnd);if(marker!=std::string::npos){size_t start=marker+3,end=url.find_first_of("/?#",start);surl=url.substr(start,end-start);if(!surl.empty()&&surl.front()=='1')surl.erase(0,1);}else surl=queryValue(url,"surl");pwd=queryValue(url,"pwd");return !surl.empty();}
void parseCookie(const std::string& value,Cookies& cookies){size_t p=0;while(p<value.size()){size_t e=value.find(';',p);std::string part=value.substr(p,e==std::string::npos?std::string::npos:e-p);while(!part.empty()&&std::isspace((unsigned char)part.front()))part.erase(0,1);size_t eq=part.find('=');if(eq!=std::string::npos&&!part.substr(0,eq).empty())cookies[part.substr(0,eq)]=part.substr(eq+1);if(e==std::string::npos)break;p=e+1;}}
void collect(const net::HttpResponse& response,Cookies& cookies){for(const auto& header:response.headers){std::string name=header.name;for(char& c:name)c=(char)std::tolower((unsigned char)c);if(name!="set-cookie")continue;size_t e=header.value.find(';'),eq=header.value.find('=');if(eq==std::string::npos||eq>e)continue;std::string key=header.value.substr(0,eq),value=header.value.substr(eq+1,e==std::string::npos?std::string::npos:e-eq-1);if(!key.empty()&&!value.empty())cookies[key]=std::move(value);}}
std::string headerValue(const net::HttpResponse& response,const std::string& wanted){for(const auto& header:response.headers){std::string name=header.name;for(char& c:name)c=(char)std::tolower((unsigned char)c);if(name==wanted)return header.value;}return {};}
std::string join(const Cookies& cookies){std::string value;for(const auto& p:cookies){if(!value.empty())value+="; ";value+=p.first+"="+p.second;}return value;}
std::string jsonString(json_t* object,const char* key){json_t* value=json_is_object(object)?json_object_get(object,key):nullptr;return json_is_string(value)?json_string_value(value):"";}
long long jsonInteger(json_t* object,const char* key,long long fallback=0){json_t* value=json_is_object(object)?json_object_get(object,key):nullptr;if(json_is_integer(value))return json_integer_value(value);if(json_is_string(value)){const char* s=json_string_value(value);char* end=nullptr;long long n=std::strtoll(s,&end,10);if(end&&end!=s)return n;}return fallback;}
json_t* parseJson(const net::HttpResponse& response){json_error_t error{};return response.body.empty()?nullptr:json_loadb(response.body.data(),response.body.size(),0,&error);}
std::string findStringKey(json_t* value,const char* wanted){
    if(json_is_array(value)){for(size_t i=0;i<json_array_size(value);++i){std::string found=findStringKey(json_array_get(value,i),wanted);if(!found.empty())return found;}return {};}
    if(!json_is_object(value))return {};
    json_t* direct=json_object_get(value,wanted);if(json_is_string(direct))return json_string_value(direct);
    const char* key=nullptr;json_t* child=nullptr;json_object_foreach(value,key,child){if(json_is_array(child)||json_is_object(child)){std::string found=findStringKey(child,wanted);if(!found.empty())return found;}}
    return {};
}
std::string findScalarKey(json_t* value,const char* wanted){if(json_is_array(value)){for(size_t i=0;i<json_array_size(value);++i){std::string found=findScalarKey(json_array_get(value,i),wanted);if(!found.empty())return found;}return {};}if(!json_is_object(value))return {};json_t* direct=json_object_get(value,wanted);if(json_is_string(direct))return json_string_value(direct);if(json_is_integer(direct))return std::to_string(json_integer_value(direct));const char* key=nullptr;json_t* child=nullptr;json_object_foreach(value,key,child){if(json_is_array(child)||json_is_object(child)){std::string found=findScalarKey(child,wanted);if(!found.empty())return found;}}return {};}
std::string findPcsUrl(json_t* value){
    if(json_is_string(value)){std::string text=json_string_value(value);if(text.rfind("http",0)!=0)text=decodePercent(text);if(text.rfind("http",0)==0&&(text.find("d.pcs.baidu.com")!=std::string::npos||text.find("pcs.baidu.com")!=std::string::npos))return text;return {};}
    if(json_is_array(value)){for(size_t i=0;i<json_array_size(value);++i){std::string found=findPcsUrl(json_array_get(value,i));if(!found.empty())return found;}return {};}
    if(!json_is_object(value))return {};
    const char* key=nullptr;json_t* child=nullptr;json_object_foreach(value,key,child){std::string found=findPcsUrl(child);if(!found.empty())return found;}
    return {};
}
void findDownloadUrl(json_t* value,int inheritedScore,std::string& best,int& bestScore){
    if(json_is_array(value)){for(size_t i=0;i<json_array_size(value);++i)findDownloadUrl(json_array_get(value,i),inheritedScore,best,bestScore);return;}
    if(!json_is_object(value))return;
    const char* key=nullptr;json_t* child=nullptr;json_object_foreach(value,key,child){
        std::string lower=key?key:"";for(char& c:lower)c=(char)std::tolower((unsigned char)c);
        int score=inheritedScore;
        if(lower=="dlink"||lower=="download_url"||lower=="download_link")score+=100;
        else if(lower.find("download")!=std::string::npos||lower=="play_url"||lower=="url")score+=55;
        if(lower.find("thumb")!=std::string::npos||lower.find("image")!=std::string::npos||lower.find("photo")!=std::string::npos)score-=100;
        if(json_is_string(child)){
            std::string text=json_string_value(child);if(text.rfind("http",0)!=0)text=decodePercent(text);
            if(text.rfind("http://",0)==0||text.rfind("https://",0)==0){
                if(text.find("pcs.baidu.com")!=std::string::npos)score+=100;
                if(text.find(".jpg")!=std::string::npos||text.find(".png")!=std::string::npos)score-=100;
                if(score>bestScore){bestScore=score;best=std::move(text);}
            }
        }else findDownloadUrl(child,score,best,bestScore);
    }
}
std::string findPcsUrlText(const std::string& body){
    size_t start=body.find("https:");if(start==std::string::npos)start=body.find("http:");
    while(start!=std::string::npos){
        size_t end=start;bool escaped=false;
        while(end<body.size()){
            char c=body[end];if(!escaped&&(c=='\"'||c=='\''||std::isspace((unsigned char)c)))break;
            if(!escaped&&c=='\\'){escaped=true;++end;continue;}escaped=false;++end;
        }
        std::string value=body.substr(start,end-start),decoded;
        for(size_t i=0;i<value.size();++i){
            if(value[i]=='\\'&&i+1<value.size()&&value[i+1]=='/'){decoded+='/';++i;}
            else if(value.compare(i,6,"\\u0026")==0){decoded+='&';i+=5;}
            else if(value.compare(i,6,"\\u003d")==0){decoded+='=';i+=5;}
            else decoded+=value[i];
        }
        for(size_t p=0;(p=decoded.find("&amp;",p))!=std::string::npos;)decoded.replace(p,5,"&");
        if(decoded.find("d.pcs.baidu.com")!=std::string::npos||decoded.find("pcs.baidu.com")!=std::string::npos)return decoded;
        start=body.find("http",end);
    }
    return {};
}
std::string pcsUrlFromThumbnail(const std::string& thumbnail){
    const std::string marker="/thumbnail/";const size_t markerAt=thumbnail.find(marker);
    if(markerAt==std::string::npos)return {};
    const size_t idStart=markerAt+marker.size(),query=thumbnail.find('?',idStart);
    if(query==std::string::npos||query==idStart)return {};
    const std::string fileKey=thumbnail.substr(idStart,query-idStart);
    if(fileKey.empty())return {};
    return "https://d.pcs.baidu.com/file/"+fileKey+thumbnail.substr(query);
}
std::string numberAfter(const std::string& html,const std::vector<std::string>& markers){for(const auto& marker:markers){size_t p=html.find(marker);if(p==std::string::npos)continue;p+=marker.size();while(p<html.size()&&(html[p]=='"'||html[p]==' '))++p;size_t e=p;while(e<html.size()&&std::isdigit((unsigned char)html[e]))++e;if(e>p)return html.substr(p,e-p);}return {};}
std::string form(const std::vector<std::pair<std::string,std::string>>& values){std::string body;for(const auto& p:values){if(!body.empty())body+='&';body+=encode(p.first)+'='+encode(p.second);}return body;}
std::string jsonArrayOne(const std::string& value){json_t* array=json_array();json_array_append_new(array,json_string(value.c_str()));char* text=json_dumps(array,JSON_COMPACT);std::string result=text?text:"[]";free(text);json_decref(array);return result;}
bool parseHls(const std::string& body,std::vector<std::string>& urls,std::vector<std::uint64_t>& sizes,std::vector<double>& durations,double& duration){
    urls.clear();sizes.clear();durations.clear();duration=0;double pendingDuration=0;if(body.rfind("#EXTM3U",0)!=0)return false;std::istringstream stream(body);std::string line;
    while(std::getline(stream,line)){if(!line.empty()&&line.back()=='\r')line.pop_back();if(line.rfind("#EXTINF:",0)==0){char* end=nullptr;pendingDuration=std::strtod(line.c_str()+8,&end);if(end==line.c_str()+8||pendingDuration<=0)pendingDuration=0;continue;}if(line.rfind("https://",0)!=0)continue;const std::string length=queryValue(line,"len");char* end=nullptr;const unsigned long long bytes=std::strtoull(length.c_str(),&end,10);if(length.empty()||!end||*end!='\0'||bytes==0||pendingDuration<=0){urls.clear();sizes.clear();durations.clear();return false;}urls.push_back(line);sizes.push_back((std::uint64_t)bytes);durations.push_back(pendingDuration);duration+=pendingDuration;pendingDuration=0;}
    return urls.size()>=2&&urls.size()==sizes.size()&&urls.size()==durations.size();
}
ProviderError error(const net::HttpResponse& response,const std::string& fallback){ProviderError e;e.code=response.succeeded?ProviderErrorCode::InvalidResponse:ProviderErrorCode::NetworkError;json_t* root=parseJson(response);long long code=jsonInteger(root,"errno",jsonInteger(root,"error_code"));std::string message=jsonString(root,"show_msg");if(message.empty())message=jsonString(root,"err_msg");if(root)json_decref(root);if(code==-9)message="百度网盘提取码错误";else if(code==-12)message="百度分享验证已过期，请重试";const bool credentialFailure=code==2||code==-6||message.find("登录状态")!=std::string::npos;if(credentialFailure){e.code=ProviderErrorCode::AuthenticationRequired;if(message.empty())message="百度登录状态无效，请重新扫码";}e.message=message.empty()?(response.error.empty()?fallback:response.error):message;return e;}
net::HttpResponse send(const net::HttpClient& http,const std::string& url,const std::string& method,const std::string& body,Cookies& cookies,const std::string& referer="",bool close=true,const std::atomic<bool>* cancel=nullptr,bool follow=true,bool ajax=true,const char* userAgent=browserUa){net::HttpRequest request;request.url=url;request.method=method;request.body=body;request.closeConnection=close;request.connectTimeoutSeconds=cancel?4:10;request.timeoutSeconds=cancel?8:20;request.cancel=cancel;request.followRedirects=follow;request.headers.push_back({"User-Agent",userAgent});request.headers.push_back({"Accept","application/json, text/plain, */*"});if(!referer.empty())request.headers.push_back({"Referer",referer});if(!cookies.empty())request.headers.push_back({"Cookie",join(cookies)});if(method=="POST"){request.headers.push_back({"Origin","https://pan.baidu.com"});request.headers.push_back({"Content-Type","application/x-www-form-urlencoded"});if(ajax)request.headers.push_back({"X-Requested-With","XMLHttpRequest"});}auto response=http.get(request);collect(response,cookies);return response;}
std::string packState(const std::string& surl,const std::string& uk,const std::string& sekey,const std::string& cookie){json_t* root=json_pack("{s:s,s:s,s:s,s:s}","surl",surl.c_str(),"uk",uk.c_str(),"sekey",sekey.c_str(),"cookie",cookie.c_str());char* text=json_dumps(root,JSON_COMPACT);std::string result=text?text:"";free(text);json_decref(root);return result;}
bool unpackState(const std::string& state,std::string& surl,std::string& uk,std::string& sekey,std::string& cookie){json_error_t e{};json_t* root=json_loadb(state.data(),state.size(),0,&e);if(!root)return false;surl=jsonString(root,"surl");uk=jsonString(root,"uk");sekey=jsonString(root,"sekey");cookie=jsonString(root,"cookie");json_decref(root);return !surl.empty()&&!uk.empty();}
std::string base64(const std::vector<unsigned char>& data){static const char table[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";std::string out;for(size_t i=0;i<data.size();i+=3){unsigned v=(unsigned)data[i]<<16;if(i+1<data.size())v|=(unsigned)data[i+1]<<8;if(i+2<data.size())v|=data[i+2];out+=table[(v>>18)&63];out+=table[(v>>12)&63];out+=i+1<data.size()?table[(v>>6)&63]:'=';out+=i+2<data.size()?table[v&63]:'=';}return out;}
std::string baiduSign(const std::string& sign3,const std::string& sign1){if(sign3.empty())return {};unsigned char s[256],key[256];for(int i=0;i<256;++i){s[i]=(unsigned char)i;key[i]=(unsigned char)sign3[(size_t)i%sign3.size()];}unsigned j=0;for(int i=0;i<256;++i){j=(j+s[i]+key[i])&255;std::swap(s[i],s[j]);}std::vector<unsigned char> out(sign1.size());unsigned i=0;j=0;for(size_t q=0;q<sign1.size();++q){i=(i+1)&255;j=(j+s[i])&255;std::swap(s[i],s[j]);out[q]=(unsigned char)sign1[q]^s[(s[i]+s[j])&255];}return base64(out);}
}

BaiduProvider::BaiduProvider(const net::HttpClient& http):httpClient_(http){}
void BaiduProvider::setSession(QuarkSession session){if(session.cookie!=session_.cookie){accountUid_.clear();encryptedSk_.clear();bdstoken_.clear();}session_=std::move(session);}
bool BaiduProvider::isAuthenticated()const{return session_.hasCredentials();}
void BaiduProvider::resetBrowseCancel(){browseCancel_.store(false);}
void BaiduProvider::cancelBrowse(){browseCancel_.store(true);}
void BaiduProvider::resetPlaybackCancel(){playbackCancel_.store(false);}
void BaiduProvider::cancelPlayback(){playbackCancel_.store(true);}
const char* BaiduProvider::name()const{return "Baidu";}
bool BaiduProvider::canHandle(const ShareReference& share)const{std::string surl,pwd;return parseShare(share.url,surl,pwd);}

ProviderResult<ShareSession> BaiduProvider::resolveShare(const ShareReference& share){ProviderResult<ShareSession> result;std::string surl,pwd;if(!parseShare(share.url,surl,pwd)){result.error.code=ProviderErrorCode::InvalidShareLink;result.error.message="无效的百度网盘分享链接";return result;}if(pwd.empty())pwd=share.accessCode;Cookies cookies;parseCookie(session_.cookie,cookies);const std::string page="https://pan.baidu.com/s/1"+surl;auto response=send(httpClient_,share.url,"GET","",cookies,"",true,&browseCancel_,false);for(int redirect=0;redirect<3&&response.statusCode>=300&&response.statusCode<400;++redirect){std::string location=headerValue(response,"location");if(location.empty())break;if(location.rfind("//",0)==0)location="https:"+location;else if(location.rfind("/",0)==0)location="https://pan.baidu.com"+location;else if(location.rfind("http",0)!=0)break;response=send(httpClient_,location,"GET","",cookies,page,true,&browseCancel_,false);}if(!response.succeeded){result.error=error(response,"百度分享页加载失败");return result;}std::string shareId=numberAfter(response.body,{"shareid:\"","\"shareid\":"}),uk=numberAfter(response.body,{"share_uk:\"","\"share_uk\":"});if(shareId.empty()||uk.empty()){result.error.code=ProviderErrorCode::InvalidResponse;result.error.message=response.body.find("platform-non-found")!=std::string::npos?"百度分享已失效":"无法读取百度分享信息";return result;}std::string sekey;if(!pwd.empty()){
        // The Android flow visits the disk page before share verification. It
        // refreshes PANPSC and csrfToken, both of which Baidu ties to captcha
        // verification state.
        send(httpClient_,"https://pan.baidu.com/disk/main","GET","",cookies,
             "https://pan.baidu.com/",true,&browseCancel_);
        long long verifyErrno=-1;
        auto verify=[&](){
            const long long now=(long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()*1000;
            const std::string url="https://pan.baidu.com/share/verify?t="+std::to_string(now)+"&surl="+encode(surl);
            response=send(httpClient_,url,"POST",form({{"pwd",pwd}}),cookies,
                          "https://pan.baidu.com/",true,&browseCancel_,true,false);
            json_t* root=parseJson(response);
            verifyErrno=jsonInteger(root,"errno",-1);
            if(response.succeeded&&verifyErrno==0)sekey=decodePercent(jsonString(root,"randsk"));
            if(root)json_decref(root);
            if(sekey.empty()){auto bdclnd=cookies.find("BDCLND");if(bdclnd!=cookies.end())sekey=decodePercent(bdclnd->second);}
            return response.succeeded&&verifyErrno==0&&!sekey.empty();
        };
        if(!verify()&&verifyErrno==-62&&!browseCancel_.load()){
            // One fresh disk-page handshake clears a stale captcha signature;
            // do not loop and accidentally trigger Baidu's abuse controls.
            send(httpClient_,"https://pan.baidu.com/disk/main","GET","",cookies,
                 "https://pan.baidu.com/",true,&browseCancel_);
            verify();
        }
        if(verifyErrno!=0){result.error=error(response,"百度分享验证失败 E"+std::to_string(verifyErrno));return result;}
        if(sekey.empty()){result.error.code=ProviderErrorCode::InvalidResponse;result.error.message="百度分享验证成功但未返回访问密钥";return result;}}
    result.succeeded=true;result.value.shareId=shareId;result.value.rootId="/";result.value.opaqueState=packState(surl,uk,sekey,join(cookies));return result;}

ProviderResult<std::vector<CloudEntry>> BaiduProvider::listFiles(const ShareSession& session,const std::string& directoryId){ProviderResult<std::vector<CloudEntry>> result;std::string surl,uk,sekey,cookie;if(!unpackState(session.opaqueState,surl,uk,sekey,cookie)){result.error.code=ProviderErrorCode::InvalidResponse;result.error.message="百度分享会话不完整";return result;}Cookies cookies;parseCookie(cookie,cookies);const bool rootDir=directoryId.empty()||directoryId=="/";for(int page=1;;++page){std::ostringstream url;url<<"https://pan.baidu.com/share/list?shareid="<<encode(session.shareId)<<"&uk="<<encode(uk)<<"&sekey="<<encode(sekey)<<"&type=0&root="<<(rootDir?1:0)<<"&page="<<page<<"&num=100&order=other&desc=1&channel=chunlei&clienttype=0&web=1&app_id=250528";if(!rootDir)url<<"&dir="<<encode(directoryId);auto response=send(httpClient_,url.str(),"GET","",cookies,"https://pan.baidu.com/s/1"+surl,true,&browseCancel_);json_t* root=parseJson(response);json_t* list=root?json_object_get(root,"list"):nullptr;if(!response.succeeded||jsonInteger(root,"errno",-1)!=0||!json_is_array(list)){if(root)json_decref(root);result.error=error(response,"百度分享目录加载失败");return result;}size_t count=json_array_size(list);for(size_t i=0;i<count;++i){json_t* item=json_array_get(list,i);CloudEntry entry;entry.id=std::to_string(jsonInteger(item,"fs_id"));entry.name=jsonString(item,"server_filename");entry.shareToken=jsonString(item,"path");json_t* thumbs=json_object_get(item,"thumbs");entry.playbackHint=jsonString(thumbs,"url1");entry.parentId=directoryId;entry.type=jsonInteger(item,"isdir")?EntryType::Directory:EntryType::File;entry.size=(std::uint64_t)std::max<long long>(0,jsonInteger(item,"size"));if(!entry.id.empty()&&!entry.name.empty())result.value.push_back(std::move(entry));}json_decref(root);if(count<100)break;}result.succeeded=true;return result;}

ProviderResult<PlaybackSource> BaiduProvider::getPlaybackSource(const ShareSession& share,const CloudEntry& file){
    ProviderResult<PlaybackSource> result;
    if(!isAuthenticated()){result.error.code=ProviderErrorCode::AuthenticationRequired;result.error.message="请先扫码登录百度网盘";return result;}
    if(playbackCancel_.load()){result.error.code=ProviderErrorCode::NetworkError;result.error.message="已取消百度播放地址解析";return result;}
    std::string surl,uk,sekey,shareCookie;
    if(!unpackState(share.opaqueState,surl,uk,sekey,shareCookie)){result.error.code=ProviderErrorCode::AuthenticationRequired;result.error.message="百度分享会话不完整，请重新扫码";return result;}
    Cookies cookies;parseCookie(shareCookie,cookies);parseCookie(session_.cookie,cookies);
    auto fail=[&](const std::string& message){ProviderResult<PlaybackSource> failed;failed.error.code=ProviderErrorCode::InvalidResponse;failed.error.message=message;return failed;};
    net::HttpResponse response;
    if(bdstoken_.empty()){
        response=send(httpClient_,"https://pan.baidu.com/api/gettemplatevariable?channel=chunlei&web=1&app_id=250528&clienttype=0","POST","fields=%5B%22bdstoken%22%5D",cookies,"https://pan.baidu.com/",true,&playbackCancel_);
        json_t* root=parseJson(response);json_t* values=root?json_object_get(root,"result"):nullptr;bdstoken_=jsonString(values,"bdstoken");if(root)json_decref(root);
        if(!response.succeeded||bdstoken_.empty())return fail("百度转存令牌获取失败，请重新扫码");
    }

    constexpr const char* tempRoot="/OKSwitchTemp";
    const auto now=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const std::string createUrl="https://pan.baidu.com/api/create?a=commit&bdstoken="+encode(bdstoken_)+"&channel=chunlei&web=1&app_id=250528&clienttype=0";
    auto createDirectory=[&](const std::string& path){const std::string body=form({{"path",path},{"size","0"},{"isdir","1"},{"local_ctime",std::to_string(now)},{"local_mtime",std::to_string(now)},{"block_list","[]"},{"rtype","1"},{"wp_retry_num","3"}});auto created=send(httpClient_,createUrl,"POST",body,cookies,"https://pan.baidu.com/",true,&playbackCancel_);json_t* createdRoot=parseJson(created);const long long code=jsonInteger(createdRoot,"errno",-999);if(createdRoot)json_decref(createdRoot);return created.succeeded&&(code==0||code==-8);};
    if(!createDirectory(tempRoot))return fail("百度临时根目录创建失败");

    // Cleanup is already performed when a playback session ends. Scanning the
    // whole staging directory and querying quota before every episode added
    // several serial network round trips to the common successful path.
    long long requestErrno=0;json_t* root=nullptr;

    const std::string transferBody=form({{"path",tempRoot},{"fsidlist","["+file.id+"]"}});
    const std::string transferUrl="https://pan.baidu.com/share/transfer?shareid="+encode(share.shareId)+"&from="+encode(uk)+"&ondup=newcopy";
    response=send(httpClient_,transferUrl,"POST",transferBody,cookies,"https://pan.baidu.com/",true,&playbackCancel_);
    root=parseJson(response);requestErrno=jsonInteger(root,"errno",-999);json_t* extra=root?json_object_get(root,"extra"):nullptr;json_t* list=extra?json_object_get(extra,"list"):nullptr;json_t* first=json_is_array(list)&&json_array_size(list)?json_array_get(list,0):nullptr;std::string savedPath=jsonString(first,"to");std::string savedId=findScalarKey(first,"to_fs_id");
    json_t* transferInfo=root?json_object_get(root,"info"):nullptr;json_t* firstInfo=json_is_array(transferInfo)&&json_array_size(transferInfo)?json_array_get(transferInfo,0):nullptr;const long long itemErrno=jsonInteger(firstInfo,"errno",0);std::string transferMessage=jsonString(root,"show_msg");if(transferMessage.empty())transferMessage=jsonString(root,"errmsg");
    if(!response.succeeded||requestErrno!=0||savedPath.empty()||savedId.empty()){
        std::string detail="百度临时转存失败 E"+std::to_string(requestErrno);if(firstInfo)detail+=" / 子项 E"+std::to_string(itemErrno);if(!transferMessage.empty())detail+=" · "+transferMessage;if(response.error.size())detail+=" · "+response.error;SDL_Log("Baidu transfer failed: %s fileSize=%llu body=%s",detail.c_str(),(unsigned long long)file.size,response.body.substr(0,2048).c_str());if(root)json_decref(root);return fail(detail);
    }
    if(root)json_decref(root);

    // The reference APK prefers Baidu's adaptive HLS output to the original
    // dlink. Keep the Switch-friendly ceiling at 1080p, then fall back to 720p.
    const char* hlsTypes1080[]={"M3U8_AUTO_1080","M3U8_AUTO_720"};
    const char* hlsTypes720[]={"M3U8_AUTO_720","M3U8_FLV_264_480"};
    const char** hlsTypes=preferredMaxHeight_>=1080?hlsTypes1080:hlsTypes720;
    for(size_t hlsIndex=0;hlsIndex<2;++hlsIndex){const char* hlsType=hlsTypes[hlsIndex];
        const std::string streamingUrl="https://pan.baidu.com/api/streaming?type="+std::string(hlsType)+"&path="+encode(savedPath)+"&origin=dlna&check_blue=1&app_id=250528&devuid="+encode(mediaDeviceId)+"&clienttype=1&channel=android_12_V2238A_bd-netdisk_1024266g&network_type=wifi";
        auto playlistResponse=send(httpClient_,streamingUrl,"GET","",cookies,"https://pan.baidu.com/",true,&playbackCancel_,true,true,mediaUa);
        std::vector<std::string> segmentUrls;std::vector<std::uint64_t> segmentSizes;std::vector<double> segmentDurations;double hlsDuration=0;
        if(playlistResponse.succeeded&&parseHls(playlistResponse.body,segmentUrls,segmentSizes,segmentDurations,hlsDuration)){
            result.succeeded=true;result.value.url=segmentUrls.front();result.value.segmentUrls=std::move(segmentUrls);result.value.segmentSizes=std::move(segmentSizes);result.value.segmentDurations=std::move(segmentDurations);result.value.temporaryId=savedPath;result.value.durationSeconds=hlsDuration;result.value.parallelFetches=3;result.value.initialBufferSegments=3;result.value.headers.push_back({"User-Agent",mediaUa});result.value.headers.push_back({"Cookie",join(cookies)});
            SDL_Log("Baidu HLS type=%s segments=%zu duration=%.1f",hlsType,result.value.segmentUrls.size(),result.value.durationSeconds);
            return result;
        }
        if(playbackCancel_.load())break;
    }
    const std::string mediaUrl="https://pan.baidu.com/api/mediainfo?type=M3U8_FLV_264_480&path="+encode(savedPath)+"&origin=dlna&check_blue=1&app_id=250528&devuid="+encode(mediaDeviceId)+"&clienttype=80&channel=android_12_V2238A_bd-netdisk_1024266g&network_type=wifi&version=12.11.9";
    response=send(httpClient_,mediaUrl,"GET","",cookies,"https://pan.baidu.com/",true,&playbackCancel_,true,true,mediaUa);
    root=parseJson(response);requestErrno=jsonInteger(root,"errno",-999);json_t* info=root?json_object_get(root,"info"):nullptr;std::string dlink=jsonString(info,"dlink");const double duration=(double)jsonInteger(info,"duration",0);if(root)json_decref(root);
    if(dlink.empty()){
        PlaybackSource temporary;temporary.temporaryId=savedPath;std::string ignored;cleanupPlaybackSource(temporary,ignored);
        if(playbackCancel_.load())return fail("已取消百度播放地址解析");
        return fail("百度个人网盘直链获取失败 E"+std::to_string(requestErrno));
    }

    net::HttpRequest redirectRequest;redirectRequest.url=dlink;redirectRequest.method="GET";redirectRequest.followRedirects=false;redirectRequest.closeConnection=true;redirectRequest.connectTimeoutSeconds=5;redirectRequest.timeoutSeconds=8;redirectRequest.maxResponseBytes=64*1024;redirectRequest.cancel=&playbackCancel_;redirectRequest.headers.push_back({"User-Agent",mediaUa});redirectRequest.headers.push_back({"Cookie",join(cookies)});
    const auto redirect=httpClient_.get(redirectRequest);std::string location=headerValue(redirect,"location");if(location.rfind("//",0)==0)location="https:"+location;
    result.succeeded=true;result.value.url=location.rfind("http",0)==0?location:dlink;result.value.temporaryId=savedPath;result.value.durationSeconds=duration;result.value.parallelFetches=1;result.value.headers.push_back({"User-Agent",mediaUa});result.value.headers.push_back({"Cookie",join(cookies)});
    SDL_Log("Baidu personal dlink errno=%lld redirect=%ld resolved=%d",requestErrno,redirect.statusCode,location.rfind("http",0)==0?1:0);
    return result;
}
bool BaiduProvider::cleanupPlaybackSource(const PlaybackSource& source,std::string& cleanupError){
    cleanupError.clear();if(source.temporaryId.empty())return true;Cookies cookies;parseCookie(session_.cookie,cookies);
    if(bdstoken_.empty()){
        auto tokenResponse=send(httpClient_,"https://pan.baidu.com/api/gettemplatevariable?channel=chunlei&web=1&app_id=250528&clienttype=0","POST","fields=%5B%22bdstoken%22%5D",cookies,"https://pan.baidu.com/",true,nullptr);
        json_t* tokenRoot=parseJson(tokenResponse);json_t* values=tokenRoot?json_object_get(tokenRoot,"result"):nullptr;bdstoken_=jsonString(values,"bdstoken");if(tokenRoot)json_decref(tokenRoot);
        if(!tokenResponse.succeeded||bdstoken_.empty()){cleanupError="百度清理令牌获取失败";return false;}
    }
    const std::string body=form({{"filelist",jsonArrayOne(source.temporaryId)},{"async","0"},{"ondup","fail"},{"bdstoken",bdstoken_},{"wp_retry_num","3"}});
    const std::string deleteUrl="https://pan.baidu.com/api/filemanager?opera=delete&bdstoken="+encode(bdstoken_)+"&channel=chunlei&web=1&app_id=250528&clienttype=0";
    auto response=send(httpClient_,deleteUrl,"POST",body,cookies,"https://pan.baidu.com/",true,nullptr);
    json_t* root=parseJson(response);const long long requestErrno=jsonInteger(root,"errno",-999);if(root)json_decref(root);
    if(!response.succeeded||requestErrno!=0){cleanupError="百度临时文件删除失败 E"+std::to_string(requestErrno)+(response.error.empty()?std::string():" · "+response.error);return false;}
    return true;
}
}
