#pragma once

#include <atomic>
#include <string>
#include <vector>
#include <SDL2/SDL.h>

namespace okswitch::net {
class HttpClient;

struct TrojanNode {
    enum class Protocol { Trojan, Vless, Shadowsocks, ShadowsocksR, Vmess, Anytls } protocol=Protocol::Trojan;
    std::string name;
    std::string server;
    std::string password;
    std::string method;
    std::string sni;
    unsigned short port=0;
    bool allowInsecure=false;
    bool tls=true;
};

// Parses only ordinary TCP/TLS Trojan links. No subscription credentials or
// node passwords are included in error messages.
bool loadTrojanSubscription(HttpClient& http,const std::string& url,
                            std::vector<TrojanNode>& nodes,std::string& error,
                            const std::atomic<bool>* cancel=nullptr);

class NodeLatency {
public:
    explicit NodeLatency(const std::vector<TrojanNode>& nodes);
    ~NodeLatency();
    void focus(size_t index){focus_.store(index);}
    std::vector<int> snapshot();
private:
    static int entry(void* context);
    const std::vector<TrojanNode>& nodes_;
    std::vector<int> values_; // -1 pending, -2 testing, -3 timeout, -4 failed
    SDL_mutex* mutex_=nullptr;
    SDL_Thread* threads_[2]{};
    std::atomic<bool> stop_{false};
    std::atomic<size_t> focus_{0};
};

class TrojanSocksProxy {
public:
    TrojanSocksProxy()=default;
    ~TrojanSocksProxy();
    TrojanSocksProxy(const TrojanSocksProxy&)=delete;
    TrojanSocksProxy& operator=(const TrojanSocksProxy&)=delete;
    bool start(const TrojanNode& node,std::string& error);
    void stop();
    std::string proxyUrl() const;
private:
    struct Client;
    struct IdleSession;
    static int acceptEntry(void* context);
    static int clientEntry(void* context);
    void reap();
    TrojanNode node_;
    std::atomic<bool> stopping_{false};
    int listener_=-1;
    unsigned short port_=0;
    SDL_Thread* acceptThread_=nullptr;
    SDL_mutex* clientsMutex_=nullptr;
    std::vector<Client*> clients_;
    SDL_mutex* idleMutex_=nullptr;
    std::vector<IdleSession*> idleSessions_;
    std::string paddingScheme_;
};
}
