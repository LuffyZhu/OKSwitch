#pragma once

#include <string>
#include <atomic>
#include <vector>
#include <SDL2/SDL.h>
#include <curl/curl.h>

namespace okswitch::net
{
struct HttpHeader
{
    std::string name;
    std::string value;
};

struct HttpRequest
{
    std::string url;
    std::string method = "GET";
    std::string body;
    // Empty uses the normal direct connection. Accepted examples include
    // http://host:port and socks5h://host:port.
    std::string proxyUrl;
    std::vector<HttpHeader> headers;
    long connectTimeoutSeconds = 10;
    long timeoutSeconds = 20;
    bool followRedirects = true;
    bool closeConnection = false;
    // Zero keeps the historical unlimited behaviour. Small metadata and HTML
    // requests should set a bound so concurrent providers cannot exhaust RAM.
    size_t maxResponseBytes = 0;
    const std::atomic<bool>* cancel = nullptr;
};

struct HttpResponse
{
    bool succeeded = false;
    long statusCode = 0;
    std::vector<HttpHeader> headers;
    std::string effectiveUrl;
    std::string body;
    std::string error;
};

class HttpClient
{
public:
    HttpClient();
    ~HttpClient();
    HttpClient(const HttpClient&)=delete;
    HttpClient& operator=(const HttpClient&)=delete;
    // Call after joining all request workers, before curl_global_cleanup.
    void shutdown();
    HttpResponse get(const HttpRequest& request) const;
private:
    struct IdleConnection { std::string origin; CURL* handle; };
    SDL_mutex* poolMutex_=nullptr;
    mutable std::vector<IdleConnection> idle_;
    CURL* acquire(const std::string& origin) const;
    void release(const std::string& origin,CURL* handle) const;
};
}
