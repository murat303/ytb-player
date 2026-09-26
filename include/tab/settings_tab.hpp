#pragma once

#include <string>
#include <vector>

#include <borealis.hpp>

class SettingsTab : public brls::Box {
public:
    SettingsTab();
    ~SettingsTab() override;

    // The header's account picture: the Account part of the settings (the tab is built at
    // start, so the one on screen answers).
    static void showAccount();

    static brls::View* create() { return new SettingsTab(); }

private:
    // A kind of settings in the left pane and the box of its cells on the right.
    struct Category {
        std::string title;
        std::string icon;
        brls::Box* group = nullptr;
    };

    void buildCategories();
    void selectCategory(size_t index);
    void syncLocalizedText();
    void syncFromStore();
    void confirmReset();
    void resetToDefaults();
    void openSessionInfo();
    void openAccountPicker();
    void openStorageInfo();

    BRLS_BIND(brls::SelectorCell, languageCell, "settings/language");
    BRLS_BIND(brls::SelectorCell, playbackQualityCell, "settings/playback_quality");
    BRLS_BIND(brls::SelectorCell, startupTabCell, "settings/startup_tab");
    BRLS_BIND(brls::SelectorCell, homeKioskCell, "settings/home_kiosk");
    BRLS_BIND(brls::BooleanCell, hideShortsCell, "settings/hide_shorts");
    BRLS_BIND(brls::BooleanCell, hardwareDecodingCell, "settings/hardware_decoding");
    BRLS_BIND(brls::BooleanCell, autoplayNextCell, "settings/autoplay_next");
    BRLS_BIND(brls::BooleanCell, skipSponsorsCell, "settings/skip_sponsors");
    BRLS_BIND(brls::BooleanCell, subtitlesCell, "settings/subtitles");
    BRLS_BIND(brls::DetailCell, sessionCell, "settings/session");
    BRLS_BIND(brls::DetailCell, accountCell, "settings/account");
    BRLS_BIND(brls::DetailCell, storageCell, "settings/storage");
    BRLS_BIND(brls::Box, categoriesBox, "settings/categories");
    BRLS_BIND(brls::Label, categoryTitle, "settings/category_title");

    std::vector<Category> categories_;
    std::vector<brls::Box*> categoryItems_;
    size_t category_ = 0;
};
