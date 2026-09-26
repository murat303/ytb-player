#pragma once

#include <string>
#include <vector>

#include "newpipe/http_client.hpp"

namespace newpipe {

struct SkipSegment {
    double start = 0.0;
    double end = 0.0;
    std::string category;
};

// SponsorBlock (sponsor.ajay.app) segments to skip in a video: sponsor reads, self-promotion
// and "like and subscribe" reminders. Only the first four hex digits of the SHA-256 of the
// video id go to the server; the video is picked out of the answer here. Empty when the
// video has none or the request fails.
std::vector<SkipSegment> fetch_sponsor_segments(HttpClient& client, const std::string& video_id);

}  // namespace newpipe
