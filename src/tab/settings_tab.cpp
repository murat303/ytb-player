#include "tab/settings_tab.hpp"

#include "newpipe/auth_store.hpp"
#include "newpipe/i18n.hpp"
#include "newpipe/library_store.hpp"
#include "newpipe/log.hpp"
#include "newpipe/settings_store.hpp"
#include "newpipe/youtube_catalog_service.hpp"
#include "view/svg_image.hpp"

namespace {

int language_selection(const std::string& value) {
    if (value == "ko") {
        return 1;
    }
    if (value == "en-US") {
        return 2;
    }
    if (value == "tr") {
        return 3;
    }
    return 0;
}

std::string language_from_selection(int selection) {
    switch (selection) {
        case 1:
            return "ko";
        case 2:
            return "en-US";
        case 3:
            return "tr";
        default:
            return "auto";
    }
}

int playback_quality_selection(newpipe::PlaybackQualityMode mode) {
    switch (mode) {
        case newpipe::PlaybackQualityMode::BEST:
            return 0;
        case newpipe::PlaybackQualityMode::HD_1080:
            return 1;
        case newpipe::PlaybackQualityMode::HD_720:
            return 2;
        case newpipe::PlaybackQualityMode::LOW_320:
            return 3;
        default:
            return 0;
    }
}

newpipe::PlaybackQualityMode playback_quality_from_selection(int selection) {
    switch (selection) {
        case 1:
            return newpipe::PlaybackQualityMode::HD_1080;
        case 2:
            return newpipe::PlaybackQualityMode::HD_720;
        case 3:
            return newpipe::PlaybackQualityMode::LOW_320;
        default:
            return newpipe::PlaybackQualityMode::BEST;
    }
}

int startup_tab_selection(const std::string& value) {
    if (value == "search") {
        return 1;
    }
    if (value == "subscriptions") {
        return 2;
    }
    if (value == "library") {
        return 3;
    }
    if (value == "settings") {
        return 4;
    }
    return 0;
}

std::string startup_tab_from_selection(int selection) {
    switch (selection) {
        case 1:
            return "search";
        case 2:
            return "subscriptions";
        case 3:
            return "library";
        case 4:
            return "settings";
        default:
            return "home";
    }
}

int home_kiosk_selection(const std::string& value) {
    if (value == "shorts") {
        return 4;
    }
    if (value == "live") {
        return 1;
    }
    if (value == "music") {
        return 2;
    }
    if (value == "gaming") {
        return 3;
    }
    return 0;
}

std::string home_kiosk_from_selection(int selection) {
    switch (selection) {
        case 1:
            return "live";
        case 2:
            return "music";
        case 3:
            return "gaming";
        case 4:
            return "shorts";
        default:
            return "recommended";
    }
}

SettingsTab*& current_tab() {
    static SettingsTab* tab = nullptr;
    return tab;
}

// Asked for before the tab was built (the tab frame builds a tab when it is first shown).
bool& account_requested() {
    static bool requested = false;
    return requested;
}

constexpr size_t kAccountCategory = 2;

}  // namespace

SettingsTab::~SettingsTab() {
    if (current_tab() == this) {
        current_tab() = nullptr;
    }
}

void SettingsTab::showAccount() {
    if (SettingsTab* tab = current_tab()) {
        tab->selectCategory(kAccountCategory);
    } else {
        account_requested() = true;
    }
}

