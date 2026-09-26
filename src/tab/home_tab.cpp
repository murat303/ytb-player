#include "tab/home_tab.hpp"

#include "activity/stream_detail_activity.hpp"
#include "newpipe/i18n.hpp"
#include "newpipe/library_store.hpp"
#include "newpipe/log.hpp"
#include "newpipe/playback_helper.hpp"
#include "newpipe/runtime.hpp"
#include "newpipe/settings_store.hpp"
#include "view/card_gesture.hpp"
#include "view/stream_card.hpp"
#include "view/tab_focus.hpp"

namespace {
constexpr int kHomeTabIndex = 0;
constexpr const char* kSavedGridName = "home";
}

HomeTab::HomeTab()
    : service_()
    , grid_(
          this,
          [this](StreamCard* card, size_t index) { setupCard(card, index); },
          [this]() { updateStatus(); }) {
    this->inflateFromXMLRes("xml/tabs/home.xml");
    grid_.attach(gridBox);
    newpipe::log_line("home: construct");

    kiosks_ = service_.list_kiosks();
    newpipe::logf("home: kiosks=%zu", kiosks_.size());
    const newpipe::AppSettings settings = newpipe::SettingsStore::instance().settings();
    for (size_t i = 0; i < kiosks_.size(); i++) {
        if (kiosks_[i].id == settings.home_kiosk) {
            kioskIndex_ = i;
            break;
        }
    }
    // Back from a video: reopen the category the list came from.
    const SavedStreamGrid& saved = stream_grid_state::saved(kSavedGridName);
    if (saved.valid) {
        for (size_t i = 0; i < kiosks_.size(); i++) {
            if (kiosks_[i].id == saved.key) {
                kioskIndex_ = i;
                break;
            }
        }
    }
    buildChips();

    ASYNC_RETAIN
    brls::delay(700, [ASYNC_TOKEN]() {
        ASYNC_RELEASE
        interactionReady_.store(true);
        newpipe::log_line("home: interaction ready");
    });
    scheduleLoadHome(250);
}

brls::View* HomeTab::getDefaultFocus() {
    if (auto* card = grid_.focusedCard()) {
        return card;
    }
    return AttachedView::getDefaultFocus();
}

void HomeTab::onCreate() {
    // Tab actions are mirrored on the sidebar item so they stay usable while the
    // sidebar holds focus, and when the feed is empty and nothing here is focusable.
    this->registerTabAction(newpipe::tr("common/refresh"), brls::ControllerButton::BUTTON_X, [this](brls::View*) {
        service_.clear_feed_caches();
        loadHome();
        return true;
    });
    newpipe::register_tab_step(this, [this](int delta) { stepKiosk(delta); });
}

// The categories as YouTube's chips above the videos: a tap (or A) opens one.
void HomeTab::buildChips() {
    if (!chipsBox) {
        return;
    }
    for (size_t i = 0; i < kiosks_.size(); i++) {
        auto* chip = new Chip(kioskTitle(kiosks_[i]), [this, i]() { selectKiosk(i); }, i == kioskIndex_);
        chipsBox->addView(chip);
        chips_.push_back(chip);
    }
}

void HomeTab::selectKiosk(size_t index) {
    if (!allowInitialInput() || index >= kiosks_.size() || index == kioskIndex_) {
        return;
    }
    kioskIndex_ = index;
    for (size_t i = 0; i < chips_.size(); i++) {
        chips_[i]->setLight(i == kioskIndex_);
    }
    newpipe::logf("home: category %s", kiosks_[kioskIndex_].id.c_str());
    loadHome();
}

// The feed comes from a worker: the request (several hundred KB) used to hold the whole UI.
void HomeTab::loadHome() {
    newpipe::logf("home: loadHome index=%zu", kioskIndex_);
    if (!initialLoadCompleted_) {
        initialLoadAttempts_++;
    }
    if (!service_.is_loaded()) {
        showStatus(newpipe::tr("common/service_init_failed", service_.error_message()));
        return;
    }
    if (kiosks_.empty()) {
        showStatus(newpipe::tr("home/no_kiosk"));
        return;
    }

    kioskIndex_ %= kiosks_.size();
    grid_.setVertical(kiosks_[kioskIndex_].id == "shorts");
    newpipe::set_grid_scrolling(scrollFrame, kiosks_[kioskIndex_].id == "shorts");
    if (grid_.restoreFrom(stream_grid_state::saved(kSavedGridName), kiosks_[kioskIndex_].id)) {
        initialLoadCompleted_ = true;
        updateStatus();
        return;
    }

    const newpipe::Kiosk kiosk = kiosks_[kioskIndex_];
    const unsigned generation = ++loadGeneration_;
    newpipe::release_grid_focus(this, gridBox);
    grid_.clear();
    showStatus({});
    if (spinner) {
        spinner->setVisibility(brls::Visibility::VISIBLE);
    }
    ASYNC_RETAIN
    brls::async([ASYNC_TOKEN, kiosk, generation]() {
        // Its own service instance: the UI thread never touches this one.
        newpipe::YouTubeCatalogService loader;
        auto feed = loader.get_home_feed(kiosk.id);
        const std::string error = loader.error_message();
        brls::sync([ASYNC_TOKEN, kiosk, generation, feed, error]() {
            ASYNC_RELEASE
            if (generation != loadGeneration_) {
                return;  // another category was picked meanwhile
            }
            if (spinner) {
                spinner->setVisibility(brls::Visibility::GONE);
            }
            if (!feed.has_value()) {
                showStatus(error.empty() ? newpipe::tr("home/load_failed") : error);
                if (!initialLoadCompleted_ && initialLoadAttempts_ < 4) {
                    newpipe::logf("home: auto retry attempt=%d", initialLoadAttempts_);
                    scheduleLoadHome(350);
                }
                return;
            }
            initialLoadCompleted_ = true;
            newpipe::logf("home: feed=%s items=%zu", feed->kiosk.id.c_str(), feed->items.size());
            grid_.reset(feed->items);
            grid_.setNextPage(feed->next_page_token, feed->next_page_uses_search, feed->next_page_allows_shorts);
            updateStatus();
        });
    });
}

