#include "view/stream_grid.hpp"

#include <algorithm>
#include <map>
#include <optional>

#include "newpipe/log.hpp"
#include "newpipe/youtube_catalog_service.hpp"
#include "view/auto_tab_frame.hpp"
#include "view/stream_card.hpp"
#include "view/tab_focus.hpp"

namespace {
// After a failed page request, wait this long before retrying on a later focus change.
constexpr auto kPageRetryDelay = std::chrono::seconds(5);

// borealis' ASYNC_RETAIN / ASYNC_RELEASE spelled out for a view we only hold a pointer to:
// the view's destructor flips the token, and a callback that finds it flipped must not touch
// the view, nor this grid, which the view owns.
struct ViewToken {
    bool* deleted;
    int* counter;
};

ViewToken retain(brls::View* view) {
    if (!view->deletionToken && !view->deletionTokenCounter) {
        view->deletionToken = new bool(false);
        view->deletionTokenCounter = new int(0);
    }
    (*view->deletionTokenCounter)++;
    return {view->deletionToken, view->deletionTokenCounter};
}

// Returns false when the view is gone.
bool release(brls::View* view, ViewToken token) {
    const bool deleted = *token.deleted;
    if (*token.counter > 0) {
        (*token.counter)--;
        if (*token.counter == 0) {
            delete token.deleted;
            delete token.counter;
            if (!deleted) {
                view->deletionToken = nullptr;
                view->deletionTokenCounter = nullptr;
            }
        }
    }
    return !deleted;
}
}  // namespace

StreamGrid::StreamGrid(AttachedView* owner, CardSetup setup_card, std::function<void()> on_grow)
    : owner_(owner), setupCard_(std::move(setup_card)), onGrow_(std::move(on_grow)) {}

void StreamGrid::clear() {
    items_.clear();
    cards_.clear();
    keys_.clear();
    focused_ = 0;
    nextPageToken_.clear();
    loadingPage_ = false;
    generation_++;
    if (grid_) {
        newpipe::release_grid_focus(owner_, grid_);
        grid_->clearViews();
        // A refresh from deep in a long list left the frame scrolled past the new, short
        // content, so the tab looked empty. The frame is the nearest one above the grid: on
        // Home and Subscriptions the chips (and channels) scroll with it.
        for (brls::View* view = grid_->getParent(); view; view = view->getParent()) {
            if (auto* frame = dynamic_cast<brls::ScrollingFrame*>(view)) {
                frame->setContentOffsetY(0, false);
                break;
            }
        }
    }
}

void StreamGrid::reset(const std::vector<newpipe::StreamItem>& items) {
    clear();
    append(items);
}

size_t StreamGrid::append(const std::vector<newpipe::StreamItem>& items) {
    const size_t first_new = items_.size();
    for (const auto& item : items) {
        if (full()) {
            break;
        }
        const std::string& key = item.id.empty() ? item.url : item.id;
        if (!key.empty() && !keys_.insert(key).second) {
            continue;
        }
        items_.push_back(item);
    }
    if (!grid_) {
        return items_.size() - first_new;
    }

    size_t index = first_new;
    // Fill a partial last row first, so a page that ends mid-row leaves no gap.
    if (index % columns() != 0 && !grid_->getChildren().empty()) {
        auto* row = dynamic_cast<brls::Box*>(grid_->getChildren().back());
        for (; row && index < items_.size() && index % columns() != 0; index++) {
            row->addView(makeCard(index));
        }
    }
    while (index < items_.size()) {
        auto* row = new brls::Box(brls::Axis::ROW);
        row->setMarginBottom(8);
        for (size_t column = 0; column < columns() && index < items_.size(); column++, index++) {
            row->addView(makeCard(index));
        }
        grid_->addView(row);
    }
    return items_.size() - first_new;
}

void StreamGrid::setNextPage(const std::string& token, bool uses_search, bool allow_short_videos) {
    nextPageToken_ = token;
    nextPageUsesSearch_ = uses_search;
    nextPageAllowsShorts_ = allow_short_videos;
    lastPageFailure_ = {};
}

void StreamGrid::focusItem(size_t index) {
    if (index >= cards_.size()) {
        return;
    }
    focused_ = index;
    // The scrolling frame (NATURAL behaviour) hands focus to its top-most visible card when
    // the focused one lies outside its bounds, which sent the restore to the first rows.
    // Scroll the target row into view first. The grid may sit below other views (Home's
    // chips) inside the frame's content, so its own offset there counts too.
    float offset = 0.0f;
    brls::ScrollingFrame* frame = nullptr;
    for (brls::View* view = grid_; view && view->hasParent(); view = view->getParent()) {
        frame = dynamic_cast<brls::ScrollingFrame*>(view->getParent());
        if (frame) {
            break;
        }
        offset += view->getLocalY();
    }
    if (frame) {
        if (brls::View* row = cards_[index]->getParent()) {
            frame->setContentOffsetY(std::max(0.0f, offset + row->getLocalY() - 12.0f), false);
        }
    }
    brls::Application::giveFocus(cards_[index]);
}

