#pragma once

#include <borealis.hpp>

#include <chrono>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

#include "newpipe/models.hpp"

class AttachedView;
class StreamCard;
struct SavedStreamGrid;

// Card grid shared by the Home, Subscriptions and Search tabs. It pages in more videos as the
// focus nears the end, while memory stays bounded:
// - only cards near the screen keep their thumbnail textures (StreamCard::draw; the JPEG
//   bytes stay in ImageLoader's memory cache, so scrolling back up reloads them at once),
// - the list stops growing at kMaxItems, and
// - items already shown are skipped when a later page repeats them.
class StreamGrid {
public:
    static constexpr size_t kColumns = 3;  // YouTube's tablet grid
    static constexpr size_t kShortsColumns = 6;
    static constexpr size_t kMaxItems = 300;

    using CardSetup = std::function<void(StreamCard*, size_t)>;

    // setup_card registers the tab's actions on each new card; on_grow runs after a page was
    // appended (the tabs refresh their item count there).
    StreamGrid(AttachedView* owner, CardSetup setup_card, std::function<void()> on_grow);

    void attach(brls::Box* grid) { grid_ = grid; }
    // Shorts stand upright, six to a row; takes effect with the next clear() or reset().
    void setVertical(bool vertical) { vertical_ = vertical; }
    void clear();
    void reset(const std::vector<newpipe::StreamItem>& items);
    // Returns how many of the items were new.
    size_t append(const std::vector<newpipe::StreamItem>& items);
    // Token for the page after the current items (empty: the list is complete).
    void setNextPage(const std::string& token, bool uses_search, bool allow_short_videos);

    const std::vector<newpipe::StreamItem>& items() const { return items_; }
    bool full() const { return items_.size() >= kMaxItems; }
    void focusItem(size_t index);
    // Card that had focus last, for the tab to return to (nullptr when empty).
    StreamCard* focusedCard() const { return focused_ < cards_.size() ? cards_[focused_] : nullptr; }

    // Keeps the list, paging state and focused card for the return from a video.
    void saveTo(SavedStreamGrid& state, const std::string& key, const std::string& title = {}) const;
    // Rebuilds from a saved state with the same key and moves focus back to the saved card.
    // The state is consumed; returns false (grid untouched) when it does not apply.
    bool restoreFrom(SavedStreamGrid& state, const std::string& key);

private:
    StreamCard* makeCard(size_t index);
    void onCardFocused(size_t index);
    void loadNextPage();
    void focusLater(size_t index);

    AttachedView* owner_;
    brls::Box* grid_ = nullptr;
    CardSetup setupCard_;
    std::function<void()> onGrow_;
    std::vector<newpipe::StreamItem> items_;
    std::vector<StreamCard*> cards_;
    std::unordered_set<std::string> keys_;
    size_t focused_ = 0;
    bool vertical_ = false;
    size_t columns() const { return vertical_ ? kShortsColumns : kColumns; }

    std::string nextPageToken_;
    bool nextPageUsesSearch_ = false;
    bool nextPageAllowsShorts_ = false;
    bool loadingPage_ = false;
    unsigned generation_ = 0;  // bumped by clear(), so a late page of an old list is dropped
    std::chrono::steady_clock::time_point lastPageFailure_{};
};

// The UI is torn down while a video plays and rebuilt afterwards. This keeps a tab's list,
// paging state and focused card across that gap, so returning from a video lands where the
// user left instead of on a fresh first page.
struct SavedStreamGrid {
    bool valid = false;
    std::string key;  // what the list shows (kiosk id, search query)
    std::string title;  // header text of the list, for tabs that cannot rebuild it
    std::vector<newpipe::StreamItem> items;
    std::string next_page_token;
    bool next_page_uses_search = false;
    bool next_page_allows_shorts = false;
    size_t focus = 0;
};

namespace stream_grid_state {
// Tab to open when the UI comes back after a video, or -1 for the startup tab setting.
int& return_tab();
SavedStreamGrid& saved(const std::string& tab);
}  // namespace stream_grid_state
