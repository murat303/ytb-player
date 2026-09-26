#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <borealis.hpp>

#include "newpipe/models.hpp"
#include "newpipe/youtube_catalog_service.hpp"

class Chip;

// A list of videos (channel, related, playlist). The screen opens at once; the loader runs on
// a worker thread with its own service instance and the cards appear when it returns. A
// channel's list is shown as its page: the header and the tabs (Videos, Shorts, Live,
// Playlists), each tab loaded when chosen.
class StreamFeedActivity : public brls::Activity {
public:
    using FeedLoader = std::function<std::optional<newpipe::HomeFeed>(newpipe::YouTubeCatalogService&)>;

    StreamFeedActivity(std::string title, FeedLoader loader, std::string failure_text);
    ~StreamFeedActivity() override;

    CONTENT_FROM_XML_RES("activity/stream_feed.xml");

    void onContentAvailable() override;

private:
    void loadFeed();
    void loadNextPage();
    void appendCards(size_t from);
    void clearCards();
    bool allowInitialInput() const;
    void playStream(const newpipe::StreamItem& item);
    void openStream(const newpipe::StreamItem& item);
    void openPlaylist(const newpipe::StreamItem& item);

    void showChannel(const newpipe::ChannelInfo& channel);
    void showPlaylist(const newpipe::PlaylistInfo& playlist);
    // A video of the playlist, played within it: its link names the list, so that the player
    // goes on with the next one.
    newpipe::StreamItem inPlaylist(const newpipe::StreamItem& item) const;
    void selectTab(size_t index);
    void stepTab(int delta);
    void updateTabs();
    void showMessage(const std::string& text);

    std::string title_;
    FeedLoader loader_;
    std::string failureText_;
    std::vector<newpipe::StreamItem> items_;
    std::string nextPageToken_;
    bool nextPageAllowsShorts_ = false;
    bool loadingPage_ = false;
    std::atomic<bool> interactionReady_{false};
    // Flipped by the destructor: a list that arrives after B was pressed is dropped.
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

    // A channel's page: its tabs, the one shown and whether its pages come through
    // get_channel_tab (after a tab was chosen) rather than get_next_page.
    std::optional<newpipe::ChannelInfo> channel_;
    std::vector<Chip*> tabChips_;
    size_t tab_ = 0;
    bool tabStepRegistered_ = false;
    bool tabPaging_ = false;
    // Bumped on every tab change so that an answer for a tab left meanwhile is dropped.
    unsigned tabGeneration_ = 0;
    // Shorts stand upright, six to a row; everything else three to a row.
    bool vertical_ = false;
    // A playlist's page: its panel on the left, the videos as rows.
    std::optional<newpipe::PlaylistInfo> playlist_;

    BRLS_BIND(brls::Box, holderBox, "feed/holder");
    BRLS_BIND(brls::Label, statusLabel, "feed/status");
    BRLS_BIND(brls::ScrollingFrame, scrollFrame, "feed/scroll");
    BRLS_BIND(brls::Box, channelBox, "feed/channel");
    BRLS_BIND(brls::Image, bannerImage, "feed/banner");
    BRLS_BIND(brls::Image, avatarImage, "feed/avatar");
    BRLS_BIND(brls::Label, channelNameLabel, "feed/channel_name");
    BRLS_BIND(brls::Label, channelMetaLabel, "feed/channel_meta");
    BRLS_BIND(brls::Label, channelDescriptionLabel, "feed/channel_description");
    BRLS_BIND(brls::Box, tabsBox, "feed/tabs");
    BRLS_BIND(brls::Label, messageLabel, "feed/message");
    BRLS_BIND(brls::Box, gridBox, "feed/grid");
    BRLS_BIND(brls::Box, playlistBox, "feed/playlist");
    BRLS_BIND(brls::Image, playlistCover, "feed/playlist_cover");
    BRLS_BIND(brls::Label, playlistTitle, "feed/playlist_title");
    BRLS_BIND(brls::Label, playlistOwner, "feed/playlist_owner");
    BRLS_BIND(brls::Label, playlistMeta, "feed/playlist_meta");
    BRLS_BIND(brls::Box, playlistActions, "feed/playlist_actions");
    BRLS_BIND(brls::Label, playlistDescription, "feed/playlist_description");
};
