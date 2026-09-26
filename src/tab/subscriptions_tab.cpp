#include "tab/subscriptions_tab.hpp"

#include <cstdlib>
#include <unordered_set>

#include "activity/stream_detail_activity.hpp"
#include "activity/stream_feed_activity.hpp"
#include "activity/wifi_login_activity.hpp"
#include "newpipe/auth_store.hpp"
#include "newpipe/i18n.hpp"
#include "newpipe/image_loader.hpp"
#include "newpipe/library_store.hpp"
#include "newpipe/log.hpp"
#include "newpipe/playback_helper.hpp"
#include "newpipe/runtime.hpp"
#include "view/card_gesture.hpp"
#include "view/stream_card.hpp"
#include "view/tab_focus.hpp"

namespace {
constexpr int kSubscriptionsTabIndex = 2;
constexpr const char* kSavedGridName = "subscriptions";
constexpr size_t kChannelLimit = 30;

// "12:34" or "1:02:03" in seconds; 0 when it is no duration.
int duration_seconds(const std::string& text) {
    int total = 0;
    int part = 0;
    bool digits = false;
    for (const char ch : text) {
        if (ch >= '0' && ch <= '9') {
            part = part * 10 + (ch - '0');
            digits = true;
        } else if (ch == ':') {
            total = total * 60 + part;
            part = 0;
        } else {
            return 0;
        }
    }
    return digits ? total * 60 + part : 0;
}

// A channel's name under its picture: the label does not cut a single line to its width, so
// long names ran into the next picture.
std::string short_name(const std::string& name) {
    constexpr size_t kLimit = 11;
    size_t characters = 0;
    for (size_t i = 0; i < name.size(); i++) {
        if ((static_cast<unsigned char>(name[i]) & 0xC0) != 0x80 && characters++ == kLimit) {
            return name.substr(0, i) + "…";
        }
    }
    return name;
}

bool is_short(const newpipe::StreamItem& item) {
    const int seconds = duration_seconds(item.duration_text);
    return item.url.find("/shorts/") != std::string::npos || (seconds > 0 && seconds <= 60);
}

// Uploaded within the day: YouTube writes "3 saat önce" / "25 dakika önce", "3 hours ago",
// "3시간 전".
bool is_today(const newpipe::StreamItem& item) {
    for (const char* unit : {"saniye", "dakika", "saat", "second", "minute", "hour", "초", "분", "시간"}) {
        if (item.published_text.find(unit) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// The filter and the whole feed when a video was started from the tab: the saved grid holds
// only what the filter showed.
struct SubscriptionsReturn {
    int filter = 0;
    std::vector<newpipe::StreamItem> all_items;
};

SubscriptionsReturn& subscriptions_return() {
    static SubscriptionsReturn state;
    return state;
}

#ifndef __SWITCH__
// Desktop screenshot tests have no YouTube session: NEWPIPE_FAKE_SUBSCRIPTIONS names a search
// whose results stand in for the subscription feed.
const char* fake_subscriptions() {
    return std::getenv("NEWPIPE_FAKE_SUBSCRIPTIONS");
}
#else
const char* fake_subscriptions() {
    return nullptr;
}
#endif
}  // namespace

SubscriptionsTab::SubscriptionsTab()
    : service_()
    , grid_(
          this,
          [this](StreamCard* card, size_t index) { this->setupCard(card, index); },
          [this]() { this->updateCount(); }) {
    this->inflateFromXMLRes("xml/tabs/subscriptions.xml");
    this->grid_.attach(this->gridBox);
    newpipe::log_line("subscriptions: construct");
    this->buildChips();
    ASYNC_RETAIN
    brls::delay(700, [ASYNC_TOKEN]() {
        ASYNC_RELEASE
        interactionReady_.store(true);
    });
}

void SubscriptionsTab::onShow() {
    AttachedView::onShow();
    // Loaded on first show: the UI is rebuilt after every video, and a constructor-time load
    // downloaded the ~2 MB feed each time, even with the tab never opened.
    if (!this->loadRequested_) {
        this->loadRequested_ = true;
        ASYNC_RETAIN
        brls::delay(50, [ASYNC_TOKEN]() {
            ASYNC_RELEASE
            this->refresh();
        });
    }
}

brls::View* SubscriptionsTab::getDefaultFocus() {
    if (auto* card = grid_.focusedCard()) {
        return card;
    }
    return AttachedView::getDefaultFocus();
}

void SubscriptionsTab::onCreate() {
    // Mirrored on the sidebar item: while signed out this tab has no focusable
    // child, so a content-only action could never be triggered.
    this->registerTabAction(newpipe::tr("common/refresh"), brls::ControllerButton::BUTTON_X, [this](brls::View*) {
        this->service_.clear_feed_caches();
        this->refresh();
        return true;
    });
    this->registerTabAction(newpipe::tr("subscriptions/session_action"), brls::ControllerButton::BUTTON_BACK, [this](brls::View*) {
        this->openSessionDialog();
        return true;
    });
    newpipe::register_tab_step(this, [this](int delta) { this->stepFilter(delta); });
}

bool SubscriptionsTab::allowInitialInput() const {
    return interactionReady_.load();
}

// The feed (about 2 MB) comes from a worker; the UI used to stand still while it loaded.
void SubscriptionsTab::refresh() {
    newpipe::log_line("subscriptions: refresh");
    const bool fake = fake_subscriptions() != nullptr;
    std::string auth_error;
    if (!fake && !this->service_.load_auth_session(&auth_error)) {
        if (this->statusLabel) {
            this->statusLabel->setVisibility(brls::Visibility::VISIBLE);
            this->statusLabel->setText(
                auth_error.empty() ? newpipe::tr("subscriptions/session_load_failed") : auth_error);
        }
        if (this->bodyLabel) {
            this->bodyLabel->setVisibility(brls::Visibility::VISIBLE);
            this->bodyLabel->setText(newpipe::tr("subscriptions/session_load_failed_body"));
        }
        this->clearGrid();
        return;
    }
    if (!fake && !this->service_.has_auth_session()) {
        this->showSignedOutState();
        return;
    }

    SavedStreamGrid& saved = stream_grid_state::saved(kSavedGridName);
    const std::string saved_title = saved.title;
    const SubscriptionsReturn back = std::move(subscriptions_return());
    subscriptions_return() = {};
    this->grid_.setVertical(back.filter == static_cast<int>(Filter::shorts));
    newpipe::set_grid_scrolling(this->scrollFrame, back.filter == static_cast<int>(Filter::shorts));
    if (this->grid_.restoreFrom(saved, kSavedGridName)) {
        // Back from a video: the same list, filter and card; the channels come from it again.
        this->statusTitle_ = saved_title;
        this->allItems_ = back.all_items.empty() ? this->grid_.items() : back.all_items;
        this->filter_ = static_cast<Filter>(back.filter);
        for (size_t i = 0; i < this->chips_.size(); i++) {
            this->chips_[i]->setLight(static_cast<int>(i) == back.filter);
        }
        if (this->chipsBox) {
            this->chipsBox->setVisibility(this->allItems_.empty() ? brls::Visibility::GONE
                                                                  : brls::Visibility::VISIBLE);
        }
        this->buildChannels();
        this->updateCount();
        this->showSessionBody();
        return;
    }

    if (this->spinner) {
        this->spinner->setVisibility(brls::Visibility::VISIBLE);
    }
    if (this->statusLabel) {
        this->statusLabel->setVisibility(brls::Visibility::GONE);
    }
    if (this->bodyLabel) {
        this->bodyLabel->setVisibility(brls::Visibility::GONE);
    }
    const unsigned generation = ++this->loadGeneration_;
    const std::string fake_query = fake ? fake_subscriptions() : "";
    ASYNC_RETAIN
    brls::async([ASYNC_TOKEN, generation, fake_query]() {
        // Its own service instance: the UI thread never touches this one.
        newpipe::YouTubeCatalogService loader;
        std::optional<newpipe::HomeFeed> feed;
        if (!fake_query.empty()) {
            newpipe::HomeFeed page;
            page.kiosk = {"subscriptions", newpipe::tr("app/subscriptions")};
            page.items = loader.search(fake_query).items;
            feed = page;
        } else {
            feed = loader.get_subscriptions_feed();
        }
        const std::string error = loader.error_message();
        const auto session = loader.auth_session();
        brls::sync([ASYNC_TOKEN, generation, feed, error, session]() {
            ASYNC_RELEASE
            if (generation != this->loadGeneration_) {
                return;
            }
            if (this->spinner) {
                this->spinner->setVisibility(brls::Visibility::GONE);
            }
            if (!feed.has_value()) {
                if (this->statusLabel) {
                    this->statusLabel->setVisibility(brls::Visibility::VISIBLE);
                    this->statusLabel->setText(error.empty() ? newpipe::tr("subscriptions/feed_load_failed") : error);
                }
                if (this->bodyLabel) {
                    std::string body = newpipe::tr("subscriptions/feed_load_failed_body");
                    if (!session.source_path.empty()) {
                        body += "\n\n" + newpipe::tr("subscriptions/current_source", session.source_path);
                    }
                    this->bodyLabel->setVisibility(brls::Visibility::VISIBLE);
                    this->bodyLabel->setText(body);
                }
                this->clearGrid();
                return;
            }
            this->statusTitle_ = feed->kiosk.title;
            this->nextPageToken_ = feed->next_page_token;
            this->nextPageUsesSearch_ = feed->next_page_uses_search;
            this->nextPageAllowsShorts_ = feed->next_page_allows_shorts;
            this->showFeed(feed->items);
        });
    });
}

void SubscriptionsTab::showFeed(const std::vector<newpipe::StreamItem>& items) {
    this->allItems_ = items;
    this->buildChannels();
    this->applyFilter(Filter::all);
    this->showSessionBody();
}

// The channels with new videos, newest first, as round pictures along the top (YouTube's row);
// one opens that channel's page.
void SubscriptionsTab::buildChannels() {
    if (!this->channelsBox || !this->channelsScroll) {
        return;
    }
    for (brls::View* view = brls::Application::getCurrentFocus(); view; view = view->getParent()) {
        if (view == this->channelsBox) {
            brls::Application::giveFocus(this->getTabBar());  // its button is about to go
            break;
        }
    }
    this->channelsBox->clearViews();
    std::unordered_set<std::string> seen;
    for (const auto& item : this->allItems_) {
        const std::string key = item.channel_id.empty() ? item.channel_name : item.channel_id;
        if (key.empty() || !seen.insert(key).second) {
            continue;
        }
        auto* entry = new brls::Box(brls::Axis::COLUMN);
        entry->setFocusable(true);
        entry->setAlignItems(brls::AlignItems::CENTER);
        entry->setWidth(96);
        entry->setPadding(6, 4, 6, 4);
        entry->setMarginRight(4);
        entry->setCornerRadius(12);
        entry->setHighlightCornerRadius(12);

        auto* picture = new brls::Image();
        picture->setDimensions(64, 64);
        picture->setCornerRadius(32);
        picture->setScalingType(brls::ImageScalingType::FILL);
        picture->setBackgroundColor(nvgRGB(0x27, 0x27, 0x27));
        if (!item.channel_avatar_url.empty()) {
            newpipe::ImageLoader::instance().load(item.channel_avatar_url, picture);
        }
        entry->addView(picture);

        auto* name = new brls::Label();
        name->setFontSize(13);
        name->setSingleLine(true);
        name->setWidth(88);
        name->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        name->setTextColor(nvgRGB(0xF1, 0xF1, 0xF1));
        name->setMarginTop(6);
        name->setText(short_name(item.channel_name));
        entry->addView(name);

        const newpipe::StreamItem channel = item;
        entry->registerClickAction([this, channel](brls::View*) {
            this->openChannel(channel);
            return true;
        });
        entry->addGestureRecognizer(new brls::TapGestureRecognizer(entry));
        this->channelsBox->addView(entry);
        if (seen.size() >= kChannelLimit) {
            break;
        }
    }
    this->channelsScroll->setVisibility(seen.empty() ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
}

void SubscriptionsTab::buildChips() {
    if (!this->chipsBox) {
        return;
    }
    const std::pair<Filter, const char*> filters[] = {
        {Filter::all, "subscriptions/filter/all"},
        {Filter::today, "subscriptions/filter/today"},
        {Filter::videos, "subscriptions/filter/videos"},
        {Filter::live, "subscriptions/filter/live"},
        {Filter::shorts, "subscriptions/filter/shorts"},
    };
    for (const auto& [filter, key] : filters) {
        const Filter chosen = filter;
        auto* chip = new Chip(newpipe::tr(key), [this, chosen]() { this->applyFilter(chosen); }, filter == Filter::all);
        this->chipsBox->addView(chip);
        this->chips_.push_back(chip);
    }
}

// L/R: the filter chip before or after the lit one, while the chips show. From the videos
// the focus moves onto the new chip (the cards it was on are about to go).
void SubscriptionsTab::stepFilter(int delta) {
    const int index = static_cast<int>(this->filter_) + delta;
    if (!this->allowInitialInput() || this->allItems_.empty() || index < 0
        || index >= static_cast<int>(this->chips_.size())) {
        return;
    }
    if (newpipe::focus_inside(this)) {
        newpipe::focus_tab_chip(this->chips_[index], this->scrollFrame);
    }
    this->applyFilter(static_cast<Filter>(index));
}

// The chips narrow the feed in place; only the whole feed keeps paging in more.
void SubscriptionsTab::applyFilter(Filter filter) {
    if (this->filter_ == Filter::all && filter != Filter::all && !this->grid_.items().empty()) {
        this->allItems_ = this->grid_.items();  // it may have paged in more since
    }
    this->filter_ = filter;
    for (size_t i = 0; i < this->chips_.size(); i++) {
        this->chips_[i]->setLight(static_cast<int>(i) == static_cast<int>(filter));
    }
    std::vector<newpipe::StreamItem> shown;
    for (const auto& item : this->allItems_) {
        const bool keep = filter == Filter::all
            || (filter == Filter::today && is_today(item))
            || (filter == Filter::videos && !item.is_live && !is_short(item))
            || (filter == Filter::live && item.is_live)
            || (filter == Filter::shorts && is_short(item));
        if (keep) {
            shown.push_back(item);
        }
    }
    newpipe::release_grid_focus(this, this->gridBox);
    // Shorts stand upright, six to a row, as on Home.
    this->grid_.setVertical(filter == Filter::shorts);
    newpipe::set_grid_scrolling(this->scrollFrame, filter == Filter::shorts);
    this->grid_.reset(shown);
    this->grid_.setNextPage(filter == Filter::all ? this->nextPageToken_ : std::string(), this->nextPageUsesSearch_,
                            this->nextPageAllowsShorts_);
    if (this->chipsBox) {
        this->chipsBox->setVisibility(this->allItems_.empty() ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
    }
    this->updateCount();
}

// With videos showing, the header names the page, as on YouTube: the count line goes away and
// comes back only for an empty list.
void SubscriptionsTab::updateCount() {
    if (this->statusLabel) {
        const bool empty = this->grid_.items().empty();
        this->statusLabel->setText(empty && !this->allItems_.empty() ? newpipe::tr("subscriptions/filter/empty")
                                                                     : newpipe::tr("common/count_with_title",
                                                                                   this->statusTitle_,
                                                                                   this->grid_.items().size()));
        this->statusLabel->setVisibility(empty ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    }
}

void SubscriptionsTab::showSessionBody() {
    if (this->bodyLabel) {
        const auto session = this->service_.auth_session();
        std::string session_name;
        if (!session.display_name.empty()) {
            session_name = session.display_name;
        } else if (!session.source_path.empty()) {
            session_name = session.source_path;
        } else {
            session_name = newpipe::tr("settings/session/saved");
        }
        std::string body = newpipe::tr("subscriptions/session_prefix", session_name);
        body += "\n" + newpipe::tr("subscriptions/controls");
        this->bodyLabel->setText(body);
        // The session line is for the empty state; the bottom bar shows the buttons.
        this->bodyLabel->setVisibility(this->allItems_.empty() ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    }
}

void SubscriptionsTab::setupCard(StreamCard* card, size_t index) {
    register_play_action(card, [this, index](brls::View*) {
        this->playStream(this->grid_.items()[index]);
        return true;
    });
    card->registerAction(newpipe::tr("common/info"), brls::ControllerButton::BUTTON_Y, [this, index](brls::View*) {
        this->openStream(this->grid_.items()[index]);
        return true;
    });
    card->addGestureRecognizer(new CardGesture(card));  // tap: play, hold: its page
}

void SubscriptionsTab::clearGrid() {
    this->allItems_.clear();
    this->buildChannels();
    if (this->chipsBox) {
        this->chipsBox->setVisibility(brls::Visibility::GONE);
    }
    newpipe::release_grid_focus(this, this->gridBox);
    this->grid_.clear();
}

void SubscriptionsTab::showSignedOutState() {
    this->clearGrid();
    if (this->statusLabel) {
        this->statusLabel->setVisibility(brls::Visibility::VISIBLE);
        this->statusLabel->setText(newpipe::tr("subscriptions/signed_out_title"));
    }
    if (this->bodyLabel) {
        this->bodyLabel->setVisibility(brls::Visibility::VISIBLE);
        const std::string body = newpipe::tr(
            "subscriptions/signed_out_body", newpipe::default_auth_import_path());
        this->bodyLabel->setText(body);
    }
}

void SubscriptionsTab::openChannel(const newpipe::StreamItem& item) {
    if (!allowInitialInput()) {
        return;
    }
    brls::Application::pushActivity(new StreamFeedActivity(
        item.channel_name.empty() ? newpipe::tr("detail/channel_action") : item.channel_name,
        [item](newpipe::YouTubeCatalogService& service) { return service.get_channel_feed(item); },
        newpipe::tr("detail/channel_load_failed")));
}

void SubscriptionsTab::openSessionDialog() {
    const auto session = this->service_.auth_session();

    std::string body;
    if (this->service_.has_auth_session()) {
        if (!session.source_path.empty()) {
            body = newpipe::tr("subscriptions/session_dialog/saved", session.source_path);
        } else if (!session.source_label.empty()) {
            body = newpipe::tr("subscriptions/session_dialog/saved", session.source_label);
        } else {
            body = newpipe::tr("subscriptions/session_dialog/saved", "manual");
        }
    } else {
        body = newpipe::tr("subscriptions/session_dialog/signed_out", newpipe::default_auth_import_path());
    }

    // At most three buttons (borealis): Wi-Fi, then the file or signing out, then close.
    auto* dialog = new brls::Dialog(body);
    dialog->addButton(newpipe::tr("subscriptions/session_dialog/wifi"), [this]() {
        brls::Application::pushActivity(new WifiLoginActivity([this]() {
            this->service_.clear_feed_caches();
            this->refresh();
        }));
    });
    if (this->service_.has_auth_session()) {
        dialog->addButton(newpipe::tr("subscriptions/session_dialog/logout"), [this]() {
            std::string error;
            if (this->service_.clear_auth_session(&error)) {
                brls::Application::notify(newpipe::tr("subscriptions/session_dialog/logout_done"));
            } else {
                brls::Application::notify(
                    error.empty() ? newpipe::tr("subscriptions/session_dialog/logout_failed") : error);
            }
            this->refresh();
        });
    } else {
        dialog->addButton(newpipe::tr("subscriptions/session_dialog/load_file"), [this]() {
            std::string error;
            if (this->service_.import_auth_session_from_file({}, &error)) {
                brls::Application::notify(newpipe::tr("subscriptions/session_dialog/load_file_done"));
            } else {
                brls::Application::notify(
                    error.empty() ? newpipe::tr("subscriptions/session_dialog/load_file_failed") : error);
            }
            this->refresh();
        });
    }
    dialog->addButton(newpipe::tr("common/close"), []() {});
    dialog->setCancelable(true);
    dialog->open();
}

void SubscriptionsTab::playStream(const newpipe::StreamItem& item) {
    if (!allowInitialInput()) {
        return;
    }
    newpipe::logf("subscriptions: playStream url=%s", item.url.c_str());
    // No detail request first: it ran on the UI thread and held the press for a network round trip.
    const auto request = newpipe::build_playback_request(item, std::nullopt);
    if (!request.has_value()) {
        this->openStream(item);
        return;
    }

    std::string ignored_error;
    newpipe::LibraryStore::instance().add_history(item, &ignored_error);
    this->saveForReturn();
    newpipe::queue_playback(*request);
    brls::Application::quit();
}

void SubscriptionsTab::openStream(const newpipe::StreamItem& item) {
    if (!allowInitialInput()) {
        return;
    }
    this->saveForReturn();
    brls::Application::pushActivity(new StreamDetailActivity(item));
}

// The grid, the filter and the whole feed, for the UI rebuilt after a video.
void SubscriptionsTab::saveForReturn() {
    this->grid_.saveTo(stream_grid_state::saved(kSavedGridName), kSavedGridName, this->statusTitle_);
    stream_grid_state::return_tab() = kSubscriptionsTabIndex;
    subscriptions_return() = {static_cast<int>(this->filter_),
                              this->filter_ == Filter::all ? this->grid_.items() : this->allItems_};
}
