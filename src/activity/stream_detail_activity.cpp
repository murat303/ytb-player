#include "activity/comment_feed_activity.hpp"
#include "activity/stream_detail_activity.hpp"

#include "activity/stream_feed_activity.hpp"
#include "newpipe/i18n.hpp"
#include "newpipe/image_loader.hpp"
#include "newpipe/library_store.hpp"
#include "newpipe/log.hpp"
#include "newpipe/playback_helper.hpp"
#include "newpipe/runtime.hpp"
#include "newpipe/youtube_catalog_service.hpp"
#include "view/card_gesture.hpp"
#include "view/stream_card.hpp"

namespace {

// The related videos fill the right column: 16:9 of its ~430 px.
constexpr float kRelatedThumbnailHeight = 240.0f;
// The first comment under "Comments": about two lines of the box.
constexpr size_t kCommentCharacters = 170;

// The big picture: the 1280x720 one the cards of the web client use (hq720) when the card had
// it, else hqdefault (480x360; "fill" crops its 4:3 bars away), which every video has.
std::string large_thumbnail_url(const newpipe::StreamItem& item) {
    if (item.thumbnail_url.find("/hq720.jpg") != std::string::npos || item.id.empty()) {
        return item.thumbnail_url;
    }
    return "https://i.ytimg.com/vi/" + item.id + "/hqdefault.jpg";
}

// At most `limit` characters (not bytes) of a text on one line, "…" when cut.
std::string one_paragraph(const std::string& text, size_t limit) {
    std::string flat;
    for (const char ch : text) {
        flat += (ch == '\n' || ch == '\r') ? ' ' : ch;
    }
    size_t characters = 0;
    for (size_t i = 0; i < flat.size(); i++) {
        if ((static_cast<unsigned char>(flat[i]) & 0xC0) == 0x80) {
            continue;
        }
        if (characters++ == limit) {
            return flat.substr(0, i) + "…";
        }
    }
    return flat;
}

// "1.234 yorum" -> "1.234": YouTube writes the bare count beside "Comments". A title without
// a number (the fallback "Yorumlar") gives nothing.
std::string comment_count(const std::string& title) {
    if (title.find_first_of("0123456789") == std::string::npos) {
        return {};
    }
    const size_t space = title.rfind(' ');
    if (space == std::string::npos || title.find_first_of("0123456789", space) != std::string::npos) {
        return title;
    }
    return title.substr(0, space);
}

// A tap on a view that takes no D-pad focus (the big picture) runs its click action.
void make_tappable(brls::View* view, std::function<void()> action) {
    view->registerClickAction([action](brls::View*) {
        action();
        return true;
    });
    view->addGestureRecognizer(new brls::TapGestureRecognizer(view));
}

}  // namespace

StreamDetailActivity::StreamDetailActivity(newpipe::StreamItem item)
    : item_(std::move(item)) {
}

StreamDetailActivity::~StreamDetailActivity() {
    *alive_ = false;
}

void StreamDetailActivity::onContentAvailable() {
    this->registerAction(newpipe::tr("hints/back"), brls::BUTTON_B, [](brls::View*) {
        brls::Application::popActivity();
        return true;
    });
    this->registerAction(newpipe::tr("detail/channel_action"), brls::BUTTON_X, [this](brls::View*) {
        this->openChannelFeed();
        return true;
    });
    this->registerAction(newpipe::tr("detail/favorite_action"), brls::BUTTON_RB, [this](brls::View*) {
        this->toggleFavorite();
        return true;
    });

    // Its bar would sit on the big picture; the description's box showing at the bottom
    // says that the column goes on.
    if (this->detailScroll) {
        this->detailScroll->setScrollingIndicatorVisible(false);
    }
    if (this->pictureBox) {
        make_tappable(this->pictureBox, [this]() { this->playStream(); });
    }
    if (this->channelRow) {
        make_tappable(this->channelRow, [this]() { this->openChannelFeed(); });
    }
    if (this->commentsBox) {
        make_tappable(this->commentsBox, [this]() { this->openComments(); });
    }
    if (this->commentTextLabel) {
        this->commentTextLabel->setText(newpipe::tr("comments/loading"));
    }
    if (this->relatedMessage) {
        this->relatedMessage->setText(newpipe::tr("feed/loading"));
    }

    this->buildActions();
    this->loadDetail();
    this->loadComments();
}

