#include "newpipe/subtitles.hpp"

#include <algorithm>

#include "nlohmann/json.hpp"
#include "newpipe/log.hpp"

namespace newpipe {
namespace {

using nlohmann::json;

std::string primary_subtag(const std::string& tag) {
    return tag.substr(0, tag.find('-'));
}

std::string trim_lines(const std::string& text) {
    std::string out;
    std::string line;
    auto flush = [&]() {
        const size_t first = line.find_first_not_of(" \t\r");
        if (first != std::string::npos) {
            const size_t last = line.find_last_not_of(" \t\r");
            if (!out.empty()) {
                out += '\n';
            }
            out += line.substr(first, last - first + 1);
        }
        line.clear();
    };
    for (const char ch : text) {
        if (ch == '\n') {
            flush();
        } else {
            line += ch;
        }
    }
    flush();
    return out;
}

}  // namespace

bool same_language(const std::string& a, const std::string& b) {
    return !a.empty() && !b.empty() && primary_subtag(a) == primary_subtag(b);
}

std::optional<CaptionTrack> pick_subtitle_track(
    const std::vector<CaptionTrack>& tracks,
    const std::string& language,
    const std::string& original_language) {
    // Auto-dubbed videos list automatic tracks in many languages; the video's own one is the
    // real transcript. Its language is not always known: English is the likeliest then.
    const std::string own = original_language.empty() ? std::string("en") : original_language;
    for (const std::string& wanted : {language, own}) {
        for (const bool automatic : {false, true}) {
            for (const auto& track : tracks) {
                if (track.auto_generated == automatic && same_language(track.language_code, wanted)) {
                    return track;
                }
            }
        }
    }
    for (const bool automatic : {false, true}) {
        for (const auto& track : tracks) {
            if (track.auto_generated == automatic) {
                return track;
            }
        }
    }
    return std::nullopt;
}

std::vector<SubtitleCue> fetch_subtitle_cues(HttpClient& client, const CaptionTrack& track) {
    std::vector<SubtitleCue> cues;
    const auto response = client.get(track.base_url + "&fmt=json3");
    if (!response.has_value() || response->empty()) {
        log_line("subtitles: download failed");
        return cues;
    }
    const json root = json::parse(*response, nullptr, false);
    if (!root.is_object()) {
        log_line("subtitles: not json3");
        return cues;
    }

    // Each event with text is a cue. Automatic tracks roll up: an event starts before the
    // previous one ends, and the player shows the overlapping ones as two lines.
    for (const auto& event : root.value("events", json::array())) {
        if (!event.is_object() || !event.contains("segs")) {
            continue;
        }
        std::string text;
        for (const auto& segment : event.value("segs", json::array())) {
            text += segment.value("utf8", std::string());
        }
        text = trim_lines(text);
        if (text.empty()) {
            continue;
        }
        SubtitleCue cue;
        cue.start = event.value("tStartMs", 0.0) / 1000.0;
        cue.end = cue.start + event.value("dDurationMs", 0.0) / 1000.0;
        cue.text = text;
        if (cue.end > cue.start) {
            cues.push_back(std::move(cue));
        }
    }
    std::sort(cues.begin(), cues.end(), [](const SubtitleCue& a, const SubtitleCue& b) {
        return a.start < b.start;
    });
    logf("subtitles: %s%s cues=%zu", track.language_code.c_str(), track.auto_generated ? " (auto)" : "",
         cues.size());
    return cues;
}

}  // namespace newpipe
