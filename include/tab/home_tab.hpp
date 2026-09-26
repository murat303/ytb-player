#pragma once

#include <atomic>
#include <string>
#include <vector>

#include <borealis.hpp>

#include "newpipe/models.hpp"
#include "newpipe/settings_store.hpp"
#include "newpipe/youtube_catalog_service.hpp"
#include "view/auto_tab_frame.hpp"
#include "view/chip.hpp"
#include "view/stream_grid.hpp"

class StreamCard;

class HomeTab : public AttachedView {
public:
    HomeTab();

    void onCreate() override;
    // B moves focus to the sidebar; coming back must land on the card the user left,
    // not on the first one at the top of a long list.
    brls::View* getDefaultFocus() override;

    static brls::View* create() { return new HomeTab(); }

private:
    void buildChips();
    void selectKiosk(size_t index);
    void loadHome();
    void scheduleLoadHome(long delay_ms);
    void setupCard(StreamCard* card, size_t index);
    void showStatus(const std::string& text);
    void updateStatus();
    std::string kioskTitle(const newpipe::Kiosk& kiosk) const;
    void stepKiosk(int delta);
    bool allowInitialInput() const;
    void playStream(const newpipe::StreamItem& item);
    void openStream(const newpipe::StreamItem& item);

    BRLS_BIND(brls::Box, chipsBox, "home/chips");
    BRLS_BIND(brls::Label, statusLabel, "home/status");
    BRLS_BIND(brls::ProgressSpinner, spinner, "home/spinner");
    BRLS_BIND(brls::ScrollingFrame, scrollFrame, "home/scroll");
    BRLS_BIND(brls::Box, gridBox, "home/grid");

    newpipe::YouTubeCatalogService service_;
    StreamGrid grid_;
    std::vector<newpipe::Kiosk> kiosks_;
    std::vector<Chip*> chips_;  // one per category, above the grid
    size_t kioskIndex_ = 0;
    unsigned loadGeneration_ = 0;  // a late feed of an earlier category is dropped
    bool initialLoadCompleted_ = false;
    int initialLoadAttempts_ = 0;
    std::atomic<bool> interactionReady_{false};
};
