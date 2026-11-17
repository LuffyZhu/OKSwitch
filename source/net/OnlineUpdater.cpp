#include "okswitch/net/OnlineUpdater.hpp"

#include <curl/curl.h>
#include <jansson.h>
#include <mbedtls/sha256.h>
#include <cstdio>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <sys/stat.h>

namespace okswitch::net {
namespace {
constexpr const char* manifestUrl="https://raw.githubusercontent.com/LuffyZhu/OKSwitch/main/update.json";
constexpr const char* currentVersion="1.1.3";
struct Download { FILE* file=nullptr;OnlineUpdater* updater=nullptr;std::atomic<bool>* cancel=nullptr;std::atomic<int>* percent=nullptr; };
size_t writeFile(char* data,size_t size,size_t count,void* context){
    auto* download=static_cast<Download*>(context);
    return std::fwrite(data,size,count,download->file)*size;
}
int progress(void* context,curl_off_t total,curl_off_t current,curl_off_t,curl_off_t){
    auto* download=static_cast<Download*>(context);
    if(total>0)download->percent->store(10+(int)(current*85/total));
    return download->cancel->load()?1:0;
}
bool sha256File(const std::string& path,std::string& hex){
    FILE* file=std::fopen(path.c_str(),"rb");if(!file)return false;
    mbedtls_sha256_context context;mbedtls_sha256_init(&context);
    bool ok=mbedtls_sha256_starts_ret(&context,0)==0;
    unsigned char buffer[65536];size_t size=0;
    while(ok&&(size=std::fread(buffer,1,sizeof(buffer),file))>0)
        ok=mbedtls_sha256_update_ret(&context,buffer,size)==0;
    if(std::ferror(file))ok=false;
    std::fclose(file);
    unsigned char digest[32]{};if(ok)ok=mbedtls_sha256_finish_ret(&context,digest)==0;
    mbedtls_sha256_free(&context);if(!ok)return false;
    static const char digits[]="0123456789abcdef";
    for(unsigned char value:digest){hex.push_back(digits[value>>4]);hex.push_back(digits[value&15]);}
    return true;
}
const char* stringField(json_t* root,const char* key){
    auto* value=json_object_get(root,key);return json_is_string(value)?json_string_value(value):"";
}
}
OnlineUpdater::OnlineUpdater(HttpClient& http,std::string proxy,std::string installedPath)
    :http_(http),proxy_(std::move(proxy)),installedPath_(std::move(installedPath)),mutex_(SDL_CreateMutex()){}
OnlineUpdater::~OnlineUpdater(){cancel();join();if(mutex_)SDL_DestroyMutex(mutex_);}
void OnlineUpdater::setMessage(const std::string& value){if(mutex_){SDL_LockMutex(mutex_);message_=value;SDL_UnlockMutex(mutex_);}}
std::string OnlineUpdater::message(){if(!mutex_)return {};SDL_LockMutex(mutex_);auto value=message_;SDL_UnlockMutex(mutex_);return value;}
bool OnlineUpdater::start(){if(thread_)return false;cancel_.store(false);done_.store(false);staged_.store(false);percent_.store(0);thread_=SDL_CreateThread(entry,"online-update",this);return thread_!=nullptr;}
void OnlineUpdater::cancel(){cancel_.store(true);}
void OnlineUpdater::join(){if(thread_){SDL_WaitThread(thread_,nullptr);thread_=nullptr;}}
int OnlineUpdater::entry(void* self){return static_cast<OnlineUpdater*>(self)->run();}
int OnlineUpdater::run(){
    auto finish=[&](const std::string& text){setMessage(text);done_.store(true);return 0;};
    if(installedPath_.empty())return finish("找不到已安装的 OKSwitch.nro，请先把它放在 SD 卡 /switch/ 下");
    setMessage("正在检查更新...");
    HttpRequest request;request.url=manifestUrl;request.proxyUrl=proxy_;request.cancel=&cancel_;
    request.connectTimeoutSeconds=8;request.timeoutSeconds=20;request.maxResponseBytes=8192;
    const auto response=http_.get(request);
    if(cancel_.load())return finish("已取消更新");
    if(!response.succeeded)return finish("检查更新失败："+response.error);
    json_error_t error{};auto* root=json_loadb(response.body.data(),response.body.size(),0,&error);
    if(!json_is_object(root)){if(root)json_decref(root);return finish("更新清单格式错误");}
    const std::string version=stringField(root,"version"),url=stringField(root,"url"),expected=stringField(root,"sha256");
    auto* bytesValue=json_object_get(root,"size");
    const auto bytes=json_is_integer(bytesValue)?json_integer_value(bytesValue):0;
    json_decref(root);
    if(version.empty()||url.rfind("https://github.com/LuffyZhu/OKSwitch/releases/download/",0)!=0
        ||expected.size()!=64||bytes<1024*1024||bytes>100*1024*1024)
        return finish("更新清单内容无效");
    for(char digit:expected)if(!std::isxdigit((unsigned char)digit))return finish("更新校验值无效");
    if(version==currentVersion)return finish("当前已是最新版（"+version+"）");
    // A .nro suffix is needed because hbloader will launch this verified copy
    // to perform the replacement after the old program has exited.
    const std::string temp=installedPath_+".update.nro";
    struct stat partial{};
    curl_off_t offset=stat(temp.c_str(),&partial)==0&&partial.st_size>0&&partial.st_size<bytes
        ?partial.st_size:0;
    if(stat(temp.c_str(),&partial)!=0||partial.st_size!=bytes){
        auto transfer=[&](curl_off_t resume,long& status)->CURLcode{
            FILE* file=std::fopen(temp.c_str(),resume?"ab":"wb");
            if(!file)return CURLE_WRITE_ERROR;
            CURL* curl=curl_easy_init();if(!curl){std::fclose(file);return CURLE_FAILED_INIT;}
            Download download{file,this,&cancel_,&percent_};
            curl_easy_setopt(curl,CURLOPT_URL,url.c_str());
            curl_easy_setopt(curl,CURLOPT_PROXY,proxy_.c_str());
            if(!proxy_.empty())curl_easy_setopt(curl,CURLOPT_NOPROXY,"");
            curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,1L);
            curl_easy_setopt(curl,CURLOPT_CAINFO,"romfs:/cacert.pem");
            curl_easy_setopt(curl,CURLOPT_SSLVERSION,(long)(CURL_SSLVERSION_TLSv1_2|CURL_SSLVERSION_MAX_TLSv1_2));
            curl_easy_setopt(curl,CURLOPT_IPRESOLVE,CURL_IPRESOLVE_V4);
            curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT,10L);
            curl_easy_setopt(curl,CURLOPT_TIMEOUT,600L);
            curl_easy_setopt(curl,CURLOPT_BUFFERSIZE,128L*1024);
            curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);
            curl_easy_setopt(curl,CURLOPT_NOPROGRESS,0L);
            curl_easy_setopt(curl,CURLOPT_XFERINFOFUNCTION,progress);
            curl_easy_setopt(curl,CURLOPT_XFERINFODATA,&download);
            curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,writeFile);
            curl_easy_setopt(curl,CURLOPT_WRITEDATA,&download);
            if(resume)curl_easy_setopt(curl,CURLOPT_RESUME_FROM_LARGE,resume);
            const CURLcode result=curl_easy_perform(curl);
            curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&status);
            curl_easy_cleanup(curl);
            const bool flushed=std::fflush(file)==0;
            const bool closed=std::fclose(file)==0;
            if(!flushed||!closed)return CURLE_WRITE_ERROR;
            return result;
        };
        setMessage(offset?"正在续传 "+version+"...":"正在下载 "+version+"...");
        long status=0;CURLcode result=transfer(offset,status);
        if(!cancel_.load()&&offset&&(result!=CURLE_OK||status!=206)){
            setMessage("续传不可用，正在重新下载...");
            offset=0;result=transfer(0,status);
        }
        if(cancel_.load())return finish("已暂停下载，下次可继续");
        if(result!=CURLE_OK||(status!=200&&status!=206))
            return finish("下载失败，可重试续传："+std::string(curl_easy_strerror(result)));
    }
    struct stat info{};if(stat(temp.c_str(),&info)!=0||info.st_size!=bytes){std::remove(temp.c_str());return finish("下载文件大小校验失败");}
    setMessage("正在校验安装包...");percent_.store(96);
    std::string actual;if(!sha256File(temp,actual)||actual!=expected){std::remove(temp.c_str());return finish("下载文件 SHA-256 校验失败");}
    stagedPath_=temp;expectedHash_=expected;staged_.store(true);
    percent_.store(100);
    return finish("下载完成（"+version+"）· 按 A 退出并安装");
}

