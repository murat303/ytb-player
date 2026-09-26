#include "tab/library_tab.hpp"

#include <cstdlib>

#include "view/stream_grid.hpp"

#include "activity/stream_detail_activity.hpp"
#include "newpipe/auth_store.hpp"
#include "newpipe/i18n.hpp"
#include "newpipe/library_store.hpp"
#include "newpipe/log.hpp"
#include "newpipe/playback_helper.hpp"
#include "newpipe/runtime.hpp"
#include "view/page_header.hpp"
#include "view/card_gesture.hpp"
#include "view/stream_card.hpp"
#include "view/svg_image.hpp"
#include "view/tab_focus.hpp"

namespace {
constexpr int kLibraryTabIndex = 3;
// History, Favorites, Watch later, Liked videos; the account's playlists follow them.
constexpr size_t kFixedSections = 4;
// The Playlists tab of a channel's page, for the desktop stand-in below.
constexpr const char* kPlaylistsTabParams = "EglwbGF5bGlzdHPyBgoKCEIGCgIQaCIA";

// The UI is rebuilt after a video. This keeps the part of the library and the video it was
// started from, so the return lands on that row (History moves it to the front, so it is
// found by id).
struct LibraryReturn {
    bool valid = false;
    std::string key;
    std::string video_id;
};

LibraryReturn& library_return() {
    static LibraryReturn state;
    return state;
}

// The account's playlists from the last visit: the rebuilt UI (after a video) shows them at
// once instead of waiting for YouTube again. They are the chosen channel's (owner).
struct KnownPlaylists {
    std::string owner;
    std::vector<newpipe::StreamItem> items;
};

KnownPlaylists& known_playlists() {
    static KnownPlaylists playlists;
    return playlists;
}

// Whose playlists a list would be: the session's picked channel.
std::string playlists_owner() {
    const auto session = newpipe::AuthStore::instance().session();
    return session.page_id + "|" + session.channel_name;
}

#ifndef __SWITCH__
// Desktop screenshot tests have no YouTube session: NEWPIPE_FAKE_PLAYLISTS names a channel
// whose public playlists stand in for the account's.
const char* fake_playlists() {
    return std::getenv("NEWPIPE_FAKE_PLAYLISTS");
}
#else
const char* fake_playlists() {
    return nullptr;
}
#endif

bool is_account_list(const std::string& key) {
    return key == "WL" || key == "LL";
}
}  // namespace

LibraryTab::LibraryTab() : service_() {
    this->inflateFromXMLRes("xml/tabs/library.xml");
    ASYNC_RETAIN
    brls::delay(700, [ASYNC_TOKEN]() {
        ASYNC_RELEASE
        interactionReady_.store(true);
    });

    // Kept on the content only: clearing is destructive and needs no sidebar
    // shortcut, an empty section has nothing to clear anyway. It asks first.
    this->registerAction(newpipe::tr("library/clear_action"), brls::ControllerButton::BUTTON_BACK, [this](brls::View*) {
        this->confirmClear();
        return true;
    });

    LibraryReturn& back = library_return();
    if (back.valid) {
        back.valid = false;
        this->sectionKey_ = back.key;
        this->focusVideoId_ = back.video_id;
    }

    this->sections_ = {
        {"history", newpipe::tr("library/history"), "svg/history.svg"},
        {"favorites", newpipe::tr("library/favorites"), "svg/star.svg"},
        {"WL", newpipe::tr("library/watch_later"), "svg/watch_later.svg"},
        {"LL", newpipe::tr("library/liked"), "svg/liked.svg"},
    };
    for (size_t i = 0; i < this->sections_.size(); i++) {
        this->addSectionItem(i);
    }
    this->loadPlaylists();
    this->updateSections();
    this->refresh();
}

brls::View* LibraryTab::cardAt(size_t index) {
    if (!this->gridBox) {
        return nullptr;
    }
    const auto& rows = this->gridBox->getChildren();
    return index < rows.size() ? rows[index] : nullptr;
}

// Back from a video: focus the row of the video it was started from, once the list has it.
void LibraryTab::restoreFocus() {
    if (this->focusVideoId_.empty()) {
        return;
    }
    const std::string video_id = this->focusVideoId_;
    this->focusVideoId_.clear();
    for (size_t i = 0; i < this->items_.size(); i++) {
        if (this->items_[i].id != video_id) {
            continue;
        }
        // As StreamGrid::focusLater: the tab frame hands focus to its sidebar after the UI is
        // rebuilt, so the row is focused afterwards, and again if the sidebar took it back.
        for (const int delay_ms : {400, 1200}) {
            ASYNC_RETAIN
            brls::delay(delay_ms, [ASYNC_TOKEN, i, delay_ms]() {
                ASYNC_RELEASE
                if (delay_ms > 400 && brls::Application::getCurrentFocus() != this->getTabBar()) {
                    return;
                }
                brls::View* card = this->cardAt(i);
                if (!card) {
                    return;
                }
                if (this->scrollFrame && this->gridBox) {
                    const float top = this->gridBox->getLocalY() + card->getLocalY();
                    this->scrollFrame->setContentOffsetY(std::max(0.0f, top - 12.0f), false);
                }
                brls::Application::giveFocus(card);
            });
        }
        return;
    }
}

