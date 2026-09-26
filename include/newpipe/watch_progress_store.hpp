#pragma once

#include <mutex>
#include <string>
#include <unordered_map>

#include "newpipe/models.hpp"

namespace newpipe {

// Where each video was left: the player resumes from it and the cards draw their red bar with
// it. Kept in the app folder (progress.json) for the most recently watched videos.
class WatchProgressStore {
public:
    static WatchProgressStore& instance();

    // Seconds to start from; 0 when the video is unknown, barely started or finished.
    double resume_position(const std::string& video_id);
    // Watched share 0..1 for the card bar (1 once finished), 0 when unknown.
    float watched_fraction(const StreamItem& item);
    // Called when the player closes. Under 10 s drops the resume point; near the end marks
    // the video finished.
    void save(const std::string& video_id, double position, double duration);

private:
    struct Entry {
        double position = 0.0;
        double duration = 0.0;
        long long order = 0;  // higher = saved later; the lowest is dropped first
    };

    WatchProgressStore() = default;
    void ensure_loaded_locked();
    void persist_locked();

    std::mutex mutex_;
    bool loaded_ = false;
    long long last_order_ = 0;
    std::unordered_map<std::string, Entry> entries_;
};

}  // namespace newpipe
