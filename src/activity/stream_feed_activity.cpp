#include "activity/stream_feed_activity.hpp"

#include "activity/stream_detail_activity.hpp"
#include "newpipe/i18n.hpp"
#include "newpipe/image_loader.hpp"
#include "newpipe/library_store.hpp"
#include "newpipe/log.hpp"
#include "newpipe/playback_helper.hpp"
#include "newpipe/runtime.hpp"
#include "view/chip.hpp"
#include "view/card_gesture.hpp"
#include "view/stream_card.hpp"
#include "view/tab_focus.hpp"

namespace {
constexpr size_t kGridColumns = 3;  // as StreamGrid::kColumns
constexpr size_t kShortsColumns = 6;
// A channel can have thousands of videos; the list stops growing here (memory).
constexpr size_t kMaxItems = 300;
// The next page is requested when the focus is this many cards from the end.
constexpr size_t kPrefetchCards = 8;
// Under the channel's name: the start of its description, one line as on YouTube.
constexpr size_t kDescriptionCharacters = 150;

// The tab params YouTube gives a channel's tabs start with the tab's name in base64.
bool is_videos_tab(const std::string& params) {
    return params.rfind("EgZ2aWRlb3", 0) == 0;
}

bool is_shorts_tab(const std::string& params) {
    return params.rfind("EgZzaG9ydH", 0) == 0;
}

// The first line of a text, at most `limit` characters (not bytes), "…" when cut.
std::string first_line(const std::string& text, size_t limit) {
    const std::string line = text.substr(0, text.find('\n'));
    size_t characters = 0;
    for (size_t i = 0; i < line.size(); i++) {
        if ((static_cast<unsigned char>(line[i]) & 0xC0) == 0x80) {
            continue;
        }
        if (characters++ == limit) {
            return line.substr(0, i) + "…";
        }
    }
    return line.size() < text.size() ? line + " …" : line;
}

// Items of a channel's own page come without the channel; the history and the detail page
// want it.
void adopt_channel(std::vector<newpipe::StreamItem>& items, size_t from, const newpipe::ChannelInfo& channel) {
    for (size_t i = from; i < items.size(); i++) {
        auto& item = items[i];
        if (item.channel_name.empty()) {
            item.channel_name = channel.name;
        }
        if (item.channel_id.empty()) {
            item.channel_id = channel.id;
            item.channel_url = "https://www.youtube.com/channel/" + channel.id;
        }
        if (item.channel_avatar_url.empty()) {
            item.channel_avatar_url = channel.avatar_url;
        }
    }
}
}  // namespace

StreamFeedActivity::StreamFeedActivity(std::string title, FeedLoader loader, std::string failure_text)
    : title_(std::move(title))
    , loader_(std::move(loader))
    , failureText_(std::move(failure_text)) {
}

StreamFeedActivity::~StreamFeedActivity() {
    *alive_ = false;
}

void StreamFeedActivity::onContentAvailable() {
    // Closed within half a second (B at once), the page is gone when this runs.
    std::shared_ptr<bool> alive = this->alive_;
    brls::delay(500, [this, alive]() {
        if (*alive) {
            interactionReady_.store(true);
        }
    });
    this->registerAction(newpipe::tr("hints/back"), brls::BUTTON_B, [](brls::View*) {
        brls::Application::popActivity();
        return true;
    });

    if (this->statusLabel) {
        this->statusLabel->setText(this->title_);
    }
    this->loadFeed();
}

