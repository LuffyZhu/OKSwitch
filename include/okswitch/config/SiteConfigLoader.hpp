#pragma once

#include <cstddef>
#include <string>

namespace okswitch::net
{
class HttpClient;
}

namespace okswitch::config
{
struct SiteConfigSummary
{
    bool loaded = false;
    std::size_t siteCount = 0;
    std::string error;
};

SiteConfigSummary loadSiteConfigSummary(
    const net::HttpClient& httpClient,
    const std::string& url);
}
