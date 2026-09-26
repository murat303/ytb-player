#pragma once

#include <optional>
#include <string>
#include <vector>

#include "newpipe/http_client.hpp"
#include "newpipe/youtube_resolver.hpp"

namespace newpipe {

struct SubtitleCue {
    double start = 0.0;
    double end = 0.0;
    std::string text;  // one or more lines separated by '\n'
};

// The track to show: a hand-made track in `language`, then an automatic one; without either
// the video's own language (YouTube answers machine-translation requests, &tlang=, with 429
// without a proof-of-origin token), then any hand-made track, then any track.
std::optional<CaptionTrack> pick_subtitle_track(
    const std::vector<CaptionTrack>& tracks,
    const std::string& language,
    const std::string& original_language);

// Language tags match on the primary subtag: "en" is "en-US".
bool same_language(const std::string& a, const std::string& b);

// Downloads the track as json3 and turns its events into cues.
std::vector<SubtitleCue> fetch_subtitle_cues(HttpClient& client, const CaptionTrack& track);

}  // namespace newpipe
