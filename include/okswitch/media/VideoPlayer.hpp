#pragma once

#include "okswitch/cloud/CloudDriveProvider.hpp"
#include <SDL2/SDL.h>
#include <string>
#include <functional>
#include <vector>

namespace okswitch::media
{
struct PlaybackResult
{
    bool opened = false;
    bool completed = false;
    double positionSeconds = 0;
    double durationSeconds = 0;
    int introSkipSeconds = 0;
    int outroSkipSeconds = 0;
    int volumePercent = 100;
    int episodeDelta = 0;
    int requestedQualityIndex = -1;
    std::string error;
};

struct PlaybackOptions
{
    std::string displayTitle;
    double resumeSeconds = 0;
    int introSkipSeconds = 0;
    int outroSkipSeconds = 0;
    int volumePercent = 100;
    bool canPrevious = false;
    bool canNext = false;
    std::vector<std::string> liveQualityLabels;
    int currentQualityIndex = 0;
    std::function<int(const std::string&)> measureText;
    std::function<void(int, int, int)> onPreferencesChanged;
    std::function<void()> openSettings;
};

class VideoPlayer
{
public:
    static PlaybackResult play(
        SDL_Renderer* renderer,
        const cloud::PlaybackSource& source,
        const PlaybackOptions& options,
        const std::function<void(const std::string&)>& showStatus,
        const std::function<void(const std::string&, int, int, int)>& drawText);
};
}
