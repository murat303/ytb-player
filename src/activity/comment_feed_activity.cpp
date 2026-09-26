#include "activity/comment_feed_activity.hpp"

#include "newpipe/i18n.hpp"
#include "newpipe/image_loader.hpp"
#include "newpipe/log.hpp"
#include "newpipe/youtube_catalog_service.hpp"
#include "view/chip.hpp"
#include "view/svg_image.hpp"

namespace {
// A page holds about 20 comments; past this many the list stops growing (memory).
constexpr size_t kMaxComments = 400;
// The next page is requested when the focus is this close to the end of the list.
constexpr size_t kPrefetchDistance = 5;
// After a failed page, wait before trying again on a later focus change.
constexpr auto kRetryDelay = std::chrono::seconds(5);

const NVGcolor kGray = nvgRGB(0xAA, 0xAA, 0xAA);

brls::Label* make_label(const std::string& text, float size, NVGcolor color, bool single_line = false) {
    auto* label = new brls::Label();
    label->setFontSize(size);
    label->setTextColor(color);
    label->setSingleLine(single_line);
    label->setText(text);
    return label;
}

// "1.234 yorum" -> "1.234", for the count beside the title when the panel gave none.
std::string bare_count(const std::string& title) {
    const size_t space = title.rfind(' ');
    if (title.find_first_of("0123456789") == std::string::npos) {
        return {};
    }
    return space == std::string::npos ? title : title.substr(0, space);
}
}  // namespace

CommentFeedActivity::CommentFeedActivity(newpipe::StreamItem video)
    : video_(std::move(video)) {
}

CommentFeedActivity::CommentFeedActivity(newpipe::StreamItem video, newpipe::CommentItem parent)
    : video_(std::move(video))
    , parent_(std::move(parent)) {
    this->firstPageToken_ = this->parent_->replies_token;
}

CommentFeedActivity::~CommentFeedActivity() {
    *alive_ = false;
}

void CommentFeedActivity::onContentAvailable() {
    this->registerAction(newpipe::tr("hints/back"), brls::BUTTON_B, [](brls::View*) {
        brls::Application::popActivity();
        return true;
    });

    if (this->titleLabel) {
        this->titleLabel->setText(newpipe::tr(this->parent_ ? "comments/replies_title" : "comments/title"));
    }
    if (this->subtitleLabel) {
        this->subtitleLabel->setText(this->video_.title);
    }
    // The replies screen starts with the comment they answer.
    if (this->parent_ && this->listBox) {
        this->listBox->addView(this->makeRow(*this->parent_, false));
        if (this->countLabel) {
            this->countLabel->setText(this->parent_->reply_count_text);
        }
    }
    this->loadPage();
}

void CommentFeedActivity::loadPage() {
    if (this->loading_ || this->items_.size() >= kMaxComments
        || (this->firstPageDone_ && this->nextPageToken_.empty())) {
        return;
    }
    if (this->lastFailure_ != std::chrono::steady_clock::time_point{}
        && std::chrono::steady_clock::now() - this->lastFailure_ < kRetryDelay) {
        return;
    }

    this->loading_ = true;
    this->updateStatus();
    const bool first = !this->firstPageDone_;
    const newpipe::StreamItem video = this->video_;
    const std::string token = first ? this->firstPageToken_ : this->nextPageToken_;
    const unsigned generation = this->generation_;
    std::shared_ptr<bool> alive = this->alive_;
    brls::async([this, alive, first, video, token, generation]() {
        // Its own service instance: the UI thread never touches this one.
        newpipe::YouTubeCatalogService loader;
        auto page = token.empty() ? loader.get_comments(video) : loader.get_comments_page(token);
        const std::string error = loader.error_message();
        brls::sync([this, alive, first, page, error, generation]() {
            if (!*alive || generation != this->generation_) {
                return;
            }
            this->loading_ = false;
            if (!page.has_value()) {
                this->error_ = error.empty() ? newpipe::tr("detail/comments_load_failed") : error;
                this->lastFailure_ = std::chrono::steady_clock::now();
                this->updateStatus();
                newpipe::logf("comments: page failed first=%d error=%s", first ? 1 : 0, error.c_str());
                return;
            }
            this->error_.clear();
            if (first) {
                this->firstPageDone_ = true;
                if (!this->parent_ && this->countLabel && this->sortChips_.empty()) {
                    this->countLabel->setText(page->count_text.empty() ? bare_count(page->title) : page->count_text);
                }
                if (!this->parent_ && this->sortChips_.empty()) {
                    this->showSorts(page->sorts);
                }
            }
            this->nextPageToken_ = page->next_page_token;
            this->appendRows(page->items);
            this->updateStatus();
        });
    });
}

