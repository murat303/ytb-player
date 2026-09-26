#pragma once

#include <atomic>
#include <string>
#include <vector>

#include <borealis.hpp>

#include "newpipe/models.hpp"
#include "newpipe/youtube_catalog_service.hpp"
#include "view/auto_tab_frame.hpp"
#include "view/chip.hpp"
#include "view/stream_grid.hpp"

class StreamCard;

class SubscriptionsTab : public AttachedView {
public:
    SubscriptionsTab();

    void onCreate() override;
    void onShow() override;
    // B moves focus to the sidebar; coming back must land on the card the user left,
    // not on the first one at the top of a long list.
    brls::View* getDefaultFocus() override;

    static brls::View* create() { return new SubscriptionsTab(); }

private:
    // The chips over the feed, as on YouTube: all, today's, videos, live, Shorts.
    enum class Filter { all, today, videos, live, shorts };

    void refresh();
    void saveForReturn();
    void showFeed(const std::vector<newpipe::StreamItem>& items);
    void buildChannels();
    void buildChips();
    void applyFilter(Filter filter);
    void stepFilter(int delta);
    void setupCard(StreamCard* card, size_t index);
    void updateCount();
    void showSessionBody();
    void clearGrid();
    void showSignedOutState();
    void openSessionDialog();
    bool allowInitialInput() const;
    void playStream(const newpipe::StreamItem& item);
    void openStream(const newpipe::StreamItem& item);
    void openChannel(const newpipe::StreamItem& item);

    BRLS_BIND(brls::HScrollingFrame, channelsScroll, "subscriptions/channels_scroll");
    BRLS_BIND(brls::Box, channelsBox, "subscriptions/channels");
    BRLS_BIND(brls::Box, chipsBox, "subscriptions/chips");
    BRLS_BIND(brls::Label, statusLabel, "subscriptions/status");
    BRLS_BIND(brls::Label, bodyLabel, "subscriptions/body");
    BRLS_BIND(brls::ProgressSpinner, spinner, "subscriptions/spinner");
    BRLS_BIND(brls::ScrollingFrame, scrollFrame, "subscriptions/scroll");
    BRLS_BIND(brls::Box, gridBox, "subscriptions/grid");

    newpipe::YouTubeCatalogService service_;
    StreamGrid grid_;
    std::vector<newpipe::StreamItem> allItems_;  // the whole feed; the grid shows the filtered part
    std::string nextPageToken_;
    bool nextPageUsesSearch_ = false;
    bool nextPageAllowsShorts_ = false;
    Filter filter_ = Filter::all;
    std::vector<Chip*> chips_;
    std::string statusTitle_;
    bool loadRequested_ = false;
    unsigned loadGeneration_ = 0;  // a late feed of an earlier refresh is dropped
    std::atomic<bool> interactionReady_{false};
};
