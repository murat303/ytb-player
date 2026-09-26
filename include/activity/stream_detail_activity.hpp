#pragma once

#include <memory>
#include <optional>
#include <string>

#include <borealis.hpp>

#include "newpipe/models.hpp"
#include "view/chip.hpp"

// A video's page, laid out like YouTube's watch page on a tablet: the picture, title and
// actions, the channel, the comments' start and the description on the left, the related
// videos on the right. What the card already knows shows at once; the description, the
// channel's picture and subscribers and the related videos come from one worker request, the
// comment count and the first comment from another.
class StreamDetailActivity : public brls::Activity {
public:
    explicit StreamDetailActivity(newpipe::StreamItem item);
    ~StreamDetailActivity() override;

    CONTENT_FROM_XML_RES("activity/stream_detail.xml");

    void onContentAvailable() override;

private:
    void loadDetail();
    void loadComments();
    void buildActions();
    void showDetail();
    void showRelated();
    void updateFavoriteAction();
    void playStream();
    void playRelated(const newpipe::StreamItem& item);
    void openChannelFeed();
    void openPlaylistFeed();
    void openComments();
    void toggleFavorite();

    newpipe::StreamItem item_;
    std::optional<newpipe::StreamDetail> detail_;
    bool detailDone_ = false;  // the request came back (with or without a detail)
    bool relatedShown_ = false;
    // Flipped by the destructor: a detail that arrives after B was pressed is dropped.
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    Chip* favoriteChip_ = nullptr;
    bool thumbnailShown_ = false;
    std::string avatarShown_;  // the avatar URL already handed to the image

    BRLS_BIND(brls::Label, titleLabel, "detail/title");
    BRLS_BIND(brls::Label, metaLabel, "detail/meta");
    BRLS_BIND(brls::Label, bodyLabel, "detail/body");
    BRLS_BIND(brls::ScrollingFrame, detailScroll, "detail/scroll");
    BRLS_BIND(brls::Box, pictureBox, "detail/picture");
    BRLS_BIND(brls::Image, thumbnail, "detail/thumbnail");
    BRLS_BIND(brls::Box, badgeBox, "detail/badge");
    BRLS_BIND(brls::Label, badgeLabel, "detail/badge_text");
    BRLS_BIND(brls::Box, channelRow, "detail/channel_row");
    BRLS_BIND(brls::Image, avatar, "detail/avatar");
    BRLS_BIND(brls::Label, channelLabel, "detail/channel");
    BRLS_BIND(brls::Label, subscribersLabel, "detail/subscribers");
    BRLS_BIND(brls::Box, actionsBox, "detail/actions");
    BRLS_BIND(brls::Box, commentsBox, "detail/comments");
    BRLS_BIND(brls::Label, commentsCountLabel, "detail/comments_count");
    BRLS_BIND(brls::Image, commentAvatar, "detail/comment_avatar");
    BRLS_BIND(brls::Label, commentTextLabel, "detail/comment_text");
    BRLS_BIND(brls::Label, relatedMessage, "detail/related_message");
    BRLS_BIND(brls::Box, relatedBox, "detail/related");
};
