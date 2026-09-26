#include "newpipe/watch_progress_store.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>

#include "nlohmann/json.hpp"
#include "newpipe/app_paths.hpp"
#include "newpipe/log.hpp"
#include "newpipe/youtube_resolver.hpp"

namespace newpipe {
namespace {

using nlohmann::json;

constexpr size_t kProgressLimit = 500;
constexpr double kMinPositionSeconds = 10.0;
constexpr double kResumeRewindSeconds = 3.0;
// Closing within the last 5 % (at least 15 s, i.e. the end screen) counts as watched.
constexpr double kFinishedShare = 0.05;
constexpr double kFinishedMinSeconds = 15.0;
// A bar thinner than this is invisible on a card.
constexpr float kMinVisibleFraction = 0.03f;

std::string progress_store_path() {
    return app_file_path("progress.json");
}

bool is_finished(double position, double duration) {
    return duration > 0.0 && duration - position < std::max(kFinishedMinSeconds, duration * kFinishedShare);
}

bool write_text_file(const std::string& path, const std::string& contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.good()) {
        return false;
    }

    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    return output.good();
}

}  // namespace

WatchProgressStore& WatchProgressStore::instance() {
    static WatchProgressStore store;
    return store;
}

void WatchProgressStore::ensure_loaded_locked() {
    if (loaded_) {
        return;
    }
    loaded_ = true;

    std::ifstream input(progress_store_path(), std::ios::binary);
    if (!input.good()) {
        return;
    }
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const json root = json::parse(text, nullptr, false);
    if (!root.is_object() || !root.contains("videos") || !root.at("videos").is_object()) {
        return;
    }

    // Each video is [position, duration, order].
    const json& videos = root.at("videos");
    for (auto it = videos.begin(); it != videos.end(); ++it) {
        const json& value = it.value();
        if (!value.is_array() || value.size() < 3 || !value[0].is_number() || !value[1].is_number()
            || !value[2].is_number_integer()) {
            continue;
        }
        Entry entry;
        entry.position = value[0].get<double>();
        entry.duration = value[1].get<double>();
        entry.order = value[2].get<long long>();
        last_order_ = std::max(last_order_, entry.order);
        entries_[it.key()] = entry;
    }
}

void WatchProgressStore::persist_locked() {
    json videos = json::object();
    for (const auto& [id, entry] : entries_) {
        videos[id] = json::array({std::round(entry.position * 10.0) / 10.0,
                                  std::round(entry.duration * 10.0) / 10.0,
                                  entry.order});
    }
    const json root = {{"videos", videos}};
    if (!write_text_file(progress_store_path(), root.dump())) {
        log_line("progress: save failed");
    }
}

double WatchProgressStore::resume_position(const std::string& video_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    ensure_loaded_locked();
    const auto it = entries_.find(video_id);
    if (it == entries_.end() || it->second.position < kMinPositionSeconds
        || is_finished(it->second.position, it->second.duration)) {
        return 0.0;
    }
    // A few seconds of overlap to pick the thread up again.
    return std::max(0.0, it->second.position - kResumeRewindSeconds);
}

float WatchProgressStore::watched_fraction(const StreamItem& item) {
    const auto from_url = YouTubeResolver::extract_video_id(item.url);
    const std::string& video_id = from_url ? *from_url : item.id;
    if (video_id.empty()) {
        return 0.0f;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    ensure_loaded_locked();
    const auto it = entries_.find(video_id);
    if (it == entries_.end() || !(it->second.duration > 0.0)) {
        return 0.0f;
    }
    if (is_finished(it->second.position, it->second.duration)) {
        return 1.0f;
    }
    const float fraction = static_cast<float>(it->second.position / it->second.duration);
    return std::clamp(fraction, kMinVisibleFraction, 1.0f);
}

void WatchProgressStore::save(const std::string& video_id, double position, double duration) {
    if (video_id.empty() || !std::isfinite(position) || !std::isfinite(duration) || duration <= 0.0) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    ensure_loaded_locked();
    const auto it = entries_.find(video_id);
    if (position < kMinPositionSeconds) {
        // Barely watched, or sent back to the start with "-": nothing to resume. A finished
        // mark from an earlier viewing stays.
        if (it != entries_.end() && !is_finished(it->second.position, it->second.duration)) {
            entries_.erase(it);
            persist_locked();
        }
        return;
    }

    Entry entry;
    entry.duration = duration;
    entry.position = is_finished(position, duration) ? duration : std::min(position, duration);
    entry.order = ++last_order_;
    entries_[video_id] = entry;

    while (entries_.size() > kProgressLimit) {
        const auto oldest = std::min_element(entries_.begin(), entries_.end(), [](const auto& a, const auto& b) {
            return a.second.order < b.second.order;
        });
        entries_.erase(oldest);
    }
    persist_locked();
}

}  // namespace newpipe