// The actions as pill buttons under the title, the first (play) one light, as on YouTube.
// They answer taps; A on a focused one runs it. The channel and the comments have their own
// rows below, the related videos their column.
void StreamDetailActivity::buildActions() {
    if (!this->actionsBox) {
        return;
    }
    this->actionsBox->addView(
        new Chip("▶  " + newpipe::tr("detail/play_action"), [this]() { this->playStream(); }, true));
    this->favoriteChip_ = new Chip(newpipe::tr("detail/favorite_action"), [this]() { this->toggleFavorite(); });
    this->actionsBox->addView(this->favoriteChip_);
    if (this->item_.url.find("list=") != std::string::npos) {
        this->actionsBox->addView(
            new Chip(newpipe::tr("detail/playlist_action"), [this]() { this->openPlaylistFeed(); }));
    }
}

void StreamDetailActivity::loadDetail() {
    this->showDetail();

    std::shared_ptr<bool> alive = this->alive_;
    const std::string url = this->item_.url;
    brls::async([this, alive, url]() {
        // Its own service instance: the UI thread never touches this one.
        newpipe::YouTubeCatalogService loader;
        auto detail = loader.get_stream_detail(url);
        brls::sync([this, alive, detail]() {
            if (!*alive) {
                return;
            }
            this->detailDone_ = true;
            this->detail_ = detail;
            if (detail.has_value()) {
                this->item_ = detail->item;
            }
            this->showDetail();
            this->showRelated();
        });
    });
}

// "1.234 yorum" and the first comment with its author's picture.
void StreamDetailActivity::loadComments() {
    std::shared_ptr<bool> alive = this->alive_;
    const newpipe::StreamItem item = this->item_;
    brls::async([this, alive, item]() {
        newpipe::YouTubeCatalogService loader;
        auto page = loader.get_comments(item);
        brls::sync([this, alive, page]() {
            if (!*alive) {
                return;
            }
            if (!page.has_value() || page->items.empty()) {
                if (this->commentTextLabel) {
                    this->commentTextLabel->setText(newpipe::tr("detail/comments_none"));
                }
                if (this->commentAvatar) {
                    this->commentAvatar->setVisibility(brls::Visibility::GONE);
                }
                return;
            }
            const newpipe::CommentItem& first = page->items.front();
            if (this->commentsCountLabel) {
                this->commentsCountLabel->setText(comment_count(page->title));
            }
            if (this->commentTextLabel) {
                this->commentTextLabel->setText(one_paragraph(first.body, kCommentCharacters));
            }
            if (this->commentAvatar && !first.author_thumbnail_url.empty()) {
                newpipe::ImageLoader::instance().load(first.author_thumbnail_url, this->commentAvatar);
            }
        });
    });
}

