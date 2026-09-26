#include "view/stream_card.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

#include "newpipe/content_locale.hpp"
#include "newpipe/image_loader.hpp"
#include "newpipe/i18n.hpp"
#include "newpipe/watch_progress_store.hpp"

namespace {

// Titles get two lines on a card; about this many characters fill them.
constexpr size_t kTitleCharacters = 72;

// At most `limit` characters (not bytes) of a UTF-8 text, with "…" when cut.
std::string clamp_utf8(const std::string& text, size_t limit) {
    size_t characters = 0;
    for (size_t i = 0; i < text.size(); i++) {
        if ((static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) {
            continue;
        }
        if (characters++ == limit) {
            size_t cut = i;
            while (cut > 0 && text[cut - 1] == ' ') {
                cut--;
            }
            return text.substr(0, cut) + "…";
        }
    }
    return text;
}

// A count without its word, as the related videos beside a video have it: "7,4 Mn" (with a
// no-break space before the unit).
bool is_bare_count(std::string text) {
    for (size_t at = text.find("\xC2\xA0"); at != std::string::npos; at = text.find("\xC2\xA0", at)) {
        text.replace(at, 2, " ");
    }
    const size_t space = text.find(' ');
    const std::string number = text.substr(0, space);
    if (number.empty() || number.find_first_not_of("0123456789.,") != std::string::npos) {
        return false;
    }
    if (space == std::string::npos) {
        return true;
    }
    const std::string unit = text.substr(space + 1);
    return unit == "B" || unit == "Mn" || unit == "Mr" || unit == "K" || unit == "M";
}

// "575.358 görüntüleme" the way YouTube's cards write it: "575 B görüntüleme", and in English
// "575,358 views" as "575K views". Texts that are already short ("4,5 Mn görüntüleme",
// "4.5M views") or do not start with the count (Korean "조회수 ...회") stay as they are.
std::string short_view_count(const std::string& text) {
    const size_t space = text.find(' ');
    if (space == std::string::npos) {
        return text;
    }
    const std::string rest = text.substr(space + 1);
    for (const char* unit : {"B ", "Mn ", "Mr ", "K ", "M "}) {
        if (rest.rfind(unit, 0) == 0) {
            return text;
        }
    }
    std::string digits;
    for (const char ch : text.substr(0, space)) {
        if (ch >= '0' && ch <= '9') {
            digits += ch;
        } else if (ch != '.' && ch != ',') {
            return text;
        }
    }
    if (digits.empty() || digits.size() > 15) {
        return text;
    }
    const bool turkish = newpipe::content_language() == newpipe::ContentLanguage::turkish;
    const double count = std::stod(digits);
    char number[32];
    const char* unit = "";
    double value = count;
    if (count >= 1e9) {
        value = count / 1e9;
        unit = turkish ? " Mr" : "B";
    } else if (count >= 1e6) {
        value = count / 1e6;
        unit = turkish ? " Mn" : "M";
    } else if (count >= 1e3) {
        value = count / 1e3;
        unit = turkish ? " B" : "K";
    } else {
        return text;
    }
    // One decimal below 10 ("1,2 Mn", "1.2M"), whole numbers above ("575 B", "575K").
    std::snprintf(number, sizeof(number), value < 10.0 ? "%.1f" : "%.0f", value);
    std::string shown = number;
    if (shown.size() > 2 && shown.compare(shown.size() - 2, 2, ".0") == 0) {
        shown.resize(shown.size() - 2);
    }
    if (turkish) {
        std::replace(shown.begin(), shown.end(), '.', ',');
    }
    return shown + unit + " " + rest;
}

}  // namespace

StreamCard::StreamCard(bool row) : row_(row) {
    this->inflateFromXMLRes(row ? "xml/views/stream_row.xml" : "xml/views/stream_card.xml");
}

void StreamCard::setChannelShown(bool shown) {
    channelShown_ = shown;
}

void StreamCard::setVertical() {
    vertical_ = true;
    channelShown_ = false;
    this->setWidthPercentage(15.8f);
    if (thumbnail) {
        thumbnail->setHeight(322);  // 9:16 of the card's width
    }
}

void StreamCard::setWide(float thumbnail_height) {
    this->setWidthPercentage(100.0f);
    this->setMarginRight(0);
    this->setMarginBottom(6);
    if (thumbnail) {
        thumbnail->setHeight(thumbnail_height);
    }
}

void StreamCard::setData(const newpipe::StreamItem& item) {
    if (titleLabel) {
        // A row's title has more room: about 45 characters a line, two lines.
        const size_t limit = vertical_ ? kTitleCharacters / 2 : row_ ? 90 : kTitleCharacters;
        titleLabel->setText(clamp_utf8(item.title, limit));
    }
    if (channelLabel) {
        channelLabel->setText(item.channel_name);
        channelLabel->setVisibility(channelShown_ && !item.channel_name.empty() ? brls::Visibility::VISIBLE
                                                                                : brls::Visibility::GONE);
    }
    if (stackBox) {
        stackBox->setVisibility(item.is_playlist ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    }
    // On the thumbnail: the duration on black, LIVE on red, a playlist's video count. A Short
    // standing upright needs no "Shorts" on it.
    const std::string badge = item.is_live ? newpipe::tr("stream/live_badge") : vertical_ ? "" : item.duration_text;
    if (badgeBox) {
        badgeBox->setVisibility(badge.empty() ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
        badgeBox->setBackgroundColor(item.is_live ? nvgRGBA(0xCC, 0x00, 0x00, 0xE6) : nvgRGBA(0, 0, 0, 0xCC));
    }
    if (badgeLabel) {
        badgeLabel->setText(badge);
    }
    if (metaLabel) {
        std::string views = item.view_count_text;
        if (is_bare_count(views)) {
            views += " " + newpipe::tr("stream/views");
        }
        std::string meta = short_view_count(views);
        // A Short's narrow card has room for the views only, as on YouTube.
        if (!item.is_live && !item.published_text.empty() && !vertical_) {
            meta += (meta.empty() ? "" : " • ") + item.published_text;
        }
        if (item.is_playlist) {
            meta = newpipe::tr("stream/playlist_view");
        }
        metaLabel->setText(meta);
    }
    // Red bar over the bottom of the thumbnail: how much of the video was watched here.
    const float watched = newpipe::WatchProgressStore::instance().watched_fraction(item);
    if (progressBar) {
        progressBar->setVisibility(watched > 0.0f ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    }
    if (progressFill && watched > 0.0f) {
        progressFill->setWidthPercentage(watched * 100.0f);
    }
    // The channel's picture beside the title; without one the text takes the whole width.
    avatarUrl_ = channelShown_ && !row_ ? item.channel_avatar_url : std::string();
    if (avatar) {
        avatar->setVisibility(avatarUrl_.empty() ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
    }
    if (textBox) {
        textBox->setWidthPercentage(avatarUrl_.empty() ? 100.0f : 86.0f);
    }
    thumbnailUrl_ = item.thumbnail_url;
    thumbnailLoaded_ = false;
}

// Cards are boxes, which borealis draws every frame wherever they are; y is the card's place
// on the screen. Within half a screen above or a screen below the view the picture loads;
// past a screen and a half above or two screens below it goes. Scrolled far down a long list
// that keeps about three screens of cards (~25 textures) in memory.
void StreamCard::draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style,
                      brls::FrameContext* ctx) {
    const float screen = brls::Application::contentHeight;
    if (y + height > -0.5f * screen && y < 2.0f * screen) {
        ensureThumbnail();
    } else if (y + height < -1.5f * screen || y > 3.0f * screen) {
        releaseThumbnail();
    }
    brls::Box::draw(vg, x, y, width, height, style, ctx);
}

void StreamCard::releaseThumbnail() {
    if (!thumbnailLoaded_ || !thumbnail) {
        return;
    }
    thumbnail->clear();
    if (avatar && !avatarUrl_.empty()) {
        avatar->clear();
    }
    thumbnailLoaded_ = false;
}

void StreamCard::ensureThumbnail() {
    if (thumbnailLoaded_ || !thumbnail || thumbnailUrl_.empty()) {
        return;
    }
    newpipe::ImageLoader::instance().load(thumbnailUrl_, thumbnail);
    if (avatar && !avatarUrl_.empty()) {
        newpipe::ImageLoader::instance().load(avatarUrl_, avatar);
    }
    thumbnailLoaded_ = true;
}