brls::View* LibraryTab::getDefaultFocus() {
    if (this->gridBox) {
        if (brls::View* card = this->gridBox->getDefaultFocus()) {
            return card;
        }
    }
    const size_t index = this->sectionIndex();
    if (index < this->sectionItems_.size()) {
        return this->sectionItems_[index];
    }
    return AttachedView::getDefaultFocus();
}

// A part in the left pane: its icon and name; A or a tap opens it.
void LibraryTab::addSectionItem(size_t index) {
    if (!this->sectionsBox || index >= this->sections_.size()) {
        return;
    }
    const Section& section = this->sections_[index];
    auto* item = new brls::Box(brls::Axis::ROW);
    item->setFocusable(true);
    item->setAlignItems(brls::AlignItems::CENTER);
    item->setPadding(11, 14, 11, 14);
    item->setCornerRadius(12);
    item->setHighlightCornerRadius(12);
    item->setMarginBottom(2);

    auto* icon = new SVGImage();
    icon->setDimensions(24, 24);
    icon->setImageFromSVGRes(section.icon);
    item->addView(icon);

    auto* label = new brls::Label();
    label->setFontSize(17);
    label->setSingleLine(true);
    label->setWidth(200);
    label->setTextColor(nvgRGB(0xF1, 0xF1, 0xF1));
    label->setText(section.title);
    label->setMarginLeft(16);
    item->addView(label);

    item->registerClickAction([this, index](brls::View*) {
        this->selectSection(index);
        return true;
    });
    item->addGestureRecognizer(new brls::TapGestureRecognizer(item));
    this->sectionsBox->addView(item);
    this->sectionItems_.push_back(item);
}

// The account's playlists under the fixed parts, from YouTube on a worker (or the last visit's).
void LibraryTab::loadPlaylists() {
    const auto add = [this](const std::vector<newpipe::StreamItem>& playlists) {
        if (playlists.empty() || !this->sectionsBox || this->sections_.size() > kFixedSections) {
            return;
        }
        auto* heading = new brls::Label();
        heading->setFontSize(13);
        heading->setTextColor(nvgRGB(0xAA, 0xAA, 0xAA));
        heading->setText(newpipe::tr("library/playlists"));
        heading->setMargins(14, 14, 6, 14);
        this->sectionsBox->addView(heading);
        for (const auto& playlist : playlists) {
            this->sections_.push_back({playlist.id, playlist.title, "svg/playlist.svg"});
            this->addSectionItem(this->sections_.size() - 1);
        }
        this->updateSections();
        // Back from a video of a playlist: its name for the header now that it is known.
        if (this->sectionIndex() >= kFixedSections && this->sectionIndex() < this->sections_.size() && this->header) {
            this->header->setTitle(newpipe::tr("app/library") + "  •  " + this->sections_[this->sectionIndex()].title);
        }
    };
    const std::string owner = playlists_owner();
    if (!known_playlists().items.empty() && known_playlists().owner == owner) {
        add(known_playlists().items);
        return;
    }
    const std::string fake = fake_playlists() ? fake_playlists() : "";
    ASYNC_RETAIN
    brls::async([ASYNC_TOKEN, fake, add, owner]() {
        newpipe::YouTubeCatalogService loader;
        std::vector<newpipe::StreamItem> playlists;
        if (!fake.empty()) {
            if (const auto feed = loader.get_channel_tab(fake, kPlaylistsTabParams)) {
                playlists = feed->items;
            }
        } else if (loader.has_auth_session()) {
            playlists = loader.list_account_playlists();
        }
        brls::sync([ASYNC_TOKEN, playlists, add, owner]() {
            ASYNC_RELEASE
            known_playlists() = {owner, playlists};
            add(playlists);
        });
    });
}

// X: the playlists again from YouTube (another channel may have been picked in Settings). The
// open part stays open when it was a fixed one.
void LibraryTab::reloadPlaylists() {
    if (!this->sectionsBox || this->sections_.size() <= kFixedSections) {
        known_playlists() = {};
        this->loadPlaylists();
        return;
    }
    // The playlist items and their heading are the pane's last views; the focus leaves them.
    for (brls::View* view = brls::Application::getCurrentFocus(); view; view = view->getParent()) {
        if (view == this->sectionsBox) {
            brls::Application::giveFocus(this->sectionItems_.front());
            break;
        }
    }
    while (this->sectionsBox->getChildren().size() > kFixedSections) {
        this->sectionsBox->removeView(this->sectionsBox->getChildren().back());
    }
    this->sections_.resize(kFixedSections);
    this->sectionItems_.resize(kFixedSections);
    if (this->sectionIndex() >= kFixedSections) {
        this->sectionKey_ = "history";
    }
    this->updateSections();
    known_playlists() = {};
    this->loadPlaylists();
}