void StreamFeedActivity::loadFeed() {
    this->showMessage(newpipe::tr("feed/loading"));
    std::shared_ptr<bool> alive = this->alive_;
    FeedLoader loader = this->loader_;
    brls::async([this, alive, loader]() {
        // Its own service instance: the UI thread never touches this one.
        newpipe::YouTubeCatalogService service;
        auto feed = loader(service);
        const std::string error = service.error_message();
        brls::sync([this, alive, feed, error]() {
            if (!*alive) {
                return;
            }
            if (!feed.has_value()) {
                this->showMessage(error.empty() ? this->failureText_ : error);
                newpipe::logf("feed_activity: load failed error=%s", error.c_str());
                return;
            }
            this->items_ = feed->items;
            if (feed->channel.has_value()) {
                this->showChannel(*feed->channel);
            } else if (feed->playlist.has_value()) {
                this->showPlaylist(*feed->playlist);
            } else if (!feed->kiosk.title.empty() && this->statusLabel) {
                this->statusLabel->setText(feed->kiosk.title);
            }
            if (this->channel_) {
                adopt_channel(this->items_, 0, *this->channel_);
            }
            this->nextPageToken_ = feed->next_page_token;
            this->nextPageAllowsShorts_ = feed->next_page_allows_shorts;
            this->showMessage(this->items_.empty() ? newpipe::tr("feed/tab_empty") : std::string());
            this->appendCards(0);
            // Until now the focus sat on the invisible holder above the list. It stays out of
            // the way afterwards: up from the top of the page must not land on it.
            brls::View* first = nullptr;
            if (this->playlist_ && this->playlistActions && !this->playlistActions->getChildren().empty()) {
                first = this->playlistActions->getChildren().front();  // "Play all"
                // Nothing is under it in the panel: down goes to the videos too.
                if (this->gridBox && !this->gridBox->getChildren().empty()) {
                    first->setCustomNavigationRoute(brls::FocusDirection::DOWN, this->gridBox->getChildren().front());
                }
            } else if (this->gridBox && !this->gridBox->getChildren().empty()) {
                first = this->gridBox->getChildren().front();
            } else if (this->tab_ < this->tabChips_.size()) {
                first = this->tabChips_[this->tab_];
            }
            if (first) {
                brls::Application::giveFocus(first);
                if (this->holderBox) {
                    this->holderBox->setFocusable(false);
                }
            }
        });
    });
}

bool StreamFeedActivity::allowInitialInput() const {
    return interactionReady_.load();
}