void HomeTab::scheduleLoadHome(long delay_ms) {
    ASYNC_RETAIN
    brls::delay(delay_ms, [ASYNC_TOKEN]() {
        ASYNC_RELEASE
        loadHome();
    });
}

std::string HomeTab::kioskTitle(const newpipe::Kiosk& kiosk) const {
    const std::string option_key = "settings/home_kiosk/options/" + kiosk.id;
    const std::string translated = newpipe::tr(option_key);
    return (!translated.empty() && translated != option_key) ? translated : kiosk.title;
}

// The line above the grid only speaks when there is something to say (loading failed, an
// empty list); with videos showing, the chips say which list it is, as on YouTube.
void HomeTab::showStatus(const std::string& text) {
    if (statusLabel) {
        statusLabel->setText(text);
        statusLabel->setVisibility(text.empty() ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
    }
}

void HomeTab::updateStatus() {
    showStatus(grid_.items().empty() ? newpipe::tr("home/load_failed") : std::string());
}

void HomeTab::setupCard(StreamCard* card, size_t index) {
    register_play_action(card, [this, index](brls::View*) {
        playStream(grid_.items()[index]);
        return true;
    });
    card->registerAction(newpipe::tr("common/info"), brls::ControllerButton::BUTTON_Y, [this, index](brls::View*) {
        openStream(grid_.items()[index]);
        return true;
    });
    card->addGestureRecognizer(new CardGesture(card));  // tap: play, hold: its page
}

// L/R: the chip before or after the lit one. From the videos the focus moves onto the new
// chip (the cards it was on are about to go); on the sidebar it stays there.
void HomeTab::stepKiosk(int delta) {
    const long index = static_cast<long>(kioskIndex_) + delta;
    if (!allowInitialInput() || index < 0 || index >= static_cast<long>(kiosks_.size())) {
        return;
    }
    if (newpipe::focus_inside(this) && static_cast<size_t>(index) < chips_.size()) {
        newpipe::focus_tab_chip(chips_[index], scrollFrame);
    }
    selectKiosk(static_cast<size_t>(index));
}

bool HomeTab::allowInitialInput() const {
    if (interactionReady_.load()) {
        return true;
    }

    newpipe::log_line("home: ignored startup input");
    return false;
}

void HomeTab::playStream(const newpipe::StreamItem& item) {
    if (!allowInitialInput()) {
        return;
    }
    newpipe::logf("home: playStream url=%s", item.url.c_str());
    // No detail request first: it ran on the UI thread and held the press for a network round trip.
    const auto request = newpipe::build_playback_request(item, std::nullopt);
    if (!request.has_value()) {
        openStream(item);
        return;
    }

    std::string ignored_error;
    newpipe::LibraryStore::instance().add_history(item, &ignored_error);
    grid_.saveTo(stream_grid_state::saved(kSavedGridName), kiosks_[kioskIndex_ % kiosks_.size()].id);
    stream_grid_state::return_tab() = kHomeTabIndex;
    newpipe::logf("home: queue playback url=%s", request->url.c_str());
    newpipe::queue_playback(*request);
    brls::Application::quit();
}

void HomeTab::openStream(const newpipe::StreamItem& item) {
    if (!allowInitialInput()) {
        return;
    }
    newpipe::logf("home: openStream url=%s", item.url.c_str());
    if (!kiosks_.empty()) {
        grid_.saveTo(stream_grid_state::saved(kSavedGridName), kiosks_[kioskIndex_ % kiosks_.size()].id);
        stream_grid_state::return_tab() = kHomeTabIndex;
    }
    brls::Application::pushActivity(new StreamDetailActivity(item));
}
