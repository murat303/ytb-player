#pragma once

#include <string>

namespace newpipe {

struct PlaybackRequest {
    std::string title;
    // Shown under the title while the video opens.
    std::string channel;
    std::string url;
    std::string referer;
    std::string http_header_fields;
    // A Short's StreamItem::reel_sequence: the player moves on through the Shorts after it.
    std::string reel_sequence;
};

bool run_switch_player(const PlaybackRequest& request, std::string& error);

}  // namespace newpipe