size_t LibraryTab::sectionIndex() const {
    for (size_t i = 0; i < this->sections_.size(); i++) {
        if (this->sections_[i].key == this->sectionKey_) {
            return i;
        }
    }
    return this->sections_.size();
}

// The open part has a gray background; right from the list comes back to it.
void LibraryTab::updateSections() {
    const size_t selected = this->sectionIndex();
    for (size_t i = 0; i < this->sectionItems_.size(); i++) {
        this->sectionItems_[i]->setBackgroundColor(i == selected ? nvgRGB(0x27, 0x27, 0x27) : nvgRGBA(0, 0, 0, 0));
    }
    if (this->sectionsBox && selected < this->sectionItems_.size()) {
        this->sectionsBox->setLastFocusedView(this->sectionItems_[selected]);
    }
}

void LibraryTab::selectSection(size_t index) {
    if (index >= this->sections_.size() || this->sections_[index].key == this->sectionKey_) {
        return;
    }
    this->sectionKey_ = this->sections_[index].key;
    this->updateSections();
    this->refresh();
}

void LibraryTab::onCreate() {
    // Mirrored on the sidebar item: an empty history/favorites list has no
    // focusable child, so a content-only action could never be triggered.
    this->registerTabAction(newpipe::tr("common/refresh"), brls::ControllerButton::BUTTON_X, [this](brls::View*) {
        this->reloadPlaylists();
        this->refresh();
        return true;
    });
    newpipe::register_tab_step(this, [this](int delta) { this->stepSection(delta); });
}

bool LibraryTab::allowInitialInput() const {
    return interactionReady_.load();
}

void LibraryTab::refresh() {
    this->generation_++;
    const size_t index = this->sectionIndex();
    if (this->header) {
        const std::string title = index < this->sections_.size() ? this->sections_[index].title : std::string();
        this->header->setTitle(newpipe::tr("app/library") + (title.empty() ? "" : "  •  " + title));
    }
    // Only the lists on the SD card can be cleared here.
    const bool local = this->sectionKey_ == "history" || this->sectionKey_ == "favorites";
    this->setActionAvailable(brls::ControllerButton::BUTTON_BACK, local);
    if (!local) {
        this->loadAccountList(this->sectionKey_);
        return;
    }

    const bool favorites = this->sectionKey_ == "favorites";
    std::string error;
    newpipe::LibraryStore::instance().load(&error);
    this->items_ = favorites ? newpipe::LibraryStore::instance().favorite_items()
                             : newpipe::LibraryStore::instance().history_items();
    this->buildGrid();
    this->restoreFocus();

    this->showMessage(this->items_.empty() ? (favorites ? newpipe::tr("library/favorites_empty")
                                                        : newpipe::tr("library/history_empty"))
                                           : std::string());
    if (this->spinner) {
        this->spinner->setVisibility(brls::Visibility::GONE);
    }
}

// Watch later, liked videos or a playlist of the signed-in account, loaded on a worker thread.
void LibraryTab::loadAccountList(const std::string& playlist_id) {
    this->items_.clear();
    this->buildGrid();
    if (this->spinner) {
        this->spinner->setVisibility(brls::Visibility::VISIBLE);
    }
    this->showMessage(newpipe::tr("library/loading"));
    const unsigned generation = this->generation_;
    const bool fake = fake_playlists() != nullptr && !is_account_list(playlist_id);
    ASYNC_RETAIN
    brls::async([ASYNC_TOKEN, generation, playlist_id, fake]() {
        // Its own service instance: the UI thread never touches this one.
        newpipe::YouTubeCatalogService loader;
        std::optional<newpipe::HomeFeed> feed;
        if (fake) {
            newpipe::StreamItem playlist;
            playlist.url = "https://www.youtube.com/playlist?list=" + playlist_id;
            feed = loader.get_playlist_feed(playlist);
        } else {
            feed = loader.get_account_playlist(playlist_id);
        }
        const bool signed_in = fake || loader.has_auth_session();
        const std::string error = loader.error_message();
        brls::sync([ASYNC_TOKEN, generation, feed, signed_in, error]() {
            ASYNC_RELEASE
            if (generation != this->generation_) {
                return;
            }
            if (this->spinner) {
                this->spinner->setVisibility(brls::Visibility::GONE);
            }
            this->items_ = feed.has_value() ? feed->items : std::vector<newpipe::StreamItem>();
            this->buildGrid();
            this->restoreFocus();
            if (!signed_in) {
                this->showMessage(newpipe::tr("library/login_required"));
            } else if (!feed.has_value()) {
                this->showMessage(error.empty() ? newpipe::tr("library/account_failed") : error);
            } else {
                this->showMessage(this->items_.empty() ? newpipe::tr("library/account_empty") : std::string());
            }
        });
    });
}