bool OnlineUpdater::applyStaged(const std::string& stagedPath,const std::string& installedPath,
                                const std::string& expectedHash,std::string& error){
    const bool validPath=installedPath=="sdmc:/switch/OKSwitch.nro"
        ||installedPath=="sdmc:/switch/OKSwitch/OKSwitch.nro";
    if(!validPath||stagedPath!=installedPath+".update.nro"||expectedHash.size()!=64){error="更新路径无效";return false;}
    std::string actual;
    if(!sha256File(stagedPath,actual)||actual!=expectedHash){error="更新文件校验失败";return false;}
    const std::string backup=installedPath+".backup";
    if(std::remove(backup.c_str())!=0&&errno!=ENOENT){error="旧备份无法清理："+std::to_string(errno);return false;}
    if(std::rename(installedPath.c_str(),backup.c_str())!=0){error="无法备份旧版："+std::to_string(errno);return false;}
    FILE* source=std::fopen(stagedPath.c_str(),"rb");
    FILE* destination=std::fopen(installedPath.c_str(),"wb");
    bool copied=source&&destination;
    unsigned char buffer[128*1024];
    while(copied){
        const size_t got=std::fread(buffer,1,sizeof(buffer),source);
        if(!got){copied=!std::ferror(source);break;}
        if(std::fwrite(buffer,1,got,destination)!=got){copied=false;break;}
    }
    if(source)std::fclose(source);
    if(destination&&std::fclose(destination)!=0)copied=false;
    actual.clear();
    if(copied)copied=sha256File(installedPath,actual)&&actual==expectedHash;
    if(!copied){
        std::remove(installedPath.c_str());
        if(std::rename(backup.c_str(),installedPath.c_str())!=0)
            error="安装失败，旧版保存在 "+backup;
        else error="安装失败，已恢复旧版";
        return false;
    }
    std::remove(backup.c_str());
    return true;
}
}
