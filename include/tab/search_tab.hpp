#pragma once

#include <atomic>
#include <string>
#include <vector>

#include <borealis.hpp>

#include "newpipe/models.hpp"
#include "newpipe/youtube_catalog_service.hpp"
#include "view/auto_tab_frame.hpp"
#include "view/stream_grid.hpp"

class StreamCard;
class SVGImage;

class SearchTab : public AttachedView {
public:
    SearchTab();

    void onCreate() override;
    // B moves focus to the sidebar; coming back must land on the card the user left,
    // not on the first one at the top of a long list. Without results: the search box.
    brls::View* getDefaultFocus() override;

    static brls::View* create() { return new SearchTab(); }

private:
    void openKeyboard();
    void doSearch(const std::string& query);
    void showQuery();
    void buildHistory();
    void showStatus(const std::string& text);
    void setupCard(StreamCard* card, size_t index);
    void updateCount();
    bool allowInitialInput() const;
    void playStream(const newpipe::StreamItem& item);
    void openStream(const newpipe::StreamItem& item);

    BRLS_BIND(brls::Box, searchBar, "search/bar");
    BRLS_BIND(SVGImage, searchIcon, "search/bar_icon");
    BRLS_BIND(brls::Label, searchText, "search/bar_text");
    BRLS_BIND(brls::Box, historyBox, "search/history");
    BRLS_BIND(brls::Label, statusLabel, "search/status");
    BRLS_BIND(brls::ProgressSpinner, spinner, "search/spinner");
    BRLS_BIND(brls::ScrollingFrame, scrollFrame, "search/scroll");
    BRLS_BIND(brls::Box, gridBox, "search/grid");

    newpipe::YouTubeCatalogService service_;
    StreamGrid grid_;
    std::string lastQuery_;
    unsigned searchGeneration_ = 0;  // a late answer to an earlier query is dropped
    std::atomic<bool> interactionReady_{false};
};
