#include "okswitch/config/SiteConfigLoader.hpp"

#include "okswitch/net/HttpClient.hpp"
#include <jansson.h>

namespace okswitch::config
{
SiteConfigSummary loadSiteConfigSummary(
    const net::HttpClient& httpClient,
    const std::string& url)
{
    SiteConfigSummary summary;
    net::HttpRequest request;
    request.url = url;

    const net::HttpResponse response = httpClient.get(request);
    if (!response.succeeded)
    {
        summary.error = response.error;
        return summary;
    }

    json_error_t jsonError{};
    json_t* root = json_loadb(
        response.body.data(), response.body.size(), 0, &jsonError);
    if (root == nullptr)
    {
        summary.error = jsonError.text;
        return summary;
    }

    json_t* sites = json_object_get(root, "sites");
    if (json_is_array(sites))
    {
        summary.loaded = true;
        summary.siteCount = json_array_size(sites);
    }
    else
    {
        summary.error = "sites is not an array";
    }

    json_decref(root);
    return summary;
}
}
