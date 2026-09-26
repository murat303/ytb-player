#include "newpipe/sponsorblock.hpp"

#include <algorithm>
#include <array>
#include <cstdio>

#include "nlohmann/json.hpp"
#include "newpipe/log.hpp"

#ifdef __SWITCH__
#include <mbedtls/sha256.h>
#else
#include <openssl/sha.h>
#endif

namespace newpipe {
namespace {

using nlohmann::json;

std::string sha256_hex(const std::string& input) {
    std::array<unsigned char, 32> digest{};
#ifdef __SWITCH__
    mbedtls_sha256_ret(reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest.data(), 0);
#else
    SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest.data());
#endif
    std::string hex;
    char byte[3];
    for (const unsigned char value : digest) {
        std::snprintf(byte, sizeof(byte), "%02x", value);
        hex += byte;
    }
    return hex;
}

}  // namespace

std::vector<SkipSegment> fetch_sponsor_segments(HttpClient& client, const std::string& video_id) {
    std::vector<SkipSegment> segments;
    if (video_id.empty()) {
        return segments;
    }

    // categories=["sponsor","selfpromo","interaction"], actionTypes=["skip"]
    const std::string url = "https://sponsor.ajay.app/api/skipSegments/" + sha256_hex(video_id).substr(0, 4)
        + "?categories=%5B%22sponsor%22%2C%22selfpromo%22%2C%22interaction%22%5D"
          "&actionTypes=%5B%22skip%22%5D";
    // The server answers 404 when no video of this hash prefix has segments.
    const auto response = client.get(url, {{"Accept", "application/json"}});
    if (!response.has_value() || response->empty()) {
        return segments;
    }
    const json root = json::parse(*response, nullptr, false);
    if (!root.is_array()) {
        return segments;
    }

    for (const auto& video : root) {
        if (!video.is_object() || video.value("videoID", std::string()) != video_id) {
            continue;
        }
        for (const auto& entry : video.value("segments", json::array())) {
            const json range = entry.value("segment", json::array());
            if (!range.is_array() || range.size() < 2 || !range[0].is_number() || !range[1].is_number()) {
                continue;
            }
            SkipSegment segment;
            segment.start = range[0].get<double>();
            segment.end = range[1].get<double>();
            segment.category = entry.value("category", std::string());
            // Shorter than a second is not worth a seek.
            if (segment.end - segment.start >= 1.0) {
                segments.push_back(segment);
            }
        }
    }
    std::sort(segments.begin(), segments.end(), [](const SkipSegment& a, const SkipSegment& b) {
        return a.start < b.start;
    });
    logf("sponsorblock: video=%s segments=%zu", video_id.c_str(), segments.size());
    return segments;
}

}  // namespace newpipe