SettingsTab::SettingsTab() {
    current_tab() = this;
    this->inflateFromXMLRes("xml/tabs/settings.xml");

    this->registerAction(newpipe::tr("settings/reset_action"), brls::ControllerButton::BUTTON_X, [this](brls::View*) {
        this->confirmReset();
        return true;
    });

    this->syncLocalizedText();

    this->languageCell->init(
        newpipe::tr("settings/language/title"),
        {
            newpipe::tr("settings/language/options/auto"),
            newpipe::tr("settings/language/options/korean"),
            newpipe::tr("settings/language/options/english"),
            newpipe::tr("settings/language/options/turkish"),
        },
        0,
        [this](int selection) {
            std::string error;
            if (!newpipe::SettingsStore::instance().update_language(
                    language_from_selection(selection), &error)) {
                brls::Application::notify(
                    error.empty() ? newpipe::tr("settings/language/save_failed") : error);
                this->syncFromStore();
                return;
            }
            brls::Application::notify(newpipe::tr("settings/language/saved"));
        });

    this->playbackQualityCell->init(
        newpipe::tr("settings/playback_quality/title"),
        {
            newpipe::tr("settings/playback_quality/options/best"),
            newpipe::tr("settings/playback_quality/options/hd_1080"),
            newpipe::tr("settings/playback_quality/options/hd_720"),
            newpipe::tr("settings/playback_quality/options/low_320"),
        },
        0,
        [this](int selection) {
            std::string error;
            if (!newpipe::SettingsStore::instance().update_playback_quality(
                    playback_quality_from_selection(selection), &error)) {
                brls::Application::notify(
                    error.empty() ? newpipe::tr("settings/playback_quality/save_failed") : error);
                this->syncFromStore();
                return;
            }
            brls::Application::notify(newpipe::tr("settings/playback_quality/saved"));
        });

    this->startupTabCell->init(
        newpipe::tr("settings/startup_tab/title"),
        {
            newpipe::tr("app/home"),
            newpipe::tr("app/search"),
            newpipe::tr("app/subscriptions"),
            newpipe::tr("app/library"),
            newpipe::tr("app/settings"),
        },
        0,
        [this](int selection) {
            std::string error;
            if (!newpipe::SettingsStore::instance().update_startup_tab(
                    startup_tab_from_selection(selection), &error)) {
                brls::Application::notify(
                    error.empty() ? newpipe::tr("settings/startup_tab/save_failed") : error);
                this->syncFromStore();
                return;
            }
            brls::Application::notify(newpipe::tr("settings/startup_tab/saved"));
        });

    this->homeKioskCell->init(
        newpipe::tr("settings/home_kiosk/title"),
        {
            newpipe::tr("settings/home_kiosk/options/recommended"),
            newpipe::tr("settings/home_kiosk/options/live"),
            newpipe::tr("settings/home_kiosk/options/music"),
            newpipe::tr("settings/home_kiosk/options/gaming"),
            newpipe::tr("settings/home_kiosk/options/shorts"),
        },
        0,
        [this](int selection) {
            std::string error;
            if (!newpipe::SettingsStore::instance().update_home_kiosk(
                    home_kiosk_from_selection(selection), &error)) {
                brls::Application::notify(
                    error.empty() ? newpipe::tr("settings/home_kiosk/save_failed") : error);
                this->syncFromStore();
                return;
            }
            brls::Application::notify(newpipe::tr("settings/home_kiosk/saved"));
        });

    this->hideShortsCell->init(newpipe::tr("settings/hide_shorts/title"), false, [this](bool value) {
        std::string error;
        if (!newpipe::SettingsStore::instance().update_hide_short_videos(value, &error)) {
            brls::Application::notify(
                error.empty() ? newpipe::tr("settings/hide_shorts/save_failed") : error);
            this->syncFromStore();
            return;
        }
        brls::Application::notify(
            value ? newpipe::tr("settings/hide_shorts/enabled")
                  : newpipe::tr("settings/hide_shorts/disabled"));
    });

    this->hardwareDecodingCell->init(newpipe::tr("settings/hardware_decoding/title"), false, [this](bool value) {
        std::string error;
        if (!newpipe::SettingsStore::instance().update_hardware_decoding(value, &error)) {
            brls::Application::notify(
                error.empty() ? newpipe::tr("settings/hardware_decoding/save_failed") : error);
            this->syncFromStore();
            return;
        }
        brls::Application::notify(
            value ? newpipe::tr("settings/hardware_decoding/enabled")
                  : newpipe::tr("settings/hardware_decoding/disabled"));
    });

    this->autoplayNextCell->init(newpipe::tr("settings/autoplay_next/title"), true, [this](bool value) {
        std::string error;
        if (!newpipe::SettingsStore::instance().update_autoplay_next(value, &error)) {
            brls::Application::notify(
                error.empty() ? newpipe::tr("settings/autoplay_next/save_failed") : error);
            this->syncFromStore();
            return;
        }
        brls::Application::notify(
            value ? newpipe::tr("settings/autoplay_next/enabled")
                  : newpipe::tr("settings/autoplay_next/disabled"));
    });

    this->skipSponsorsCell->init(newpipe::tr("settings/skip_sponsors/title"), true, [this](bool value) {
        std::string error;
        if (!newpipe::SettingsStore::instance().update_skip_sponsors(value, &error)) {
            brls::Application::notify(
                error.empty() ? newpipe::tr("settings/skip_sponsors/save_failed") : error);
            this->syncFromStore();
            return;
        }
        brls::Application::notify(
            value ? newpipe::tr("settings/skip_sponsors/enabled")
                  : newpipe::tr("settings/skip_sponsors/disabled"));
    });

    this->subtitlesCell->init(newpipe::tr("settings/subtitles/title"), false, [this](bool value) {
        std::string error;
        if (!newpipe::SettingsStore::instance().update_subtitles_enabled(value, &error)) {
            brls::Application::notify(
                error.empty() ? newpipe::tr("settings/subtitles/save_failed") : error);
            this->syncFromStore();
            return;
        }
        brls::Application::notify(
            value ? newpipe::tr("settings/subtitles/enabled")
                  : newpipe::tr("settings/subtitles/disabled"));
    });

    this->sessionCell->setText(newpipe::tr("settings/session/title"));
    this->sessionCell->registerClickAction([this](brls::View*) {
        this->openSessionInfo();
        return true;
    });

    this->accountCell->setText(newpipe::tr("settings/account/title"));
    this->accountCell->registerClickAction([this](brls::View*) {
        this->openAccountPicker();
        return true;
    });

    this->storageCell->setText(newpipe::tr("settings/storage/title"));
    this->storageCell->registerClickAction([this](brls::View*) {
        this->openStorageInfo();
        return true;
    });

    this->buildCategories();
    this->syncFromStore();
}

