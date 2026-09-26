#pragma once

#include <optional>
#include <string>
#include <vector>

namespace newpipe {

struct Kiosk {
    std::string id;
    std::string title;
};

struct StreamItem {
    std::string id;
    std::string url;
    std::string title;
    std::string channel_name;
    std::string channel_url;
    std::string channel_id;
    std::string thumbnail_url;
    std::string channel_avatar_url;  // the round picture beside the title
    std::string duration_text;
    std::string view_count_text;
    std::string published_text;
    bool is_live = false;
    // A playlist rather than a video (channel pages): url is the playlist's, duration_text its
    // video count ("25 video").
    bool is_playlist = false;
    // A Short's place in YouTube's Shorts player: the params that give the Shorts after it
    // (the channel's, or ones like it). Empty for other videos.
    std::string reel_sequence;
};

struct CommentItem {
    std::string author_name;
    std::string author_url;
    std::string author_thumbnail_url;
    std::string body;
    std::string published_text;
    std::string like_count_text;
    std::string reply_count_text;
    bool is_verified = false;
    bool is_creator = false;  // written by the video's channel
    std::string pinned_text;  // "@jawed tarafından sabitlendi" on a pinned comment
    // The token of the comment's replies (CommentPage of them via get_comments_page).
    std::string replies_token;
};

// A tab of a channel's page and the params its browse request takes.
struct ChannelTab {
    std::string title;
    std::string params;
};

// The top of a channel's page: banner, picture, name, "@handle", subscribers, video count, the
// description's start and the tabs this app can show (videos, Shorts, live, playlists).
struct ChannelInfo {
    std::string id;
    std::string name;
    std::string handle;
    std::string subscribers;
    std::string video_count;
    std::string avatar_url;
    std::string banner_url;
    std::string description;
    std::vector<ChannelTab> tabs;
};

// The top of a playlist's page: its name, "... tarafından", "25 video • 1,2 B görüntüleme",
// the description and the cover.
struct PlaylistInfo {
    std::string id;
    std::string title;
    std::string owner;
    std::string meta;
    std::string description;
    std::string thumbnail_url;
};

// A feed page may carry the token YouTube hands out for the next page. Search-based lists
// (search results and the anonymous home categories) continue through the search API,
// signed-in feeds through the authenticated browse API.
struct HomeFeed {
    Kiosk kiosk;
    std::vector<StreamItem> items;
    std::optional<ChannelInfo> channel;  // set for a channel's page
    std::optional<PlaylistInfo> playlist;  // set for a playlist
    std::string next_page_token;
    bool next_page_uses_search = false;
    bool next_page_allows_shorts = false;
};

struct SearchResults {
    std::string query;
    std::vector<StreamItem> items;
    bool used_fallback = false;
    std::string next_page_token;
};

struct StreamDetail {
    StreamItem item;
    std::string description;
    std::string channel_subscriber_count_text;  // "12,7 Mn abone", from the watch page
    std::string channel_avatar_url;
    std::vector<StreamItem> related_items;
    std::optional<std::string> playback_url;
};

// "Top comments" / "Newest first" and the token of the first page in that order.
struct CommentSort {
    std::string title;
    std::string token;
    bool selected = false;
};

struct CommentPage {
    std::string title;
    std::vector<CommentItem> items;
    std::string next_page_token;  // empty on the last page
    // The first page of a video's comments also has these: "10 Mn" and the orders.
    std::string count_text;
    std::vector<CommentSort> sorts;
};

// One of the account's notifications (the bell): "Linus Tech Tips yükledi: ...", when, the
// channel's picture, the video's picture and the video it opens.
struct NotificationItem {
    std::string text;
    std::string time;
    std::string avatar_url;
    std::string thumbnail_url;
    std::string video_id;
    bool unread = false;
};

// A YouTube channel the signed-in Google account can act as (its own and its brand channels).
struct YouTubeAccount {
    std::string name;
    std::string handle;   // "@SomeChannel"
    std::string page_id;  // empty for the account's own channel
    std::string photo_url;
    bool selected = false;  // the one YouTube currently answers for
};

// A chapter of a video: where it starts and its title.
struct Chapter {
    double start = 0.0;  // seconds
    std::string title;
};

}  // namespace newpipe