void StreamGrid::saveTo(SavedStreamGrid& state, const std::string& key, const std::string& title) const {
    state.valid = !items_.empty();
    state.key = key;
    state.title = title;
    state.items = items_;
    state.next_page_token = nextPageToken_;
    state.next_page_uses_search = nextPageUsesSearch_;
    state.next_page_allows_shorts = nextPageAllowsShorts_;
    state.focus = focused_;
}

bool StreamGrid::restoreFrom(SavedStreamGrid& state, const std::string& key) {
    const bool applies = state.valid && state.key == key && !state.items.empty();
    state.valid = false;
    if (!applies) {
        return false;
    }

    reset(state.items);
    setNextPage(state.next_page_token, state.next_page_uses_search, state.next_page_allows_shorts);
    const size_t focus = std::min(state.focus, items_.size() - 1);
    focused_ = focus;
    state.items.clear();
    focusLater(focus);
    newpipe::logf("grid: restored items=%zu focus=%zu", items_.size(), focus);
    return true;
}

StreamCard* StreamGrid::makeCard(size_t index) {
    auto* card = new StreamCard();
    if (vertical_) {
        card->setVertical();
    }
    card->setData(items_[index]);
    setupCard_(card, index);
    card->getFocusEvent()->subscribe([this, index](brls::View*) { onCardFocused(index); });
    cards_.push_back(card);
    return card;
}

void StreamGrid::onCardFocused(size_t index) {
    focused_ = index;
    if (index + 2 * columns() >= items_.size()) {
        loadNextPage();
    }
}

void StreamGrid::loadNextPage() {
    if (loadingPage_ || nextPageToken_.empty() || full()) {
        return;
    }
    if (lastPageFailure_ != std::chrono::steady_clock::time_point{}
        && std::chrono::steady_clock::now() - lastPageFailure_ < kPageRetryDelay) {
        return;
    }

    loadingPage_ = true;
    const std::string token = nextPageToken_;
    const bool uses_search = nextPageUsesSearch_;
    const bool allow_shorts = nextPageAllowsShorts_;
    newpipe::logf("grid: next page request items=%zu search=%d", items_.size(), uses_search ? 1 : 0);

    const ViewToken view_token = retain(owner_);
    AttachedView* owner = owner_;
    const unsigned generation = generation_;
    brls::async([this, owner, view_token, generation, token, uses_search, allow_shorts]() {
        // A separate service instance: the tab's own one is used on the UI thread meanwhile.
        newpipe::YouTubeCatalogService pager;
        auto page = pager.get_next_page(token, uses_search, allow_shorts);
        const std::string error = pager.error_message();
        brls::sync([this, owner, view_token, generation, page, error]() {
            if (!release(owner, view_token) || generation != generation_) {
                return;
            }
            loadingPage_ = false;
            if (!page.has_value()) {
                lastPageFailure_ = std::chrono::steady_clock::now();
                newpipe::logf("grid: next page failed error=%s", error.c_str());
                return;
            }
            const size_t added = append(page->items);
            // A page that brings nothing new would repeat forever; stop there.
            nextPageToken_ = added > 0 ? page->next_page_token : std::string();
            newpipe::logf("grid: next page added=%zu total=%zu more=%d",
                          added, items_.size(), nextPageToken_.empty() ? 0 : 1);
            if (onGrow_) {
                onGrow_();
            }
        });
    });
}

void StreamGrid::focusLater(size_t index) {
    // The tab frame hands focus to its sidebar after the UI is rebuilt; move it afterwards.
    // A tab that is built later than Home (Subscriptions, reopened by the return from a video)
    // lost the card to the sidebar again, so a second move follows if the sidebar item has
    // the focus by then.
    for (const int delay_ms : {400, 1200}) {
        const ViewToken view_token = retain(owner_);
        AttachedView* owner = owner_;
        const bool retry = delay_ms > 400;
        brls::delay(delay_ms, [this, owner, view_token, index, retry]() {
            if (!release(owner, view_token)) {
                return;
            }
            if (retry && brls::Application::getCurrentFocus() != owner->getTabBar()) {
                return;
            }
            focusItem(index);
        });
    }
}

namespace stream_grid_state {

int& return_tab() {
    static int tab = -1;
    return tab;
}

SavedStreamGrid& saved(const std::string& tab) {
    static std::map<std::string, SavedStreamGrid> grids;
    return grids[tab];
}

}  // namespace stream_grid_state
