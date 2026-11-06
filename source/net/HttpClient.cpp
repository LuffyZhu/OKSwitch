#include "okswitch/net/HttpClient.hpp"

#include <curl/curl.h>
#include <cstdio>

namespace okswitch::net
{
namespace
{
int transferProgress(void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    return static_cast<const std::atomic<bool>*>(context)->load()?1:0;
}
size_t responseHeader(char* data,size_t size,size_t count,void* context){
    size_t bytes=size*count;std::string line(data,bytes);auto colon=line.find(':');
    if(colon!=std::string::npos){
        std::string value=line.substr(colon+1);
        while(!value.empty()&&(value.front()==' '||value.front()=='\t'))value.erase(0,1);
        while(!value.empty()&&(value.back()=='\r'||value.back()=='\n'))value.pop_back();
        static_cast<HttpResponse*>(context)->headers.push_back({line.substr(0,colon),value});
    }
    return bytes;
}
struct WriteContext
{
    std::string* body;
    size_t limit;
    bool exceeded = false;
};
size_t writeResponse(char* data, size_t size, size_t count, void* userData)
{
    const size_t bytes = size * count;
    auto* output = static_cast<WriteContext*>(userData);
    if (output->limit && (output->body->size() > output->limit ||
        bytes > output->limit - output->body->size()))
    {
        output->exceeded = true;
        return 0;
    }
    output->body->append(data, bytes);
    return bytes;
}
}

HttpClient::HttpClient():poolMutex_(SDL_CreateMutex()) {}
HttpClient::~HttpClient(){shutdown();}
void HttpClient::shutdown(){
    if(!poolMutex_)return;
    for(auto& item:idle_)curl_easy_cleanup(item.handle);
    idle_.clear();SDL_DestroyMutex(poolMutex_);poolMutex_=nullptr;
}
CURL* HttpClient::acquire(const std::string& origin) const {
    CURL* handle=nullptr;
    if(poolMutex_){
        SDL_LockMutex(poolMutex_);
        for(auto it=idle_.begin();it!=idle_.end();++it){
            if(it->origin==origin){handle=it->handle;idle_.erase(it);break;}
        }
        SDL_UnlockMutex(poolMutex_);
    }
    return handle?handle:curl_easy_init();
}
void HttpClient::release(const std::string& origin,CURL* handle) const {
    // Reset per-request pointers/options but retain connections and DNS cache.
    curl_easy_reset(handle);
    CURL* evicted=nullptr;
    if(poolMutex_){
        SDL_LockMutex(poolMutex_);
        if(idle_.size()>=8){evicted=idle_.front().handle;idle_.erase(idle_.begin());}
        idle_.push_back({origin,handle});
        SDL_UnlockMutex(poolMutex_);
    }else evicted=handle;
    if(evicted)curl_easy_cleanup(evicted);
}

HttpResponse HttpClient::get(const HttpRequest& request) const
{
    HttpResponse response;
    const size_t scheme=request.url.find("://");
    const size_t end=scheme==std::string::npos?std::string::npos:request.url.find_first_of("/?#",scheme+3);
    const std::string origin=request.url.substr(0,end);
    CURL* curl = acquire(origin);
    if (curl == nullptr)
    {
        response.error = "curl_easy_init failed";
        return response;
    }

    curl_slist* headers = nullptr;
    for (const HttpHeader& header : request.headers)
    {
        const std::string line = header.name + ": " + header.value;
        headers = curl_slist_append(headers, line.c_str());
    }

    // Closing the response connection lets affected emulator SSL implementations
    // drain buffered TLS records once the underlying socket reaches EOF.
    if(request.closeConnection){
        headers=curl_slist_append(headers,"Connection: close");
        curl_easy_setopt(curl,CURLOPT_FRESH_CONNECT,1L);
        curl_easy_setopt(curl,CURLOPT_FORBID_REUSE,1L);
    }
    curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(curl,CURLOPT_PROXY,request.proxyUrl.c_str());
    if(!request.proxyUrl.empty())curl_easy_setopt(curl,CURLOPT_NOPROXY,"");
    // The Switch curl TLS backend does not reliably have every current public
    // root CA available on older firmware. Ship the Mozilla bundle in RomFS so
    // certificate, hostname and date verification can remain enabled.
    if(request.url.rfind("https://",0)==0){
        curl_easy_setopt(curl,CURLOPT_CAINFO,"romfs:/cacert.pem");
        // Ryujinx 1.3.x can terminate its SSL service during some TLS 1.3
        // handshakes. TLS 1.2 is supported by the services OKSwitch uses and
        // avoids that emulator crash path.
        curl_easy_setopt(curl,CURLOPT_SSLVERSION,
            (long)(CURL_SSLVERSION_TLSv1_2|CURL_SSLVERSION_MAX_TLSv1_2));
    }
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(curl, CURLOPT_DNS_CACHE_TIMEOUT, 300L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    if(request.cancel){
        curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);
        curl_easy_setopt(curl,CURLOPT_NOPROGRESS,0L);
        curl_easy_setopt(curl,CURLOPT_XFERINFOFUNCTION,transferProgress);
        curl_easy_setopt(curl,CURLOPT_XFERINFODATA,request.cancel);
    }
    if (request.method != "GET")
    {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, request.method.c_str());
    }
    if (!request.body.empty())
    {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.data());
        curl_easy_setopt(
            curl,
            CURLOPT_POSTFIELDSIZE,
            static_cast<long>(request.body.size()));
    }
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, request.followRedirects ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, request.connectTimeoutSeconds);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, request.timeoutSeconds);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "OKSwitch/0.1");
    WriteContext writeContext{&response.body,request.maxResponseBytes};
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeResponse);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &writeContext);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, responseHeader);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response);
    if (headers != nullptr)
    {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    const CURLcode result = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.statusCode);
    if (result == CURLE_OK)
    {
        char* effectiveUrl = nullptr;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.statusCode);
        curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effectiveUrl);
        if (effectiveUrl != nullptr)
        {
            response.effectiveUrl = effectiveUrl;
        }
        response.succeeded = response.statusCode >= 200 && response.statusCode < 300;
        if (!response.succeeded)
        {
            response.error = "HTTP status " + std::to_string(response.statusCode);
        }
    }
    else
    {
        response.error = writeContext.exceeded ? "响应内容过大，已停止读取"
            : result==CURLE_PEER_FAILED_VERIFICATION
            ? "HTTPS 证书校验失败，请检查主机日期时间"
            : curl_easy_strerror(result);
    }

    double dns=0,connect=0,tls=0,first=0,total=0;
    long connections=0;
    curl_off_t downloaded=0,expected=-1;
    curl_easy_getinfo(curl,CURLINFO_SIZE_DOWNLOAD_T,&downloaded);
    curl_easy_getinfo(curl,CURLINFO_CONTENT_LENGTH_DOWNLOAD_T,&expected);
    curl_easy_getinfo(curl,CURLINFO_NAMELOOKUP_TIME,&dns);
    curl_easy_getinfo(curl,CURLINFO_CONNECT_TIME,&connect);
    curl_easy_getinfo(curl,CURLINFO_APPCONNECT_TIME,&tls);
    curl_easy_getinfo(curl,CURLINFO_STARTTRANSFER_TIME,&first);
    curl_easy_getinfo(curl,CURLINFO_TOTAL_TIME,&total);
    curl_easy_getinfo(curl,CURLINFO_NUM_CONNECTS,&connections);
    // Log only origin: query strings can contain API keys or share tokens.
    SDL_Log("HTTP %s status=%ld curl=%d dns=%.3f tcp=%.3f tls=%.3f first=%.3f total=%.3f new=%ld",
        origin.c_str(),response.statusCode,(int)result,dns,connect,tls,first,total,connections);
    SDL_Log("HTTP body status=%ld received=%lld expected=%lld decoded=%zu",response.statusCode,(long long)downloaded,(long long)expected,response.body.size());
    if(result==CURLE_OPERATION_TIMEDOUT){
        char detail[160];
        std::snprintf(detail,sizeof(detail),"Timeout %s H%ld B%lld/%lld TCP%.1f TLS%.1f F%.1f / %.1fs",
            request.proxyUrl.empty()?"direct":"proxy",response.statusCode,
            (long long)downloaded,(long long)expected,connect,tls,first,total);
        response.error=detail;
    }
    // Clear options referring to local objects before releasing their storage.
    release(origin,curl);
    curl_slist_free_all(headers);
    return response;
}
}