// "En popüler" and "En yeni" as chips over the list, the shown order light.
void CommentFeedActivity::showSorts(const std::vector<newpipe::CommentSort>& sorts) {
    if (!this->sortsBox || sorts.size() < 2) {
        return;
    }
    this->sorts_ = sorts;
    for (size_t i = 0; i < sorts.size(); i++) {
        auto* chip = new Chip(sorts[i].title, [this, i]() { this->selectSort(i); }, sorts[i].selected);
        this->sortsBox->addView(chip);
        this->sortChips_.push_back(chip);
        if (sorts[i].selected) {
            this->sortsBox->setLastFocusedView(chip);
        }
    }
    this->sortsBox->setVisibility(brls::Visibility::VISIBLE);
}

// The list again from the first page of the other order.
void CommentFeedActivity::selectSort(size_t index) {
    if (index >= this->sorts_.size() || this->sorts_[index].selected) {
        return;
    }
    for (size_t i = 0; i < this->sorts_.size(); i++) {
        this->sorts_[i].selected = i == index;
        this->sortChips_[i]->setLight(i == index);
    }
    this->sortsBox->setLastFocusedView(this->sortChips_[index]);
    // A removed comment must not keep the focus.
    for (brls::View* view = brls::Application::getCurrentFocus(); view; view = view->getParent()) {
        if (view == this->listBox) {
            brls::Application::giveFocus(this->sortChips_[index]);
            break;
        }
    }
    this->generation_++;
    this->listBox->clearViews();
    this->items_.clear();
    this->firstPageDone_ = false;
    this->firstPageToken_ = this->sorts_[index].token;
    this->nextPageToken_.clear();
    this->loading_ = false;
    this->lastFailure_ = {};
    this->loadPage();
}

void CommentFeedActivity::appendRows(const std::vector<newpipe::CommentItem>& items) {
    if (!this->listBox) {
        return;
    }
    const bool first_rows = this->items_.empty();
    for (const auto& item : items) {
        if (this->items_.size() >= kMaxComments) {
            break;
        }
        this->items_.push_back(item);
        brls::Box* row = this->makeRow(item, this->parent_.has_value());
        const size_t index = this->items_.size() - 1;
        row->getFocusEvent()->subscribe([this, index](brls::View*) {
            if (index + kPrefetchDistance >= this->items_.size()) {
                this->loadPage();
            }
        });
        this->listBox->addView(row);
    }
    // Until now the focus sat on the invisible holder; hand it to the first comment. The
    // holder then steps aside: up from the top of the list must not land on it.
    if (first_rows && !this->items_.empty() && this->holderBox
        && brls::Application::getCurrentFocus() == this->holderBox) {
        brls::Application::giveFocus(this->listBox->getChildren().front());
        this->holderBox->setFocusable(false);
    }
}

