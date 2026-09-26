#include "tab/search_tab.hpp"

#include "activity/stream_detail_activity.hpp"
#include "newpipe/i18n.hpp"
#include "newpipe/library_store.hpp"
#include "newpipe/log.hpp"
#include "newpipe/playback_helper.hpp"
#include "newpipe/runtime.hpp"
#include "newpipe/settings_store.hpp"
#include "view/chip.hpp"
#include "view/card_gesture.hpp"
#include "view/stream_card.hpp"
#include "view/svg_image.hpp"
#include "view/tab_focus.hpp"

namespace {
constexpr int kSearchTabIndex = 1;
constexpr const char* kSavedGridName = "search";
// Recent searches shown as chips under the box; longer queries are cut.
constexpr size_t kHistoryChips = 6;
constexpr size_t kHistoryChipCharacters = 24;

std::string clamp_query(const std::string& query) {
    size_t characters = 0;
    for (size_t i = 0; i < query.size(); i++) {
        if ((static_cast<unsigned char>(query[i]) & 0xC0) != 0x80 && characters++ == kHistoryChipCharacters) {
            return query.substr(0, i) + "…";
        }
    }
    return query;
}
}  // namespace

SearchTab::SearchTab()
    : service_()
    , grid_(
          this,
          [this](StreamCard* card, size_t index) { setupCard(card, index); },
          [this]() { updateCount(); }) {
    this->inflateFromXMLRes("xml/tabs/search.xml");
    grid_.attach(gridBox);
    newpipe::log_line("search: construct");

    // The search box opens the keyboard on A or a tap, as YouTube's does.
    if (searchIcon) {
        searchIcon->setImageFromSVGRes("svg/search.svg");
    }
    if (searchBar) {
        searchBar->registerClickAction([this](brls::View*) {
            openKeyboard();
            return true;
        });
        searchBar->addGestureRecognizer(new brls::TapGestureRecognizer(searchBar));
    }

    // Back from a video started here: show the same results again.
    SavedStreamGrid& saved = stream_grid_state::saved(kSavedGridName);
    if (saved.valid) {
        const std::string query = saved.key;
        if (grid_.restoreFrom(saved, query)) {
            lastQuery_ = query;
        }
    }
    showQuery();
    buildHistory();
    ASYNC_RETAIN
    brls::delay(700, [ASYNC_TOKEN]() {
        ASYNC_RELEASE
        interactionReady_.store(true);
        newpipe::log_line("search: interaction ready");
    });
}

brls::View* SearchTab::getDefaultFocus() {
    if (auto* card = grid_.focusedCard()) {
        return card;
    }
    if (searchBar) {
        return searchBar;
    }
    return AttachedView::getDefaultFocus();
}

void SearchTab::onCreate() {
    // Registered on the sidebar item as well, so X opens the keyboard from the rail too.
    this->registerTabAction(newpipe::tr("search/action"), brls::ControllerButton::BUTTON_X, [this](brls::View*) {
        openKeyboard();
        return true;
    });
}

void SearchTab::openKeyboard() {
    brls::Application::getImeManager()->openForText(
        [this](const std::string& text) { doSearch(text); },
        newpipe::tr("search/ime_title"),
        newpipe::tr("search/ime_subtitle"),
        80,
        lastQuery_);
}

// The box shows the query searched for, or the prompt in gray before the first search.
void SearchTab::showQuery() {
    if (!searchText) {
        return;
    }
    searchText->setText(lastQuery_.empty() ? newpipe::tr("search/placeholder") : lastQuery_);
    searchText->setTextColor(lastQuery_.empty() ? nvgRGB(0xAA, 0xAA, 0xAA) : nvgRGB(0xF1, 0xF1, 0xF1));
}

