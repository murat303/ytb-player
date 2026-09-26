#pragma once

#include <atomic>
#include <string>
#include <vector>

#include <borealis.hpp>

#include "newpipe/models.hpp"
#include "newpipe/youtube_catalog_service.hpp"
#include "view/auto_tab_frame.hpp"

class PageHeader;

// The Library in two panes, as in YouTube's tablet app: its parts on the left (History and
// Favorites from the SD card, the account's Watch later, Liked videos and playlists), the
// chosen part's videos on the right as rows.
class LibraryTab : public AttachedView {
public:
    LibraryTab();

    void onCreate() override;
    // Entering the tab lands on the videos; the parts are one step left.
    brls::View* getDefaultFocus() override;

    static brls::View* create() { return new LibraryTab(); }

private:
    // A part of the library: "history", "favorites", an account list ("WL", "LL") or the id of
    // one of the account's playlists.
    struct Section {
        std::string key;
        std::string title;
        std::string icon;
    };

    void refresh();
    void loadAccountList(const std::string& playlist_id);
    void loadPlaylists();
    void reloadPlaylists();
    void addSectionItem(size_t index);
    void updateSections();
    void selectSection(size_t index);
    size_t sectionIndex() const;
    void showMessage(const std::string& text);
    void buildGrid();
    brls::View* cardAt(size_t index);
    void restoreFocus();
    void stepSection(int delta);
    void confirmClear();
    void clearCurrentSection();
    bool allowInitialInput() const;
    void playStream(const newpipe::StreamItem& item);
    void openStream(const newpipe::StreamItem& item);

    BRLS_BIND(PageHeader, header, "library/header");
    BRLS_BIND(brls::Box, sectionsBox, "library/sections");
    BRLS_BIND(brls::Label, bodyLabel, "library/body");
    BRLS_BIND(brls::ProgressSpinner, spinner, "library/spinner");
    BRLS_BIND(brls::ScrollingFrame, scrollFrame, "library/scroll");
    BRLS_BIND(brls::Box, gridBox, "library/grid");

    newpipe::YouTubeCatalogService service_;
    std::vector<newpipe::StreamItem> items_;
    std::vector<Section> sections_;
    std::vector<brls::Box*> sectionItems_;  // one per section, in the left pane
    std::string sectionKey_ = "history";
    std::string focusVideoId_;  // back from a video: the card to focus once the list is built
    unsigned generation_ = 0;  // a late account list of an earlier section is dropped
    std::atomic<bool> interactionReady_{false};
};
