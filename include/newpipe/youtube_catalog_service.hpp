#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "newpipe/auth_store.hpp"
#include "newpipe/catalog_service.hpp"
#include "newpipe/http_client.hpp"
#include "newpipe/settings_store.hpp"

namespace newpipe {

class YouTubeCatalogService final : public CatalogService {
public:
    explicit YouTubeCatalogService(HttpClient* client = nullptr, AuthStore* auth_store = nullptr);

    bool is_loaded() const { return true; }
    const std::string& error_message() const { return error_message_; }

    bool load_auth_session(std::string* error_message = nullptr);
    bool reload_auth_session(std::string* error_message = nullptr);
    bool has_auth_session() const;
    AuthSession auth_session() const;
    bool import_auth_session_from_file(
        const std::string& file_path = {},
        std::string* error_message = nullptr);
    bool update_auth_session_from_cookie(
        const std::string& cookie_header,
        const std::string& source_label,
        std::string* error_message = nullptr);
    bool clear_auth_session(std::string* error_message = nullptr);

    std::vector<Kiosk> list_kiosks() const override;
    std::optional<HomeFeed> get_home_feed(const std::string& kiosk_id) const override;
    std::optional<HomeFeed> get_subscriptions_feed() const;
    std::optional<HomeFeed> get_related_feed(const StreamItem& item) const;
    std::optional<HomeFeed> get_channel_feed(const StreamItem& item) const;
    // One tab of a channel's page (params from ChannelInfo::tabs): videos, Shorts, live streams
    // or playlists. With a continuation (the feed's next_page_token), that tab's next page.
    std::optional<HomeFeed> get_channel_tab(
        const std::string& channel_id,
        const std::string& params,
        const std::string& continuation = {}) const;
    std::optional<HomeFeed> get_playlist_feed(const StreamItem& item) const;
    // The Shorts that follow one in YouTube's Shorts player: `params` is a Short's reel_sequence
    // or the next_page_token of an earlier answer. The items carry the id and the link only.
    std::optional<HomeFeed> get_shorts_sequence(const std::string& params) const;
    // Shorts from a search with YouTube's Shorts filter (Home's Shorts), with their titles.
    std::optional<HomeFeed> get_shorts_feed(const std::string& query) const;
    // A list of the signed-in account ("WL" watch later, "LL" liked videos); needs the session.
    std::optional<HomeFeed> get_account_playlist(const std::string& playlist_id) const;
    // The account's notifications, newest first; empty without a session (error_message says).
    std::vector<NotificationItem> list_notifications() const;
    // The account's own and saved playlists (its library's "Playlists" page) as playlist items,
    // without Watch later and Liked videos; empty without a session.
    std::vector<StreamItem> list_account_playlists() const;
    std::optional<CommentPage> get_comments(const StreamItem& item) const;
    // Next page of comments (see CommentPage::next_page_token); not cached.
    std::optional<CommentPage> get_comments_page(const std::string& token) const;
    // The chapters YouTube marks on its player bar, in order; empty when the video has none.
    std::vector<Chapter> get_chapters(const std::string& video_id) const;
    // The channels the session's Google account can act as (YouTube's channel switcher).
    std::vector<YouTubeAccount> list_accounts() const;
    SearchResults search(const std::string& query) const override;
    std::optional<StreamDetail> get_stream_detail(const std::string& url) const override;
    // Refresh (X) must ask YouTube again instead of showing the first page it cached.
    void clear_feed_caches();
    // Next page of a feed or search list (see HomeFeed::next_page_token). The tabs call it on a
    // worker thread through a separate service instance, so it touches no shared caches.
    std::optional<HomeFeed> get_next_page(
        const std::string& token,
        bool uses_search,
        bool allow_short_videos) const;

private:
    std::optional<HomeFeed> fetch_home_feed(
        const std::string& kiosk_id,
        const std::string& title,
        const std::string& query,
        bool allow_short_videos,
        const std::string& search_params = {}) const;
    std::optional<HomeFeed> fetch_authenticated_browse_feed(
        const std::string& browse_id,
        const std::string& title,
        const std::string& referer,
        size_t limit,
        bool allow_short_videos) const;
    std::optional<StreamDetail> fetch_stream_detail_from_player(const std::string& url) const;
    std::optional<HomeFeed> fetch_channel_feed_from_rss(
        const StreamItem& item,
        size_t limit) const;
    std::optional<HomeFeed> fetch_playlist_feed_from_browse(
        const std::string& playlist_id,
        size_t limit) const;
    std::optional<CommentPage> fetch_comments_from_watch(
        const StreamItem& item,
        size_t limit) const;
    // search_params: YouTube's search filter (the "sp" of a search URL), empty for none.
    SearchResults fetch_search_results(
        const std::string& query,
        size_t limit,
        bool allow_short_videos,
        const std::string& search_params = {}) const;
    void cache_stream_details(const std::vector<StreamItem>& items) const;
    void cache_stream_detail(const StreamDetail& detail) const;
    void invalidate_auth_caches();
    // POST to the WEB browse API, signed in when there is a session (required when asked).
    // client_version: the X-Youtube-Client-Version header; empty for the usual one.
    // url: another endpoint of the web client than browse.
    std::optional<std::string> post_web_browse(
        const std::string& payload,
        bool require_session,
        const std::string& client_version = {},
        const std::string& url = {}) const;
    // The ytInitialData JSON of a playlist's page on youtube.com, loaded with the session's cookies.
    std::optional<std::string> fetch_playlist_page_data(const std::string& playlist_id) const;

    HttpsHttpClient owned_client_;
    HttpClient* client_ = nullptr;
    AuthStore* auth_store_ = nullptr;
    mutable std::unordered_map<std::string, HomeFeed> home_feed_cache_;
    mutable std::unordered_map<std::string, HomeFeed> related_feed_cache_;
    mutable std::unordered_map<std::string, HomeFeed> channel_feed_cache_;
    mutable std::unordered_map<std::string, HomeFeed> playlist_feed_cache_;
    mutable std::unordered_map<std::string, HomeFeed> authenticated_browse_cache_;
    mutable std::unordered_map<std::string, StreamDetail> detail_cache_;
    mutable std::unordered_map<std::string, CommentPage> comments_cache_;
    mutable std::string error_message_;
};

}  // namespace newpipe
