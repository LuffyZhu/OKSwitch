#include "okswitch/media/Guazi.hpp"

#include <array>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <sstream>
#include <vector>
#include <switch.h>
#include <jansson.h>
extern "C" {
#include <libavutil/aes.h>
#include <libavutil/base64.h>
#include <libavutil/md5.h>
#include <libavutil/mem.h>
}

namespace okswitch::media::guazi
{
namespace
{
constexpr const char* hosts[]={
    "https://sdapi.e2wu4ht.com", "https://api.w32z7vtd.com",
    "https://api.6a7nnf7.com", "https://api.umygrx3.com",
    "https://api.rmedphk.com"};
constexpr const char* signSecret="*&zvdvdvddbfikkkumtmdwqppp?|4Y!s!2br";
constexpr const char* oldDeviceKey="aLFBMWpxBrIDAD1Si/KVvm41";
constexpr const char* defaultReferer="http://WJiZxLXA2.com/";
constexpr const char* sessionPath="sdmc:/switch/OKSwitch/guazi-session.json";

struct Session { std::string token,id,deviceKey; bool loaded=false; size_t host=0; };
Session session;
std::mutex sessionMutex;

struct UInt1024 { std::array<std::uint32_t,32> w{}; };

int compare(const UInt1024& a,const UInt1024& b){
    for(int i=31;i>=0;--i)if(a.w[(size_t)i]!=b.w[(size_t)i])return a.w[(size_t)i]>b.w[(size_t)i]?1:-1;
    return 0;
}
UInt1024 addMod(const UInt1024& a,const UInt1024& b,const UInt1024& modulus){
    std::array<std::uint32_t,33> sum{};std::uint64_t carry=0;
    for(size_t i=0;i<32;++i){const std::uint64_t v=(std::uint64_t)a.w[i]+b.w[i]+carry;sum[i]=(std::uint32_t)v;carry=v>>32;}sum[32]=(std::uint32_t)carry;
    bool reduce=sum[32]!=0;
    if(!reduce){UInt1024 low;for(size_t i=0;i<32;++i)low.w[i]=sum[i];reduce=compare(low,modulus)>=0;}
    if(reduce){std::uint64_t borrow=0;for(size_t i=0;i<32;++i){const std::uint64_t sub=(std::uint64_t)modulus.w[i]+borrow;const std::uint64_t old=sum[i];sum[i]=(std::uint32_t)(old-sub);borrow=old<sub;}sum[32]=(std::uint32_t)((std::uint64_t)sum[32]-borrow);}
    UInt1024 out;for(size_t i=0;i<32;++i)out.w[i]=sum[i];return out;
}
UInt1024 multiplyMod(UInt1024 a,const UInt1024& b,const UInt1024& modulus){
    UInt1024 out;
    for(size_t bit=0;bit<1024;++bit){if((b.w[bit/32]>>(bit%32))&1u)out=addMod(out,a,modulus);a=addMod(a,a,modulus);}
    return out;
}
UInt1024 exponentiate(UInt1024 base,const UInt1024& exponent,const UInt1024& modulus){
    UInt1024 out;out.w[0]=1;
    for(size_t bit=0;bit<1024;++bit){if((exponent.w[bit/32]>>(bit%32))&1u)out=multiplyMod(out,base,modulus);base=multiplyMod(base,base,modulus);}
    return out;
}
int hexDigit(char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;}
UInt1024 fromHex(const char* hex){
    UInt1024 value;const size_t length=std::strlen(hex);size_t byte=0;
    for(size_t p=length;p>=2;p-=2,++byte){const int hi=hexDigit(hex[p-2]),lo=hexDigit(hex[p-1]);value.w[byte/4]|=(std::uint32_t)((hi<<4)|lo)<<((byte%4)*8);if(p==2)break;}
    return value;
}
UInt1024 fromBytes(const unsigned char* data,size_t count){UInt1024 value;for(size_t i=0;i<count&&i<128;++i){const size_t source=count-1-i;value.w[i/4]|=(std::uint32_t)data[source]<<((i%4)*8);}return value;}
std::array<unsigned char,128> toBytes(const UInt1024& value){std::array<unsigned char,128> out{};for(size_t i=0;i<128;++i)out[127-i]=(unsigned char)(value.w[i/4]>>((i%4)*8));return out;}

const UInt1024& requestModulus(){static const auto v=fromHex("d4339fbfcbcb0fb1691dd7f4504bae17db9f44530c455c51391e503ae4caabc673ecd09aa8491a23483cb9421c2f44e95a0fa4f04501ca318d8e019e929d079426c0a14c414847da94930aecdff31550cc63b2fe894ba39efe3b9c9722464e05660e1079e4469f5ec0f44906158ff4175ecc51e9ec11e44da42f9db20000f8c9");return v;}
const UInt1024& responseModulus(){static const auto v=fromHex("7ba84aad62e2d734268d34f5a336c4e1074578918dc6e6f195de86ac51b18a6c5f32c301e81a49869713a2e02acb0005a6988a7ad50105b5f062c614d7036beb8f175663e608c0b2e2b63cbdd9621676cc523d3ce8353a67efe85c1756537fdbd46d0337713dc142d14b070a653df08ff702235bec0a6de08f64794aa900f58d");return v;}
const UInt1024& responsePrivateExponent(){static const auto v=fromHex("0247fb80b1574ff305570b881087bd200d9b497b1deb726d387f8f6a74635b135eba3800bc006824d47aa7418d688b4a8f653700c7172abccd7f74fa03716bb73912a71657d1669555ebf1585f073719a359d778d757153eed436c9a87fa1db5d731faf44c48625dd6dff99396c377dc00bd87480db94c020fb088a299e4a71d");return v;}
const UInt1024& publicExponent(){static UInt1024 v=[](){UInt1024 x;x.w[0]=65537;return x;}();return v;}

void randomBytes(unsigned char* data,size_t count,bool nonzero=false){
    static std::uint64_t state=(std::uint64_t)std::chrono::high_resolution_clock::now().time_since_epoch().count()^0x9e3779b97f4a7c15ULL;
    for(size_t i=0;i<count;++i){state^=state<<13;state^=state>>7;state^=state<<17;unsigned char v=(unsigned char)state;if(nonzero&&v==0)v=1;data[i]=v;}
}
std::vector<unsigned char> rsaEncrypt(const std::string& plain){
    if(plain.size()>117)return {};std::array<unsigned char,128> encoded{};encoded[1]=2;const size_t padding=125-plain.size();randomBytes(encoded.data()+2,padding,true);encoded[2+padding]=0;std::memcpy(encoded.data()+3+padding,plain.data(),plain.size());
    const auto result=exponentiate(fromBytes(encoded.data(),encoded.size()),publicExponent(),requestModulus());const auto bytes=toBytes(result);return {bytes.begin(),bytes.end()};
}
std::string rsaDecryptBlocks(const std::vector<unsigned char>& encrypted){
    if(encrypted.empty()||encrypted.size()%128)return {};std::string out;
    for(size_t offset=0;offset<encrypted.size();offset+=128){const auto value=exponentiate(fromBytes(encrypted.data()+offset,128),responsePrivateExponent(),responseModulus());const auto block=toBytes(value);if(block[0]!=0||(block[1]!=1&&block[1]!=2))return {};size_t p=2;while(p<block.size()&&block[p]!=0)++p;if(++p>=block.size())return {};out.append((const char*)block.data()+p,block.size()-p);}
    return out;
}
std::string base64(const std::vector<unsigned char>& value){std::string out(((value.size()+2)/3)*4+1,'\0');av_base64_encode(out.data(),(int)out.size(),value.data(),(int)value.size());out.resize(std::strlen(out.c_str()));return out;}
std::vector<unsigned char> unbase64(const std::string& value){std::vector<unsigned char> out(value.size()*3/4+4);const int n=av_base64_decode(out.data(),value.c_str(),(int)out.size());if(n<0)return {};out.resize((size_t)n);return out;}
std::string hex(const std::vector<unsigned char>& value,bool upper=true){const char* digits=upper?"0123456789ABCDEF":"0123456789abcdef";std::string out;out.reserve(value.size()*2);for(unsigned char c:value){out+=digits[c>>4];out+=digits[c&15];}return out;}
std::vector<unsigned char> unhex(const std::string& value){if(value.size()%2)return {};std::vector<unsigned char> out(value.size()/2);for(size_t i=0;i<out.size();++i){int a=hexDigit(value[i*2]),b=hexDigit(value[i*2+1]);if(a<0||b<0)return {};out[i]=(unsigned char)((a<<4)|b);}return out;}
std::string md5Upper(const std::string& value){unsigned char digest[16];av_md5_sum(digest,(const unsigned char*)value.data(),value.size());std::vector<unsigned char> bytes(digest,digest+16);return hex(bytes,true);}
std::string randomText(size_t count){static const char alphabet[]="abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";std::string out(count,'0');std::vector<unsigned char> bytes(count);randomBytes(bytes.data(),bytes.size());for(size_t i=0;i<count;++i)out[i]=alphabet[bytes[i]%62];return out;}
std::string randomHex(size_t count){std::vector<unsigned char> bytes((count+1)/2);randomBytes(bytes.data(),bytes.size());std::string out=hex(bytes);out.resize(count);return out;}

std::vector<unsigned char> aes(const std::vector<unsigned char>& input,const std::string& key,const std::string& iv,bool decrypt){
    if(key.size()!=16||iv.size()!=16||input.empty()||input.size()%16)return {};AVAES* context=av_aes_alloc();if(!context)return {};av_aes_init(context,(const unsigned char*)key.data(),128,decrypt?1:0);std::vector<unsigned char> output(input.size());unsigned char actualIv[16];std::memcpy(actualIv,iv.data(),16);av_aes_crypt(context,output.data(),input.data(),(int)(input.size()/16),actualIv,decrypt?1:0);av_free(context);return output;
}
std::vector<unsigned char> encryptAes(const std::string& plain,const std::string& key,const std::string& iv){std::vector<unsigned char> padded(plain.begin(),plain.end());const unsigned char pad=(unsigned char)(16-padded.size()%16);padded.insert(padded.end(),pad,pad);return aes(padded,key,iv,false);}
std::string decryptAes(const std::vector<unsigned char>& encrypted,const std::string& key,const std::string& iv){auto plain=aes(encrypted,key,iv,true);if(plain.empty())return {};const unsigned pad=plain.back();if(pad==0||pad>16||pad>plain.size())return {};for(size_t i=plain.size()-pad;i<plain.size();++i)if(plain[i]!=pad)return {};return {(const char*)plain.data(),plain.size()-pad};}

std::string encode(const std::string& value){static const char* digits="0123456789ABCDEF";std::string out;for(unsigned char c:value){if(std::isalnum(c)||c=='-'||c=='_'||c=='.'||c=='~')out+=c;else{out+='%';out+=digits[c>>4];out+=digits[c&15];}}return out;}
std::string form(const std::vector<std::pair<std::string,std::string>>& fields){std::string out;for(const auto& field:fields){if(!out.empty())out+='&';out+=encode(field.first)+'='+encode(field.second);}return out;}
std::string jsonText(json_t* value){char* raw=json_dumps(value,JSON_COMPACT|JSON_ENSURE_ASCII);std::string out=raw?raw:"{}";free(raw);return out;}
std::string jsonString(json_t* object,const char* key){json_t* value=json_is_object(object)?json_object_get(object,key):nullptr;if(json_is_string(value))return json_string_value(value);if(json_is_integer(value))return std::to_string(json_integer_value(value));return {};}
long long nowSeconds(){return (long long)std::time(nullptr);}

bool loadSession(){
    if(session.loaded)return !session.token.empty();session.loaded=true;json_error_t error{};json_t* root=json_load_file(sessionPath,0,&error);if(!root)return false;session.token=jsonString(root,"token");session.id=jsonString(root,"id");session.deviceKey=jsonString(root,"device_key");json_decref(root);return !session.token.empty();
}
void saveSession(){json_t* root=json_pack("{s:s,s:s,s:s}","token",session.token.c_str(),"id",session.id.c_str(),"device_key",session.deviceKey.c_str());json_dump_file(root,sessionPath,JSON_COMPACT);json_decref(root);}

struct ApiResult { json_t* value=nullptr; std::string error; ~ApiResult(){if(value)json_decref(value);} ApiResult()=default;ApiResult(const ApiResult&)=delete;ApiResult& operator=(const ApiResult&)=delete;ApiResult(ApiResult&& o)noexcept:value(o.value),error(std::move(o.error)){o.value=nullptr;} };

ApiResult requestApi(net::HttpClient& http,const std::string& endpoint,const std::string& payload,const std::atomic<bool>* cancel){
    ApiResult result;const std::string key=randomText(16),iv=randomText(16);const auto encrypted=encryptAes(payload,key,iv);if(encrypted.empty()){result.error="秒播请求加密失败";return result;}
    const std::string requestKey=hex(encrypted);const std::string keyJson="{\"iv\":\""+iv+"\",\"key\":\""+key+"\"}";const std::string keys=base64(rsaEncrypt(keyJson));if(keys.empty()){result.error="秒播密钥加密失败";return result;}
    const std::string timestamp=std::to_string(nowSeconds()),model="Nintendo-Switch";
    const std::string signature=md5Upper("token_id=,token="+session.token+",phone_type=1,request_key="+requestKey+",app_id=1,time="+timestamp+",keys="+keys+signSecret);
    const auto body=form({{"token_id",""},{"token",session.token},{"phone_type","1"},{"request_key",requestKey},{"app_id","1"},{"time",timestamp},{"keys",keys},{"signature",signature},{"phone_model",model},{"ad_version","1"}});
    std::string lastError;
    for(size_t attempt=0;attempt<std::size(hosts);++attempt){const size_t index=(session.host+attempt)%std::size(hosts);net::HttpRequest request;request.url=std::string(hosts[index])+endpoint;request.method="POST";request.body=body;request.cancel=cancel;request.connectTimeoutSeconds=5;request.timeoutSeconds=15;request.maxResponseBytes=4*1024*1024;request.closeConnection=true;request.headers={{"User-Agent","okhttp/3.12.0"},{"Content-Type","application/x-www-form-urlencoded"},{"Ver","3.0.4.8"},{"Version","2606029"},{"api-ver","3.0.4.8"},{"PackageName","com.v54f07a912.t4ee50f184.dbb1e7669f20260721"},{"code","GZ0313"},{"deviceId","864150060008697"},{"lang","zh_cn"},{"Cache-Control","no-cache"},{"Referer",hosts[index]}};
        const auto response=http.get(request);if(!response.succeeded){lastError=response.error;continue;}json_error_t parseError{};json_t* envelope=json_loadb(response.body.data(),response.body.size(),0,&parseError);if(!envelope){lastError="响应格式错误";continue;}const long long code=json_integer_value(json_object_get(envelope,"code"));json_t* data=json_object_get(envelope,"data");if(code!=200||!json_is_object(data)){lastError=jsonString(envelope,"msg");json_decref(envelope);continue;}const std::string responseKey=jsonString(data,"response_key"),wrappedKeys=jsonString(data,"keys");const std::string keyInfoText=rsaDecryptBlocks(unbase64(wrappedKeys));json_error_t keyError{};json_t* keyInfo=json_loadb(keyInfoText.data(),keyInfoText.size(),0,&keyError);const std::string responseAesKey=jsonString(keyInfo,"key"),responseIv=jsonString(keyInfo,"iv");if(keyInfo)json_decref(keyInfo);const std::string clear=decryptAes(unhex(responseKey),responseAesKey,responseIv);json_decref(envelope);json_error_t clearError{};json_t* decoded=json_loadb(clear.data(),clear.size(),0,&clearError);if(!decoded){lastError="秒播响应解密失败";continue;}session.host=index;result.value=decoded;return result;}
    result.error="秒播接口不可用"+(lastError.empty()?std::string():"："+lastError);return result;
}

bool applyLogin(ApiResult& login){if(!login.value)return false;session.token=jsonString(login.value,"token");const std::string id=jsonString(login.value,"app_user_id");if(!id.empty())session.id=id;if(session.token.empty())return false;saveSession();return true;}
bool ensureAuth(net::HttpClient& http,const std::atomic<bool>* cancel,std::string& error){
    loadSession();if(session.deviceKey.empty()){session.deviceKey=randomHex(40);saveSession();}
    if(!session.token.empty())return true;
    json_t* sign=json_pack("{s:s,s:s}","new_key",session.deviceKey.c_str(),"old_key",oldDeviceKey);auto login=requestApi(http,"/App/Authentication/Device/signIn",jsonText(sign),cancel);json_decref(sign);
    if(!applyLogin(login)){session.token.clear();json_t* signup=json_pack("{s:s,s:s,s:i,s:s}","new_key",session.deviceKey.c_str(),"old_key",oldDeviceKey,"phone_type",1,"code","");auto created=requestApi(http,"/App/Authentication/Device/signUp",jsonText(signup),cancel);json_decref(signup);if(!applyLogin(created)){error=created.error.empty()?"秒播设备认证失败":created.error;return false;}}
    auto refreshed=requestApi(http,"/App/Authentication/Authenticator/refresh","{}",cancel);if(refreshed.value)applyLogin(refreshed);return true;
}

ApiResult authorizedRequest(net::HttpClient& http,const std::string& endpoint,
                            const std::string& payload,const std::atomic<bool>* cancel){
    auto first=requestApi(http,endpoint,payload,cancel);
    if(first.value)return first;
    session.token.clear();session.id.clear();saveSession();
    std::string authError;
    if(!ensureAuth(http,cancel,authError)){first.error=authError;return first;}
    return requestApi(http,endpoint,payload,cancel);
}

std::string absoluteUrl(const std::string& base,const std::string& value){if(value.rfind("http://",0)==0||value.rfind("https://",0)==0)return value;const size_t slash=base.rfind('/');return (slash==std::string::npos?base:base.substr(0,slash+1))+value;}
std::string queryJson(const std::string& value){json_t* root=json_object();size_t p=0;while(p<value.size()){size_t end=value.find('&',p);const std::string field=value.substr(p,end==std::string::npos?std::string::npos:end-p);const size_t equal=field.find('=');if(equal!=std::string::npos)json_object_set_new(root,field.substr(0,equal).c_str(),json_string(field.substr(equal+1).c_str()));if(end==std::string::npos)break;p=end+1;}const std::string out=jsonText(root);json_decref(root);return out;}
}

SearchResponse search(net::HttpClient& http,const std::string& keyword,const std::atomic<bool>* cancel){
    std::lock_guard<std::mutex> lock(sessionMutex);SearchResponse out;std::string error;if(!ensureAuth(http,cancel,error)){out.error=error;return out;}json_t* payload=json_pack("{s:s,s:s,s:s,s:s}","keywords",keyword.c_str(),"ns","","nt",std::to_string(nowSeconds()).c_str(),"order_val","1");auto response=authorizedRequest(http,"/App/Index/findMoreVod",jsonText(payload),cancel);json_decref(payload);if(!response.value){out.error=response.error;return out;}json_t* list=json_object_get(response.value,"list");if(json_is_array(list))for(size_t i=0;i<json_array_size(list)&&out.items.size()<30;++i){json_t* value=json_array_get(list,i);SearchResult item;item.sourceName="秒播";item.title=jsonString(value,"vod_name");item.posterUrl=jsonString(value,"vod_pic");item.status=jsonString(value,"vod_scroe");const std::string id=jsonString(value,"vod_id");item.detailUrl="okswitch:guazi:"+id;if(!item.title.empty()&&!id.empty())out.items.push_back(std::move(item));}out.total=(int)out.items.size();out.succeeded=true;return out;
}

DetailResponse detail(net::HttpClient& http,const std::string& id,const std::atomic<bool>* cancel){
    std::lock_guard<std::mutex> lock(sessionMutex);DetailResponse out;std::string error;if(!ensureAuth(http,cancel,error)){out.error=error;return out;}json_t* infoRequest=json_pack("{s:s,s:s,s:s,s:s}","token_id",session.id.c_str(),"vod_id",id.c_str(),"mobile_time",std::to_string(nowSeconds()).c_str(),"token",session.token.c_str());auto info=authorizedRequest(http,"/App/IndexPlay/playInfo",jsonText(infoRequest),cancel);json_decref(infoRequest);if(!info.value){out.error=info.error;return out;}json_t* vod=json_object_get(info.value,"vodInfo");out.detail.title=jsonString(vod,"vod_name");out.detail.posterUrl=jsonString(vod,"vod_pic");out.detail.year=jsonString(vod,"vod_year");out.detail.actors=jsonString(vod,"vod_actor");out.detail.director=jsonString(vod,"vod_director");out.detail.description=jsonString(vod,"vod_use_content");
    json_t* episodeRequest=json_pack("{s:s,s:s}","vurl_cloud_id","2","vod_d_id",id.c_str());auto episodes=authorizedRequest(http,"/App/Resource/Vurl/show",jsonText(episodeRequest),cancel);json_decref(episodeRequest);if(!episodes.value){out.error=episodes.error;return out;}json_t* list=json_object_get(episodes.value,"list");const bool full1080=appletGetAppletType()==AppletType_Application&&appletGetOperationMode()==AppletOperationMode_Console;const char* qualities1080[]={"1080","720","480","360","240","2160","3840"};const char* qualities720[]={"720","480","360","240"};const char** qualities=full1080?qualities1080:qualities720;const size_t qualityCount=full1080?7:4;if(json_is_array(list))for(size_t i=0;i<json_array_size(list);++i){json_t* value=json_array_get(list,i);json_t* play=json_object_get(value,"play");std::string param,quality;for(size_t qi=0;qi<qualityCount;++qi){const char* q=qualities[qi];json_t* option=json_is_object(play)?json_object_get(play,q):nullptr;if(!json_is_object(option)||jsonString(option,"show_type")=="2")continue;param=jsonString(option,"param");if(!param.empty()){quality=q;break;}}if(param.empty())continue;EpisodeLink episode;episode.label=jsonString(value,"title");if(episode.label.empty())episode.label="第"+std::to_string(i+1)+"集";episode.opaque=queryJson(param);out.detail.episodes.push_back(std::move(episode));}
    if(!out.detail.episodes.empty())out.detail.shares.push_back({full1080?"秒播 | 1080P":"秒播 | 720P","okswitch:guazi"});out.succeeded=!out.detail.title.empty()&&!out.detail.episodes.empty();if(!out.succeeded)out.error="秒播暂无可播放剧集";return out;
}

cloud::ProviderResult<cloud::PlaybackSource> playback(net::HttpClient& http,const std::string& opaque,const std::atomic<bool>* cancel){
    std::lock_guard<std::mutex> lock(sessionMutex);cloud::ProviderResult<cloud::PlaybackSource> out;std::string error;if(!ensureAuth(http,cancel,error)){out.error={cloud::ProviderErrorCode::AuthenticationRequired,error};return out;}json_error_t parseError{};json_t* request=json_loadb(opaque.data(),opaque.size(),0,&parseError);if(!request){out.error={cloud::ProviderErrorCode::InvalidResponse,"秒播剧集参数无效"};return out;}json_t* oldId=json_object_get(request,"vod_d_id");if(oldId){json_object_set(request,"vod_id",oldId);json_object_del(request,"vod_d_id");}const auto ticks=std::chrono::steady_clock::now().time_since_epoch();json_object_set_new(request,"_client_ts",json_string(std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(ticks).count()).c_str()));json_object_set_new(request,"mobile_time",json_string(std::to_string(nowSeconds()).c_str()));json_object_set_new(request,"rand",json_string(std::to_string(std::rand()&0x7fffffff).c_str()));json_object_set_new(request,"type",json_string("play"));auto resolved=authorizedRequest(http,"/App/Resource/VurlDetail/showOne",jsonText(request),cancel);json_decref(request);if(!resolved.value){out.error={cloud::ProviderErrorCode::NetworkError,resolved.error};return out;}const std::string playlistUrl=jsonString(resolved.value,"url");if(playlistUrl.empty()){out.error={cloud::ProviderErrorCode::InvalidResponse,"秒播没有返回播放地址"};return out;}
    net::HttpRequest playlist;playlist.url=playlistUrl;playlist.cancel=cancel;playlist.connectTimeoutSeconds=8;playlist.timeoutSeconds=20;playlist.maxResponseBytes=512*1024;playlist.closeConnection=true;playlist.headers={{"User-Agent","Lavf/57.83.100"},{"Referer",defaultReferer}};const auto response=http.get(playlist);if(!response.succeeded){out.error={cloud::ProviderErrorCode::NetworkError,"秒播 HLS 加载失败："+response.error};return out;}std::istringstream lines(response.body);std::string line;double pending=0,total=0;while(std::getline(lines,line)){if(!line.empty()&&line.back()=='\r')line.pop_back();if(line.rfind("#EXTINF:",0)==0){pending=std::strtod(line.c_str()+8,nullptr);continue;}if(line.empty()||line[0]=='#')continue;if(pending<=0)continue;out.value.segmentUrls.push_back(absoluteUrl(playlistUrl,line));out.value.segmentSizes.push_back(0);out.value.segmentDurations.push_back(pending);total+=pending;pending=0;}if(out.value.segmentUrls.empty()){out.error={cloud::ProviderErrorCode::InvalidResponse,"秒播 HLS 没有有效分片"};return out;}out.value.url=playlistUrl;out.value.durationSeconds=total;out.value.parallelFetches=3;out.value.headers={{"User-Agent","Lavf/57.83.100"},{"Referer",defaultReferer}};out.succeeded=true;return out;
}
}
