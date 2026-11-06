#pragma once
#include "okswitch/media/MediaSource.hpp"
#include <cstdint>
namespace okswitch::media {
struct PlaybackRecord {
    SearchResult item;
    std::string shareUrl, fileId, fileName;
    int episodeNumber=0, episodeOrdinal=0;
    double positionSeconds=0, durationSeconds=0;
    int introSkipSeconds=0, outroSkipSeconds=0;
    int volumePercent=100;
    int64_t updatedAt=0;
};
struct SeriesPlaybackPreferences {
    std::string title, detailUrl;
    int introSkipSeconds=0, outroSkipSeconds=0, volumePercent=100;
    int64_t updatedAt=0;
};
class PlaybackHistory {
public:
    bool load();
    // Playback engine calls this with actual elapsed playback, never on browse/select.
    bool recordProgress(const PlaybackRecord& record);
    bool remove(const std::string& detailUrl,const std::string& shareUrl,const std::string& fileId);
    bool removeSeries(const std::string& detailUrl);
    const PlaybackRecord* find(const std::string& detailUrl,const std::string& shareUrl,const std::string& fileId) const;
    // Series preferences follow every episode and the most recently updated
    // record is authoritative.
    const PlaybackRecord* findSeries(const std::string& detailUrl) const;
    const PlaybackRecord* findSeriesByTitle(const std::string& title) const;
    const PlaybackRecord* findTitleEpisode(const std::string& title,int episodeNumber,int episodeOrdinal) const;
    const SeriesPlaybackPreferences* findPreferences(const std::string& title,
        const std::string& detailUrl) const;
    bool recordPreferences(const SeriesPlaybackPreferences& preferences);
    const std::vector<PlaybackRecord>& items() const { return records_; }
private:
    std::vector<PlaybackRecord> records_;
    std::vector<SeriesPlaybackPreferences> preferences_;
    bool readable_=true;
};
}