// The kinds of settings as items with a coloured icon; A or a tap shows that kind's cells.
void SettingsTab::buildCategories() {
    const auto group = [this](const char* id) { return dynamic_cast<brls::Box*>(this->getView(id)); };
    this->categories_ = {
        {newpipe::tr("settings/categories/general"), "svg/settings_general.svg", group("settings/group_general")},
        {newpipe::tr("settings/categories/video"), "svg/settings_video.svg", group("settings/group_video")},
        {newpipe::tr("settings/categories/account"), "svg/settings_account.svg", group("settings/group_account")},
        {newpipe::tr("settings/categories/storage"), "svg/settings_storage.svg", group("settings/group_storage")},
    };
    if (!this->categoriesBox) {
        return;
    }
    for (size_t i = 0; i < this->categories_.size(); i++) {
        auto* item = new brls::Box(brls::Axis::ROW);
        item->setFocusable(true);
        item->setAlignItems(brls::AlignItems::CENTER);
        item->setPadding(12, 14, 12, 14);
        item->setCornerRadius(14);
        item->setHighlightCornerRadius(14);
        item->setMarginBottom(4);

        auto* icon = new SVGImage();
        icon->setDimensions(32, 32);
        icon->setImageFromSVGRes(this->categories_[i].icon);
        item->addView(icon);

        auto* label = new brls::Label();
        label->setFontSize(17);
        label->setSingleLine(true);
        label->setWidth(230);
        label->setTextColor(nvgRGB(0xF1, 0xF1, 0xF1));
        label->setText(this->categories_[i].title);
        label->setMarginLeft(16);
        item->addView(label);

        item->registerClickAction([this, i](brls::View*) {
            this->selectCategory(i);
            return true;
        });
        item->addGestureRecognizer(new brls::TapGestureRecognizer(item));
        this->categoriesBox->addView(item);
        this->categoryItems_.push_back(item);
    }
    this->selectCategory(account_requested() ? kAccountCategory : 0);
    account_requested() = false;
}