// A comment as YouTube shows it: the author's round picture; beside it "@name • 2 days ago",
// the text, the likes and, on a video's comments, a blue "12 replies" that opens them.
brls::Box* CommentFeedActivity::makeRow(const newpipe::CommentItem& item, bool reply) {
    auto* row = new brls::Box(brls::Axis::ROW);
    row->setFocusable(true);
    row->setWidthPercentage(100);
    row->setPadding(12, 12, 12, reply ? 56 : 12);
    row->setCornerRadius(12);
    row->setHighlightCornerRadius(12);
    row->setMarginBottom(2);

    const float size = reply ? 32.0f : 40.0f;
    auto* avatar = new brls::Image();
    avatar->setDimensions(size, size);
    avatar->setCornerRadius(size / 2);
    avatar->setScalingType(brls::ImageScalingType::FILL);
    avatar->setBackgroundColor(nvgRGB(0x3A, 0x3A, 0x3A));
    if (!item.author_thumbnail_url.empty()) {
        newpipe::ImageLoader::instance().load(item.author_thumbnail_url, avatar);
    }
    row->addView(avatar);

    auto* text = new brls::Box(brls::Axis::COLUMN);
    text->setWidthPercentage(reply ? 89.0f : 93.0f);
    text->setMarginLeft(14);
    if (!item.pinned_text.empty()) {
        auto* pinned = make_label(item.pinned_text, 12, kGray, true);
        pinned->setMarginBottom(4);
        text->addView(pinned);
    }
    std::string head = item.author_name.empty() ? newpipe::tr("comments/unknown_author") : item.author_name;
    if (!item.published_text.empty()) {
        head += "  •  " + item.published_text;
    }
    text->addView(make_label(head, 13, item.is_creator ? nvgRGB(0xF1, 0xF1, 0xF1) : kGray, true));
    auto* body = make_label(item.body, 15, nvgRGB(0xF1, 0xF1, 0xF1));
    body->setLineHeight(1.35f);
    body->setMarginTop(4);
    text->addView(body);

    auto* actions = new brls::Box(brls::Axis::ROW);
    actions->setAlignItems(brls::AlignItems::CENTER);
    actions->setMarginTop(8);
    auto* thumb = new SVGImage();
    thumb->setDimensions(16, 16);
    thumb->setImageFromSVGRes("svg/thumb_up.svg");
    actions->addView(thumb);
    if (!item.like_count_text.empty()) {
        auto* likes = make_label(item.like_count_text, 13, kGray, true);
        likes->setMarginLeft(6);
        actions->addView(likes);
    }
    const bool opens_replies = !reply && !this->parent_ && !item.replies_token.empty();
    if (opens_replies) {
        const std::string label = item.reply_count_text.empty()
            ? newpipe::tr("comments/replies_title")
            : newpipe::tr("comments/replies", item.reply_count_text);
        auto* replies = make_label(label, 13, nvgRGB(0x3E, 0xA6, 0xFF), true);
        replies->setMarginLeft(26);
        actions->addView(replies);
    }
    text->addView(actions);
    row->addView(text);

    if (opens_replies) {
        const newpipe::CommentItem parent = item;
        row->registerAction(newpipe::tr("comments/replies_title"), brls::BUTTON_A, [this, parent](brls::View*) {
            brls::Application::pushActivity(new CommentFeedActivity(this->video_, parent));
            return true;
        });
        row->addGestureRecognizer(new brls::TapGestureRecognizer(row));
    }
    return row;
}

void CommentFeedActivity::updateStatus() {
    if (!this->statusLabel) {
        return;
    }
    const std::string count = std::to_string(this->items_.size());
    // "12 yorum ..." on a video's comments, "12 yanıt ..." on a comment's replies.
    const std::string kind = this->parent_ ? "comments/reply_count_" : "comments/count_";
    std::string text;
    if (!this->firstPageDone_) {
        text = this->loading_ ? newpipe::tr("comments/loading") : this->error_;
    } else if (this->loading_) {
        text = newpipe::tr(kind + "loading", count);
    } else if (!this->error_.empty()) {
        text = newpipe::tr(kind + "failed", count);
    } else if (this->items_.size() >= kMaxComments) {
        text = newpipe::tr(kind + "limit", count);
    } else if (this->nextPageToken_.empty()) {
        text = newpipe::tr(kind + "all", count);
    } else {
        text = newpipe::tr(kind + "more", count);
    }
    this->statusLabel->setText(text);
}
