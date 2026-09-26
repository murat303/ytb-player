#include "tab/notifications_tab.hpp"

#include <cstdlib>

#include "activity/stream_detail_activity.hpp"
#include "newpipe/content_locale.hpp"
#include "newpipe/i18n.hpp"
#include "newpipe/image_loader.hpp"
#include "newpipe/library_store.hpp"
#include "newpipe/log.hpp"
#include "newpipe/playback_helper.hpp"
#include "newpipe/runtime.hpp"
#include "newpipe/youtube_catalog_service.hpp"
#include "view/card_gesture.hpp"
#include "view/stream_grid.hpp"
#include "view/tab_focus.hpp"

namespace {
constexpr int kNotificationsTabIndex = 4;
// The text beside the picture: about three lines' worth.
constexpr size_t kTextCharacters = 160;

// The video a notification was opened from, for the row to focus when the UI comes back.
std::string& return_video() {
    static std::string video_id;
    return video_id;
}

std::string clamp_text(const std::string& text, size_t limit) {
    size_t characters = 0;
    for (size_t i = 0; i < text.size(); i++) {
        if ((static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) {
            continue;
        }
        if (characters++ == limit) {
            return text.substr(0, i) + "…";
        }
    }
    return text;
}

#ifndef __SWITCH__
// Desktop screenshot tests have no YouTube session: NEWPIPE_FAKE_NOTIFICATIONS names a search
// whose videos stand in for the notifications ("<channel> yükledi: <title>").
const char* fake_notifications() {
    return std::getenv("NEWPIPE_FAKE_NOTIFICATIONS");
}
#else
const char* fake_notifications() {
    return nullptr;
}
#endif
}  // namespace

NotificationsTab::NotificationsTab() {
    this->inflateFromXMLRes("xml/tabs/notifications.xml");
    ASYNC_RETAIN
    brls::delay(700, [ASYNC_TOKEN]() {
        ASYNC_RELEASE
        interactionReady_.store(true);
    });
    this->focusVideoId_ = return_video();
    return_video().clear();
    this->refresh();
}

void NotificationsTab::onCreate() {
    this->registerTabAction(newpipe::tr("common/refresh"), brls::ControllerButton::BUTTON_X, [this](brls::View*) {
        this->refresh();
        return true;
    });
}

brls::View* NotificationsTab::getDefaultFocus() {
    if (this->listBox) {
        if (brls::View* row = this->listBox->getDefaultFocus()) {
            return row;
        }
    }
    return AttachedView::getDefaultFocus();
}

void NotificationsTab::showMessage(const std::string& text) {
    if (this->messageLabel) {
        this->messageLabel->setText(text);
        this->messageLabel->setVisibility(text.empty() ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
    }
}

// The list comes from a worker; the rows are built when it is back.
void NotificationsTab::refresh() {
    const unsigned generation = ++this->generation_;
    this->showMessage(newpipe::tr("notifications/loading"));
    const std::string fake = fake_notifications() ? fake_notifications() : "";
    ASYNC_RETAIN
    brls::async([ASYNC_TOKEN, generation, fake]() {
        newpipe::YouTubeCatalogService loader;
        std::vector<newpipe::NotificationItem> items;
        bool signed_in = true;
        if (!fake.empty()) {
            const auto results = loader.search(fake);
            for (size_t i = 0; i < results.items.size() && i < 20; i++) {
                const auto& video = results.items[i];
                newpipe::NotificationItem item;
                item.text = video.channel_name
                    + (newpipe::content_language() == newpipe::ContentLanguage::turkish ? " yükledi: " : " uploaded: ")
                    + video.title;
                item.time = video.published_text;
                item.avatar_url = video.channel_avatar_url;
                item.thumbnail_url = video.thumbnail_url;
                item.video_id = video.id;
                item.unread = i < 3;
                items.push_back(item);
            }
        } else {
            signed_in = loader.has_auth_session();
            if (signed_in) {
                items = loader.list_notifications();
            }
        }
        const std::string error = loader.error_message();
        brls::sync([ASYNC_TOKEN, generation, items, signed_in, error]() {
            ASYNC_RELEASE
            if (generation != this->generation_) {
                return;
            }
            newpipe::release_grid_focus(this, this->listBox);
            this->items_ = items;
            if (this->listBox) {
                this->listBox->clearViews();
                for (size_t i = 0; i < this->items_.size(); i++) {
                    this->listBox->addView(this->makeRow(i));
                }
            }
            if (!signed_in) {
                this->showMessage(newpipe::tr("notifications/login_required"));
            } else if (this->items_.empty()) {
                this->showMessage(error.empty() ? newpipe::tr("notifications/empty") : error);
            } else {
                this->showMessage({});
            }
            // Back from a video: its row again.
            const std::string video_id = this->focusVideoId_;
            this->focusVideoId_.clear();
            for (size_t i = 0; i < this->items_.size() && !video_id.empty(); i++) {
                if (this->items_[i].video_id == video_id) {
                    this->focusRowLater(i);
                    break;
                }
            }
        });
    });
}

// After the tab frame has settled (it hands the focus to its rail after a rebuild): the row
// of this list, if the list is still the same and the tab still there.
void NotificationsTab::focusRowLater(size_t index) {
    const unsigned generation = this->generation_;
    ASYNC_RETAIN
    brls::delay(400, [ASYNC_TOKEN, generation, index]() {
        ASYNC_RELEASE
        if (generation != this->generation_ || !this->listBox || index >= this->listBox->getChildren().size()) {
            return;
        }
        brls::Application::giveFocus(this->listBox->getChildren()[index]);
    });
}

// A notification: a blue dot when unread, the channel's picture, the text and when, the
// video's picture on the right.
brls::Box* NotificationsTab::makeRow(size_t index) {
    const newpipe::NotificationItem& item = this->items_[index];
    auto* row = new brls::Box(brls::Axis::ROW);
    row->setFocusable(true);
    row->setAlignItems(brls::AlignItems::CENTER);
    row->setPadding(10, 12, 10, 12);
    row->setCornerRadius(14);
    row->setHighlightCornerRadius(14);
    row->setMarginBottom(2);

    auto* dot = new brls::Box();
    dot->setDimensions(8, 8);
    dot->setCornerRadius(4);
    dot->setBackgroundColor(item.unread ? nvgRGB(0x3E, 0xA6, 0xFF) : nvgRGBA(0, 0, 0, 0));
    dot->setMarginRight(12);
    row->addView(dot);

    auto* avatar = new brls::Image();
    avatar->setDimensions(48, 48);
    avatar->setCornerRadius(24);
    avatar->setScalingType(brls::ImageScalingType::FILL);
    avatar->setBackgroundColor(nvgRGB(0x3A, 0x3A, 0x3A));
    if (!item.avatar_url.empty()) {
        newpipe::ImageLoader::instance().load(item.avatar_url, avatar);
    }
    row->addView(avatar);

    auto* text = new brls::Box(brls::Axis::COLUMN);
    text->setWidth(640);
    text->setMarginLeft(16);
    auto* message = new brls::Label();
    message->setFontSize(15);
    message->setLineHeight(1.35f);
    message->setTextColor(nvgRGB(0xF1, 0xF1, 0xF1));
    message->setText(clamp_text(item.text, kTextCharacters));
    text->addView(message);
    auto* time = new brls::Label();
    time->setFontSize(13);
    time->setSingleLine(true);
    time->setTextColor(nvgRGB(0xAA, 0xAA, 0xAA));
    time->setText(item.time);
    time->setMarginTop(4);
    text->addView(time);
    row->addView(text);

    auto* spacer = new brls::Box();
    spacer->setGrow(1.0f);
    row->addView(spacer);

    auto* thumbnail = new brls::Image();
    thumbnail->setDimensions(176, 99);
    thumbnail->setCornerRadius(10);
    thumbnail->setScalingType(brls::ImageScalingType::FILL);
    thumbnail->setBackgroundColor(nvgRGB(0x27, 0x27, 0x27));
    if (!item.thumbnail_url.empty()) {
        newpipe::ImageLoader::instance().load(item.thumbnail_url, thumbnail);
    }
    row->addView(thumbnail);

    if (!item.video_id.empty()) {
        register_play_action(row, [this, index](brls::View*) {
            this->playVideo(index);
            return true;
        });
        row->registerAction(newpipe::tr("common/info"), brls::ControllerButton::BUTTON_Y, [this, index](brls::View*) {
            this->openVideo(index);
            return true;
        });
        row->addGestureRecognizer(new CardGesture(row));
    }
    return row;
}

// "Linus Tech Tips yükledi: The video's title" -> the title, for the player's bar.
newpipe::StreamItem NotificationsTab::videoOf(const newpipe::NotificationItem& notification) const {
    newpipe::StreamItem item;
    item.id = notification.video_id;
    item.url = "https://www.youtube.com/watch?v=" + notification.video_id;
    const size_t colon = notification.text.find(": ");
    item.title = colon == std::string::npos ? notification.text : notification.text.substr(colon + 2);
    item.thumbnail_url = notification.thumbnail_url;
    item.channel_avatar_url = notification.avatar_url;
    return item;
}

void NotificationsTab::playVideo(size_t index) {
    if (!this->interactionReady_.load() || index >= this->items_.size()) {
        return;
    }
    const newpipe::StreamItem item = this->videoOf(this->items_[index]);
    const auto request = newpipe::build_playback_request(item, std::nullopt);
    if (!request.has_value()) {
        return;
    }
    std::string ignored_error;
    newpipe::LibraryStore::instance().add_history(item, &ignored_error);
    return_video() = item.id;
    stream_grid_state::return_tab() = kNotificationsTabIndex;
    newpipe::queue_playback(*request);
    brls::Application::quit();
}

void NotificationsTab::openVideo(size_t index) {
    if (!this->interactionReady_.load() || index >= this->items_.size()) {
        return;
    }
    brls::Application::pushActivity(new StreamDetailActivity(this->videoOf(this->items_[index])));
}