// The header names the part, as on YouTube; the text over the list only speaks for an empty or
// failed one (the bottom bar shows the buttons).
void LibraryTab::showMessage(const std::string& text) {
    if (this->bodyLabel) {
        this->bodyLabel->setText(text);
        this->bodyLabel->setVisibility(text.empty() ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
    }
}

// The videos as rows: the thumbnail on the left, the title, views and channel beside it.
void LibraryTab::buildGrid() {
    if (!this->gridBox) {
        return;
    }

    newpipe::release_grid_focus(this, this->gridBox);
    this->gridBox->clearViews();
    if (this->scrollFrame) {
        this->scrollFrame->setContentOffsetY(0, false);
    }
    for (size_t j = 0; j < this->items_.size(); j++) {
        auto* card = new StreamCard(true);
        card->setData(this->items_[j]);
        register_play_action(card, [this, j](brls::View*) {
            this->playStream(this->items_[j]);
            return true;
        });
        card->registerAction(newpipe::tr("common/info"), brls::ControllerButton::BUTTON_Y, [this, j](brls::View*) {
            this->openStream(this->items_[j]);
            return true;
        });
        card->addGestureRecognizer(new CardGesture(card));  // tap: play, hold: its page
        this->gridBox->addView(card);
    }
}

// L/R: the section above or below the lit one. From the videos the focus moves onto that
// section in the list (the cards it was on are about to go); on the sidebar it stays there.
void LibraryTab::stepSection(int delta) {
    const long index = static_cast<long>(this->sectionIndex()) + delta;
    if (!this->allowInitialInput() || index < 0 || index >= static_cast<long>(this->sections_.size())) {
        return;
    }
    if (newpipe::focus_inside(this) && static_cast<size_t>(index) < this->sectionItems_.size()) {
        brls::Application::giveFocus(this->sectionItems_[index]);
    }
    this->selectSection(static_cast<size_t>(index));
}

void LibraryTab::confirmClear() {
    if (this->sectionKey_ != "history" && this->sectionKey_ != "favorites") {
        brls::Application::notify(newpipe::tr("library/cannot_clear_account"));
        return;
    }
    auto* dialog = new brls::Dialog(newpipe::tr(this->sectionKey_ == "favorites" ? "library/clear_favorites_confirm"
                                                                                 : "library/clear_history_confirm"));
    // borealis closes a dialog (and gives the focus back) before it runs a button's callback;
    // closing it again there popped the page under it, which asked to quit the app.
    dialog->addButton(newpipe::tr("hints/cancel"), []() {});
    dialog->addButton(newpipe::tr("library/clear_action"), [this]() {
        this->clearCurrentSection();
    });
    dialog->setCancelable(true);
    dialog->open();
}

void LibraryTab::clearCurrentSection() {
    if (this->sectionKey_ != "history" && this->sectionKey_ != "favorites") {
        brls::Application::notify(newpipe::tr("library/cannot_clear_account"));
        return;
    }
    const bool favorites = this->sectionKey_ == "favorites";
    std::string error;
    const bool ok = favorites ? newpipe::LibraryStore::instance().clear_favorites(&error)
                              : newpipe::LibraryStore::instance().clear_history(&error);
    if (!ok) {
        brls::Application::notify(error.empty() ? newpipe::tr("library/clear_failed") : error);
        return;
    }

    brls::Application::notify(favorites ? newpipe::tr("library/favorites_cleared")
                                        : newpipe::tr("library/history_cleared"));
    this->refresh();
}

void LibraryTab::playStream(const newpipe::StreamItem& item) {
    if (!allowInitialInput()) {
        return;
    }
    const auto request = newpipe::build_playback_request(item, std::nullopt);
    if (!request.has_value()) {
        this->openStream(item);
        return;
    }

    std::string ignored_error;
    newpipe::LibraryStore::instance().add_history(item, &ignored_error);
    library_return() = {true, this->sectionKey_, item.id};
    stream_grid_state::return_tab() = kLibraryTabIndex;
    newpipe::queue_playback(*request);
    brls::Application::quit();
}

void LibraryTab::openStream(const newpipe::StreamItem& item) {
    if (!allowInitialInput()) {
        return;
    }
    brls::Application::pushActivity(new StreamDetailActivity(item));
}