// The channel's banner, picture, name, "@handle • subscribers • videos", the start of its
// description and a chip for each tab; the Videos tab is the one loaded with the page.
void StreamFeedActivity::showChannel(const newpipe::ChannelInfo& channel) {
    this->channel_ = channel;
    if (this->statusLabel) {
        this->statusLabel->setVisibility(brls::Visibility::GONE);
    }
    if (this->channelBox) {
        this->channelBox->setVisibility(brls::Visibility::VISIBLE);
    }
    if (this->bannerImage) {
        if (channel.banner_url.empty()) {
            this->bannerImage->setVisibility(brls::Visibility::GONE);
        } else {
            newpipe::ImageLoader::instance().load(channel.banner_url, this->bannerImage);
        }
    }
    if (this->avatarImage && !channel.avatar_url.empty()) {
        newpipe::ImageLoader::instance().load(channel.avatar_url, this->avatarImage);
    }
    if (this->channelNameLabel) {
        this->channelNameLabel->setText(channel.name.empty() ? this->title_ : channel.name);
    }
    if (this->channelMetaLabel) {
        std::string meta = channel.handle;
        for (const std::string* part : {&channel.subscribers, &channel.video_count}) {
            if (!part->empty()) {
                meta += (meta.empty() ? "" : " • ") + *part;
            }
        }
        this->channelMetaLabel->setText(meta);
    }
    if (this->channelDescriptionLabel) {
        this->channelDescriptionLabel->setText(first_line(channel.description, kDescriptionCharacters));
        this->channelDescriptionLabel->setVisibility(
            channel.description.empty() ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
    }

    if (!this->tabsBox) {
        return;
    }
    this->tabsBox->clearViews();
    this->tabChips_.clear();
    this->tab_ = channel.tabs.size();  // none lit unless there is a Videos tab
    for (size_t i = 0; i < channel.tabs.size(); i++) {
        if (this->tab_ == channel.tabs.size() && is_videos_tab(channel.tabs[i].params)) {
            this->tab_ = i;
        }
        auto* chip = new Chip(channel.tabs[i].title, [this, i]() { this->selectTab(i); });
        // Up to the tabs brings the whole header back into view.
        chip->getFocusEvent()->subscribe([this](brls::View*) {
            if (this->scrollFrame && brls::Application::getInputType() == brls::InputType::GAMEPAD) {
                this->scrollFrame->setContentOffsetY(0, true);
            }
        });
        this->tabsBox->addView(chip);
        this->tabChips_.push_back(chip);
    }
    this->tabsBox->setVisibility(this->tabChips_.empty() ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
    this->updateTabs();
    if (!this->tabChips_.empty() && !this->tabStepRegistered_) {
        this->tabStepRegistered_ = true;
        newpipe::register_tab_step(this->getContentView(), [this](int delta) { this->stepTab(delta); });
    }
}

// The playlist's cover, name, owner, counts and description in the left panel, with "Play
// all" (the first video, within the list); the videos become rows beside it.
void StreamFeedActivity::showPlaylist(const newpipe::PlaylistInfo& playlist) {
    this->playlist_ = playlist;
    if (this->statusLabel) {
        this->statusLabel->setVisibility(brls::Visibility::GONE);
    }
    if (this->playlistBox) {
        this->playlistBox->setVisibility(brls::Visibility::VISIBLE);
    }
    const std::string cover = !playlist.thumbnail_url.empty()
        ? playlist.thumbnail_url
        : (!this->items_.empty() ? this->items_.front().thumbnail_url : std::string());
    if (this->playlistCover && !cover.empty()) {
        newpipe::ImageLoader::instance().load(cover, this->playlistCover);
    }
    if (this->playlistTitle) {
        this->playlistTitle->setText(playlist.title.empty() ? this->title_ : playlist.title);
    }
    if (this->playlistOwner) {
        this->playlistOwner->setText(playlist.owner);
        this->playlistOwner->setVisibility(playlist.owner.empty() ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
    }
    if (this->playlistMeta) {
        this->playlistMeta->setText(playlist.meta);
    }
    if (this->playlistDescription) {
        this->playlistDescription->setText(first_line(playlist.description, kDescriptionCharacters));
        this->playlistDescription->setVisibility(
            playlist.description.empty() ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
    }
    if (this->playlistActions) {
        this->playlistActions->addView(new Chip("▶  " + newpipe::tr("feed/play_all"), [this]() {
            if (!this->items_.empty()) {
                this->playStream(this->inPlaylist(this->items_.front()));
            }
        }, true));
    }
}

newpipe::StreamItem StreamFeedActivity::inPlaylist(const newpipe::StreamItem& item) const {
    newpipe::StreamItem copy = item;
    if (this->playlist_ && !item.id.empty() && item.url.find("list=") == std::string::npos) {
        copy.url = "https://www.youtube.com/watch?v=" + item.id + "&list=" + this->playlist_->id;
    }
    return copy;
}

// The open tab's chip is the light one; up from the list comes back to it.
void StreamFeedActivity::updateTabs() {
    for (size_t i = 0; i < this->tabChips_.size(); i++) {
        this->tabChips_[i]->setLight(i == this->tab_);
    }
    if (this->tabsBox && this->tab_ < this->tabChips_.size()) {
        this->tabsBox->setLastFocusedView(this->tabChips_[this->tab_]);
    }
}

// L/R: the tab before or after the lit one (the first with none lit). The focus follows
// onto the new chip from the tabs; from the cards clearCards() moves it there.
void StreamFeedActivity::stepTab(int delta) {
    const long current = this->tab_ < this->tabChips_.size() ? static_cast<long>(this->tab_) : -1;
    const long index = current + delta;
    if (!this->interactionReady_.load() || index < 0 || index >= static_cast<long>(this->tabChips_.size())) {
        return;
    }
    if (this->tabsBox && newpipe::focus_inside(this->tabsBox)) {
        brls::Application::giveFocus(this->tabChips_[index]);
    }
    this->selectTab(static_cast<size_t>(index));
}

void StreamFeedActivity::selectTab(size_t index) {
    if (!this->channel_ || index >= this->channel_->tabs.size() || index == this->tab_) {
        return;
    }
    this->tab_ = index;
    this->updateTabs();
    const unsigned generation = ++this->tabGeneration_;
    const newpipe::ChannelTab tab = this->channel_->tabs[index];
    this->clearCards();
    this->vertical_ = is_shorts_tab(tab.params);
    newpipe::set_grid_scrolling(this->scrollFrame, this->vertical_);
    this->tabPaging_ = true;
    this->showMessage(newpipe::tr("feed/loading"));

    std::shared_ptr<bool> alive = this->alive_;
    const std::string channel_id = this->channel_->id;
    brls::async([this, alive, generation, channel_id, tab]() {
        newpipe::YouTubeCatalogService service;
        auto feed = service.get_channel_tab(channel_id, tab.params);
        const std::string error = service.error_message();
        brls::sync([this, alive, generation, feed, error]() {
            if (!*alive || generation != this->tabGeneration_) {
                return;  // closed, or another tab was chosen meanwhile
            }
            if (!feed.has_value()) {
                this->showMessage(error.empty() ? newpipe::tr("feed/tab_failed") : error);
                return;
            }
            this->items_ = feed->items;
            adopt_channel(this->items_, 0, *this->channel_);
            this->nextPageToken_ = feed->next_page_token;
            this->nextPageAllowsShorts_ = true;
            this->showMessage(this->items_.empty() ? newpipe::tr("feed/tab_empty") : std::string());
            this->appendCards(0);
        });
    });
}

void StreamFeedActivity::showMessage(const std::string& text) {
    if (!this->messageLabel) {
        return;
    }
    this->messageLabel->setText(text);
    this->messageLabel->setVisibility(text.empty() ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
}

// Before a tab's cards go, the focus leaves them for that tab's chip: a removed card must
// not keep it.
void StreamFeedActivity::clearCards() {
    if (!this->gridBox) {
        return;
    }
    for (brls::View* view = brls::Application::getCurrentFocus(); view; view = view->getParent()) {
        if (view == this->gridBox) {
            if (this->tab_ < this->tabChips_.size()) {
                brls::Application::giveFocus(this->tabChips_[this->tab_]);
            }
            break;
        }
    }
    this->gridBox->clearViews();
    this->items_.clear();
    this->nextPageToken_.clear();
    this->loadingPage_ = false;
    if (this->scrollFrame) {
        this->scrollFrame->setContentOffsetY(0, false);
    }
}

// Cards for items_[from..]; a page that starts inside a row fills that row first.
void StreamFeedActivity::appendCards(size_t from) {
    if (!this->gridBox) {
        return;
    }
    const size_t columns = this->vertical_ ? kShortsColumns : kGridColumns;
    for (size_t j = from; j < this->items_.size(); j++) {
        // A playlist's videos are rows straight in the list, played within the playlist.
        brls::Box* row = this->gridBox;
        if (!this->playlist_) {
            row = nullptr;
            if (j % columns != 0 && !this->gridBox->getChildren().empty()) {
                row = dynamic_cast<brls::Box*>(this->gridBox->getChildren().back());
            }
            if (!row) {
                row = new brls::Box(brls::Axis::ROW);
                row->setMarginBottom(8);
                this->gridBox->addView(row);
            }
        }
        auto* card = new StreamCard(this->playlist_.has_value());
        if (this->vertical_) {
            card->setVertical();
        }
        card->setChannelShown(!this->channel_.has_value());
        card->setData(this->items_[j]);
        register_play_action(
            card,
            [this, j](brls::View*) {
                this->playStream(this->inPlaylist(this->items_[j]));
                return true;
            },
            this->items_[j].is_playlist ? newpipe::tr("hints/open") : std::string());
        card->registerAction(newpipe::tr("common/info"), brls::ControllerButton::BUTTON_Y, [this, j](brls::View*) {
            this->openStream(this->items_[j]);
            return true;
        });
        card->getFocusEvent()->subscribe([this, j](brls::View*) {
            if (j + kPrefetchCards >= this->items_.size()) {
                this->loadNextPage();
            }
        });
        card->addGestureRecognizer(new CardGesture(card));  // tap: play, hold: its page
        row->addView(card);
    }
}

void StreamFeedActivity::loadNextPage() {
    if (this->loadingPage_ || this->nextPageToken_.empty() || this->items_.size() >= kMaxItems) {
        return;
    }
    this->loadingPage_ = true;
    std::shared_ptr<bool> alive = this->alive_;
    const std::string token = this->nextPageToken_;
    const bool allow_shorts = this->nextPageAllowsShorts_;
    const bool tab_page = this->tabPaging_;
    const unsigned generation = this->tabGeneration_;
    const std::string channel_id = this->channel_ ? this->channel_->id : std::string();
    brls::async([this, alive, token, allow_shorts, tab_page, generation, channel_id]() {
        newpipe::YouTubeCatalogService service;
        // A chosen tab's pages hold Shorts and playlists too, which only its own parser reads.
        auto page = tab_page ? service.get_channel_tab(channel_id, {}, token)
                             : service.get_next_page(token, false, allow_shorts);
        brls::sync([this, alive, page, generation]() {
            if (!*alive || generation != this->tabGeneration_) {
                return;
            }
            this->loadingPage_ = false;
            if (!page.has_value() || page->items.empty()) {
                // A failed or empty page ends the list instead of retrying on every move.
                this->nextPageToken_.clear();
                return;
            }
            const size_t from = this->items_.size();
            for (const auto& item : page->items) {
                if (this->items_.size() >= kMaxItems) {
                    break;
                }
                this->items_.push_back(item);
            }
            if (this->channel_) {
                adopt_channel(this->items_, from, *this->channel_);
            }
            this->nextPageToken_ = page->next_page_token;
            this->appendCards(from);
            newpipe::logf("feed_activity: next page total=%zu more=%d", this->items_.size(),
                          this->nextPageToken_.empty() ? 0 : 1);
        });
    });
}

void StreamFeedActivity::playStream(const newpipe::StreamItem& item) {
    if (!allowInitialInput()) {
        return;
    }
    if (item.is_playlist) {
        this->openPlaylist(item);
        return;
    }
    // The watch URL is enough; the player resolves the stream itself.
    const auto request = newpipe::build_playback_request(item, std::nullopt);
    if (!request.has_value()) {
        this->openStream(item);
        return;
    }

    std::string ignored_error;
    newpipe::LibraryStore::instance().add_history(item, &ignored_error);
    newpipe::logf("feed_activity: queue playback url=%s", request->url.c_str());
    newpipe::queue_playback(*request);
    brls::Application::quit();
}

void StreamFeedActivity::openStream(const newpipe::StreamItem& item) {
    if (!allowInitialInput()) {
        return;
    }
    if (item.is_playlist) {
        this->openPlaylist(item);
        return;
    }
    brls::Application::pushActivity(new StreamDetailActivity(item));
}

// A playlist from a channel's Playlists tab opens as its own list.
void StreamFeedActivity::openPlaylist(const newpipe::StreamItem& item) {
    const newpipe::StreamItem playlist = item;
    brls::Application::pushActivity(new StreamFeedActivity(
        item.title,
        [playlist](newpipe::YouTubeCatalogService& service) {
            auto feed = service.get_playlist_feed(playlist);
            if (feed) {
                feed->kiosk.title = playlist.title;  // the name the card showed
            }
            return feed;
        },
        newpipe::tr("detail/playlist_load_failed")));
}