void SettingsTab::selectCategory(size_t index) {
    if (index >= this->categories_.size()) {
        return;
    }
    this->category_ = index;
    for (size_t i = 0; i < this->categories_.size(); i++) {
        if (this->categories_[i].group) {
            this->categories_[i].group->setVisibility(i == index ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
        }
        if (i < this->categoryItems_.size()) {
            this->categoryItems_[i]->setBackgroundColor(i == index ? nvgRGB(0x27, 0x27, 0x27) : nvgRGBA(0, 0, 0, 0));
        }
    }
    // Left from the cells comes back to this item.
    if (this->categoriesBox && index < this->categoryItems_.size()) {
        this->categoriesBox->setLastFocusedView(this->categoryItems_[index]);
    }
    if (this->categoryTitle) {
        this->categoryTitle->setText(this->categories_[index].title);
    }
}

void SettingsTab::syncLocalizedText() {
    if (this->languageCell) {
        this->languageCell->setText(newpipe::tr("settings/language/title"));
    }
    if (this->playbackQualityCell) {
        this->playbackQualityCell->setText(newpipe::tr("settings/playback_quality/title"));
    }
    if (this->startupTabCell) {
        this->startupTabCell->setText(newpipe::tr("settings/startup_tab/title"));
    }
    if (this->homeKioskCell) {
        this->homeKioskCell->setText(newpipe::tr("settings/home_kiosk/title"));
    }
    if (this->hideShortsCell) {
        this->hideShortsCell->setText(newpipe::tr("settings/hide_shorts/title"));
    }
    if (this->hardwareDecodingCell) {
        this->hardwareDecodingCell->setText(newpipe::tr("settings/hardware_decoding/title"));
    }
    if (this->autoplayNextCell) {
        this->autoplayNextCell->setText(newpipe::tr("settings/autoplay_next/title"));
    }
    if (this->skipSponsorsCell) {
        this->skipSponsorsCell->setText(newpipe::tr("settings/skip_sponsors/title"));
    }
    if (this->subtitlesCell) {
        this->subtitlesCell->setText(newpipe::tr("settings/subtitles/title"));
    }
    if (this->sessionCell) {
        this->sessionCell->setText(newpipe::tr("settings/session/title"));
    }
    if (this->accountCell) {
        this->accountCell->setText(newpipe::tr("settings/account/title"));
    }
    if (this->storageCell) {
        this->storageCell->setText(newpipe::tr("settings/storage/title"));
    }
}

void SettingsTab::syncFromStore() {
    std::string error;
    newpipe::SettingsStore::instance().load(&error);
    const newpipe::AppSettings settings = newpipe::SettingsStore::instance().settings();

    this->languageCell->setSelection(language_selection(settings.language), true);
    this->playbackQualityCell->setSelection(
        playback_quality_selection(settings.playback_quality), true);
    this->startupTabCell->setSelection(startup_tab_selection(settings.startup_tab), true);
    this->homeKioskCell->setSelection(home_kiosk_selection(settings.home_kiosk), true);
    this->hideShortsCell->setOn(settings.hide_short_videos, false);
    this->hardwareDecodingCell->setOn(settings.hardware_decoding, false);
    this->autoplayNextCell->setOn(settings.autoplay_next, false);
    this->skipSponsorsCell->setOn(settings.skip_sponsors, false);
    this->subtitlesCell->setOn(settings.subtitles_enabled, false);

    if (!newpipe::AuthStore::instance().load(&error) && !error.empty()) {
        this->sessionCell->setDetailText(newpipe::tr("settings/session/load_failed"));
    } else if (newpipe::AuthStore::instance().has_session()) {
        const auto session = newpipe::AuthStore::instance().session();
        this->sessionCell->setDetailText(
            session.display_name.empty() ? newpipe::tr("settings/session/saved")
                                         : session.display_name);
    } else {
        this->sessionCell->setDetailText(newpipe::tr("settings/session/signed_out"));
    }
    const auto session = newpipe::AuthStore::instance().session();
    this->accountCell->setDetailText(
        !session.authenticated() ? newpipe::tr("settings/session/signed_out")
            : session.channel_name.empty() ? newpipe::tr("settings/account/default")
                                           : session.channel_name);

    this->storageCell->setDetailText(newpipe::tr("settings/storage/detail"));
}

// X: every setting back to its default, after a yes.
void SettingsTab::confirmReset() {
    auto* dialog = new brls::Dialog(newpipe::tr("settings/reset_confirm"));
    dialog->addButton(newpipe::tr("hints/cancel"), []() {});
    dialog->addButton(newpipe::tr("settings/reset_action"), [this]() {
        this->resetToDefaults();
    });
    dialog->setCancelable(true);
    dialog->open();
}

void SettingsTab::resetToDefaults() {
    std::string error;
    if (!newpipe::SettingsStore::instance().reset(&error)) {
        brls::Application::notify(error.empty() ? newpipe::tr("settings/reset_failed") : error);
        return;
    }

    this->syncFromStore();
    brls::Application::notify(newpipe::tr("settings/reset_done"));
}

void SettingsTab::openSessionInfo() {
    std::string error;
    newpipe::AuthStore::instance().load(&error);
    const auto session = newpipe::AuthStore::instance().session();

    std::string body;
    if (!error.empty()) {
        body = newpipe::tr("settings/session/dialog/load_failed", error);
    } else if (!newpipe::AuthStore::instance().has_session()) {
        body = newpipe::tr("settings/session/dialog/signed_out");
    } else {
        body = newpipe::tr(
            "settings/session/dialog/saved",
            session.display_name.empty() ? newpipe::tr("common/none") : session.display_name,
            newpipe::default_auth_session_path());
        if (!session.source_path.empty()) {
            body += "\n" + newpipe::tr("settings/session/dialog/source", session.source_path);
        }
    }

    auto* dialog = new brls::Dialog(body);
    dialog->addButton(newpipe::tr("common/close"), []() {});
    dialog->setCancelable(true);
    dialog->open();
}

// The channels of the signed-in Google account, fetched from YouTube; the picked one is kept
// with the session and every request acts for it (Subscriptions, Watch later, Liked videos).
void SettingsTab::openAccountPicker() {
    if (!newpipe::AuthStore::instance().has_session()) {
        brls::Application::notify(newpipe::tr("settings/account/login_required"));
        return;
    }
    brls::Application::notify(newpipe::tr("settings/account/loading"));
    ASYNC_RETAIN
    brls::async([ASYNC_TOKEN]() {
        // Its own service instance: the UI thread never touches this one.
        newpipe::YouTubeCatalogService loader;
        const auto accounts = loader.list_accounts();
        brls::sync([ASYNC_TOKEN, accounts]() {
            ASYNC_RELEASE
            if (accounts.empty()) {
                brls::Application::notify(newpipe::tr("settings/account/failed"));
                return;
            }
            const auto session = newpipe::AuthStore::instance().session();
            std::vector<std::string> names;
            int selected = 0;
            for (size_t i = 0; i < accounts.size(); i++) {
                const auto& account = accounts[i];
                names.push_back(account.handle.empty() || account.handle == account.name
                                    ? account.name
                                    : account.name + "  (" + account.handle + ")");
                const bool current = session.page_id.empty() ? account.selected : account.page_id == session.page_id;
                if (current) {
                    selected = static_cast<int>(i);
                }
            }
            auto* dropdown = new brls::Dropdown(
                newpipe::tr("settings/account/picker_title"), names,
                [this, accounts](int index) {
                    if (index < 0 || index >= static_cast<int>(accounts.size())) {
                        return;
                    }
                    const auto& account = accounts[index];
                    std::string error;
                    if (!newpipe::AuthStore::instance().set_identity(account.page_id, account.name, account.photo_url, &error)) {
                        brls::Application::notify(error.empty() ? newpipe::tr("settings/account/failed") : error);
                        return;
                    }
                    this->syncFromStore();
                    brls::Application::notify(newpipe::tr("settings/account/chosen", account.name));
                },
                selected);
            brls::Application::pushActivity(new brls::Activity(dropdown));
        });
    });
}

void SettingsTab::openStorageInfo() {
    const std::string body = newpipe::tr(
        "settings/storage/dialog/body",
        newpipe::default_settings_store_path(),
        newpipe::default_auth_session_path(),
        newpipe::default_auth_import_path(),
        newpipe::default_library_store_path());

    auto* dialog = new brls::Dialog(body);
    dialog->addButton(newpipe::tr("common/close"), []() {});
    dialog->setCancelable(true);
    dialog->open();
}
