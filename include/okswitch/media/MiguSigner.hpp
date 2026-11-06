#pragma once

#include <string>
#include "okswitch/net/HttpClient.hpp"

namespace okswitch::media
{
// Uses the official web player's public WebAssembly module at runtime.
// No playback URL, module binary, or account token is stored on disk.
bool signMiguPlaylist(net::HttpClient& http, const std::string& url,
                      std::string& signedUrl, std::string& error, const std::atomic<bool>* cancel = nullptr);
}