// Recent searches as chips under the box; one repeats that search.
void SearchTab::buildHistory() {
    if (!historyBox) {
        return;
    }
    // Deleting the focused chip would leave borealis without a focus, and then nothing moves.
    for (brls::View* view = brls::Application::getCurrentFocus(); view; view = view->getParent()) {
        if (view == historyBox) {
            if (searchBar) {
                brls::Application::giveFocus(searchBar);
            }
            break;
        }
    }
    historyBox->clearViews();
    const auto history = newpipe::LibraryStore::instance().search_history();
    for (size_t i = 0; i < history.size() && i < kHistoryChips; i++) {
        const std::string query = history[i];
        historyBox->addView(new Chip(clamp_query(query), [this, query]() { doSearch(query); }));
    }
    historyBox->setVisibility(history.empty() ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
}

void SearchTab::showStatus(const std::string& text) {
    if (statusLabel) {
        statusLabel->setText(text);
        statusLabel->setVisibility(text.empty() ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
    }
}

// The search runs on a worker: the UI used to stand still for the whole request.
void SearchTab::doSearch(const std::string& query) {
    if (query.empty()) {
        return;
    }
    lastQuery_ = query;
    newpipe::logf("search: doSearch query=%s", query.c_str());
    std::string ignored_error;
    newpipe::LibraryStore::instance().add_search(query, &ignored_error);
    showQuery();
    buildHistory();
    newpipe::release_grid_focus(this, gridBox);
    grid_.clear();
    showStatus({});
    if (spinner) {
        spinner->setVisibility(brls::Visibility::VISIBLE);
    }

    const unsigned generation = ++searchGeneration_;
    ASYNC_RETAIN
    brls::async([ASYNC_TOKEN, query, generation]() {
        // Its own service instance: the UI thread never touches this one.
        newpipe::YouTubeCatalogService loader;
        const auto results = loader.search(query);
        const std::string error = loader.error_message();
        brls::sync([ASYNC_TOKEN, query, generation, results, error]() {
            ASYNC_RELEASE
            if (generation != searchGeneration_) {
                return;
            }
            if (spinner) {
                spinner->setVisibility(brls::Visibility::GONE);
            }
            newpipe::logf("search: results=%zu", results.items.size());
            grid_.reset(results.items);
            grid_.setNextPage(results.next_page_token, true,
                              !newpipe::SettingsStore::instance().settings().hide_short_videos);
            if (grid_.items().empty()) {
                showStatus(error.empty() ? newpipe::tr("search/no_results", query) : error);
            } else if (searchBar && brls::Application::getCurrentFocus() == searchBar) {
                grid_.focusItem(0);  // the keyboard came from the box: go on to the results
            }
        });
    });
}

void SearchTab::setupCard(StreamCard* card, size_t index) {
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

void SearchTab::updateCount() {
    // As on YouTube the list tells no count; the status line only speaks for errors.
}

bool SearchTab::allowInitialInput() const {
    if (interactionReady_.load()) {
        return true;
    }

    newpipe::log_line("search: ignored startup input");
    return false;
}

void SearchTab::playStream(const newpipe::StreamItem& item) {
    if (!allowInitialInput()) {
        return;
    }
    newpipe::logf("search: playStream url=%s", item.url.c_str());
    // No detail request first: it ran on the UI thread and held the press for a network round trip.
    const auto request = newpipe::build_playback_request(item, std::nullopt);
    if (!request.has_value()) {
        openStream(item);
        return;
    }

    std::string ignored_error;
    newpipe::LibraryStore::instance().add_history(item, &ignored_error);
    grid_.saveTo(stream_grid_state::saved(kSavedGridName), lastQuery_);
    stream_grid_state::return_tab() = kSearchTabIndex;
    newpipe::logf("search: queue playback url=%s", request->url.c_str());
    newpipe::queue_playback(*request);
    brls::Application::quit();
}

void SearchTab::openStream(const newpipe::StreamItem& item) {
    if (!allowInitialInput()) {
        return;
    }
    newpipe::logf("search: openStream url=%s", item.url.c_str());
    grid_.saveTo(stream_grid_state::saved(kSavedGridName), lastQuery_);
    stream_grid_state::return_tab() = kSearchTabIndex;
    brls::Application::pushActivity(new StreamDetailActivity(item));
}
