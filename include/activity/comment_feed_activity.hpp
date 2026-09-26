#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <borealis.hpp>

#include "newpipe/models.hpp"

class Chip;

// Comments of one video, or the replies to one comment. The screen opens at once and loads
// the first page itself; later pages come while the focus nears the end of the list. A
// comment with replies opens them in another such screen (A or a tap).
class CommentFeedActivity : public brls::Activity {
public:
    explicit CommentFeedActivity(newpipe::StreamItem video);
    // The replies to `parent` (its replies_token), the comment itself shown first.
    CommentFeedActivity(newpipe::StreamItem video, newpipe::CommentItem parent);
    ~CommentFeedActivity() override;

    CONTENT_FROM_XML_RES("activity/comment_feed.xml");

    void onContentAvailable() override;

private:
    void loadPage();
    void appendRows(const std::vector<newpipe::CommentItem>& items);
    brls::Box* makeRow(const newpipe::CommentItem& item, bool reply);
    void showSorts(const std::vector<newpipe::CommentSort>& sorts);
    void selectSort(size_t index);
    void updateStatus();

    newpipe::StreamItem video_;
    std::optional<newpipe::CommentItem> parent_;
    std::vector<newpipe::CommentItem> items_;
    std::string nextPageToken_;
    // The first page comes from the video (or the parent's token, or a chosen order's token).
    std::string firstPageToken_;
    bool firstPageDone_ = false;
    bool loading_ = false;
    std::string error_;
    std::chrono::steady_clock::time_point lastFailure_{};
    std::vector<newpipe::CommentSort> sorts_;
    std::vector<Chip*> sortChips_;
    // Bumped when the order changes, so that a page of the other order is dropped.
    unsigned generation_ = 0;
    // Flipped by the destructor: a page that arrives after B was pressed is dropped.
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

    BRLS_BIND(brls::Box, holderBox, "comments/holder");
    BRLS_BIND(brls::Label, titleLabel, "comments/title");
    BRLS_BIND(brls::Label, countLabel, "comments/count");
    BRLS_BIND(brls::Label, subtitleLabel, "comments/subtitle");
    BRLS_BIND(brls::Box, sortsBox, "comments/sorts");
    BRLS_BIND(brls::Box, listBox, "comments/list");
    BRLS_BIND(brls::Label, statusLabel, "comments/status");
};