void StreamDetailActivity::showDetail() {
    if (this->titleLabel) {
        this->titleLabel->setText(
            this->item_.title.empty() ? newpipe::tr("detail/default_title") : this->item_.title);
    }

    // "576.726 görüntüleme • 4 ay önce"; the channel has its own row, the duration is on the picture.
    std::string meta;
    for (const std::string* part : {&this->item_.view_count_text, &this->item_.published_text}) {
        if (part->empty()) {
            continue;
        }
        if (!meta.empty()) {
            meta += " • ";
        }
        meta += *part;
    }
    if (this->metaLabel) {
        this->metaLabel->setText(meta);
    }
    if (this->channelLabel) {
        this->channelLabel->setText(this->item_.channel_name);
    }
    if (this->subscribersLabel) {
        this->subscribersLabel->setText(
            this->detail_.has_value() ? this->detail_->channel_subscriber_count_text : std::string());
    }

    if (this->thumbnail && !this->thumbnailShown_) {
        newpipe::ImageLoader::instance().load(large_thumbnail_url(this->item_), this->thumbnail);
        this->thumbnailShown_ = true;
    }
    const std::string badge = this->item_.is_live ? newpipe::tr("stream/live_badge") : this->item_.duration_text;
    if (this->badgeBox) {
        this->badgeBox->setVisibility(badge.empty() ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
        this->badgeBox->setBackgroundColor(this->item_.is_live ? nvgRGBA(0xCC, 0x00, 0x00, 0xE6)
                                                               : nvgRGBA(0, 0, 0, 0xCC));
    }
    if (this->badgeLabel) {
        this->badgeLabel->setText(badge);
    }
    const std::string avatar_url = this->detail_.has_value() && !this->detail_->channel_avatar_url.empty()
        ? this->detail_->channel_avatar_url
        : this->item_.channel_avatar_url;
    if (this->avatar && !avatar_url.empty() && this->avatarShown_ != avatar_url) {
        this->avatarShown_ = avatar_url;
        newpipe::ImageLoader::instance().load(avatar_url, this->avatar);
    }

    std::string body;
    if (this->detail_.has_value() && !this->detail_->description.empty()) {
        body = this->detail_->description;
    }
    if (body.empty()) {
        body = this->detailDone_ ? newpipe::tr("detail/no_description") : newpipe::tr("detail/loading");
    }
    if (this->bodyLabel) {
        this->bodyLabel->setText(body);
    }

    const bool has_channel = !this->item_.channel_id.empty() || !this->item_.channel_url.empty();
    if (this->getContentView()) {
        this->getContentView()->setActionAvailable(brls::BUTTON_X, has_channel);
    }
    this->updateFavoriteAction();
}

// The related videos as wide cards, one under another: A plays one, Y opens its page.
void StreamDetailActivity::showRelated() {
    if (this->relatedShown_ || !this->relatedBox) {
        return;
    }
    this->relatedShown_ = true;
    const bool none = !this->detail_.has_value() || this->detail_->related_items.empty();
    if (this->relatedMessage) {
        this->relatedMessage->setText(none ? newpipe::tr("detail/related_load_failed") : std::string());
        this->relatedMessage->setVisibility(none ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    }
    if (none) {
        return;
    }
    for (const newpipe::StreamItem& related : this->detail_->related_items) {
        auto* card = new StreamCard();
        card->setWide(kRelatedThumbnailHeight);
        card->setData(related);
        register_play_action(card, [this, related](brls::View*) {
            this->playRelated(related);
            return true;
        });
        card->registerAction(newpipe::tr("common/info"), brls::ControllerButton::BUTTON_Y, [related](brls::View*) {
            brls::Application::pushActivity(new StreamDetailActivity(related));
            return true;
        });
        card->addGestureRecognizer(new CardGesture(card));  // tap: play, hold: its page
        this->relatedBox->addView(card);
    }
}

void StreamDetailActivity::updateFavoriteAction() {
    const bool favorite = newpipe::LibraryStore::instance().is_favorite(this->item_.url);
    const std::string text =
        favorite ? newpipe::tr("detail/unfavorite_action") : newpipe::tr("detail/favorite_action");
    if (this->getContentView()) {
        this->getContentView()->updateActionHint(brls::BUTTON_RB, text);
    }
    if (this->favoriteChip_) {
        this->favoriteChip_->setText(text);
    }
}

void StreamDetailActivity::playStream() {
    const auto request = newpipe::build_playback_request(this->item_, this->detail_);
    if (!request.has_value()) {
        brls::Application::notify(newpipe::tr("detail/playback_url_failed"));
        return;
    }

    std::string ignored_error;
    newpipe::LibraryStore::instance().add_history(this->item_, &ignored_error);
    newpipe::queue_playback(*request);
    brls::Application::quit();
}

void StreamDetailActivity::playRelated(const newpipe::StreamItem& item) {
    const auto request = newpipe::build_playback_request(item, std::nullopt);
    if (!request.has_value()) {
        brls::Application::pushActivity(new StreamDetailActivity(item));
        return;
    }
    std::string ignored_error;
    newpipe::LibraryStore::instance().add_history(item, &ignored_error);
    newpipe::queue_playback(*request);
    brls::Application::quit();
}

void StreamDetailActivity::openChannelFeed() {
    const newpipe::StreamItem item = this->item_;
    brls::Application::pushActivity(new StreamFeedActivity(
        item.channel_name.empty() ? newpipe::tr("detail/channel_action") : item.channel_name,
        [item](newpipe::YouTubeCatalogService& service) { return service.get_channel_feed(item); },
        newpipe::tr("detail/channel_load_failed")));
}

void StreamDetailActivity::openPlaylistFeed() {
    const newpipe::StreamItem item = this->item_;
    const std::string playlist_title = newpipe::tr("detail/playlist_action");
    brls::Application::pushActivity(new StreamFeedActivity(
        playlist_title,
        [item, playlist_title](newpipe::YouTubeCatalogService& service) {
            auto feed = service.get_playlist_feed(item);
            // Upstream falls back to a fixed (Korean or English) name when the list has none.
            const std::string& title = feed ? feed->kiosk.title : playlist_title;
            if (feed && (title == "재생목록" || title == "Playlist")) {
                feed->kiosk.title = playlist_title;
            }
            return feed;
        },
        newpipe::tr("detail/playlist_load_failed")));
}

void StreamDetailActivity::openComments() {
    brls::Application::pushActivity(new CommentFeedActivity(this->item_));
}

void StreamDetailActivity::toggleFavorite() {
    bool favorite = false;
    std::string error;
    if (!newpipe::LibraryStore::instance().toggle_favorite(this->item_, &favorite, &error)) {
        brls::Application::notify(error.empty() ? newpipe::tr("detail/favorite_save_failed") : error);
        return;
    }

    this->updateFavoriteAction();
    brls::Application::notify(
        favorite ? newpipe::tr("detail/favorite_added")
                 : newpipe::tr("detail/favorite_removed"));
}
