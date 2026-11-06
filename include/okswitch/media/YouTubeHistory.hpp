#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace okswitch::media {
struct YouTubeHistoryEntry {
    std::string id, title, channel, thumbnail;
    double positionSeconds = 0, durationSeconds = 0;
    int64_t updatedAt = 0;
};
class YouTubeHistory {
public:
    bool load();
    bool record(const YouTubeHistoryEntry& entry);
    const YouTubeHistoryEntry* find(const std::string& id) const;
    const std::vector<YouTubeHistoryEntry>& items() const { return items_; }
private:
    std::vector<YouTubeHistoryEntry> items_;
};
}
