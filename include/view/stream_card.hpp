#pragma once

#include <borealis.hpp>

#include "newpipe/models.hpp"

// A video: a card of a grid (thumbnail over the text) or, with `row`, a row of a list
// (thumbnail beside the text, stream_row.xml).
class StreamCard : public brls::Box {
public:
    explicit StreamCard(bool row = false);

    // Records the thumbnail's URL; draw() loads it once the card is near the screen.
    void setData(const newpipe::StreamItem& item);

    // Both before setData. On a channel's own page its name and picture are left off the cards;
    // a Short stands upright, six to a row, with only its title and views.
    void setChannelShown(bool shown);
    void setVertical();
    // A card as wide as its column (the related videos beside a video's page).
    void setWide(float thumbnail_height);

    // Frees the thumbnail texture of a card far from the screen. The JPEG stays in ImageLoader's
    // memory cache, so ensureThumbnail() brings it back without a download in most cases.
    void releaseThumbnail();
    void ensureThumbnail();

    // Loads or frees the thumbnail by where the card is drawn, so that a list moved by a
    // finger (the focus stays behind) gets its pictures as well as one moved by the D-pad.
    void draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style,
              brls::FrameContext* ctx) override;

private:
    BRLS_BIND(brls::Image, thumbnail, "stream/thumbnail");
    BRLS_BIND(brls::Image, avatar, "stream/avatar");
    BRLS_BIND(brls::Box, textBox, "stream/text");
    BRLS_BIND(brls::Label, titleLabel, "stream/title");
    BRLS_BIND(brls::Label, channelLabel, "stream/channel");
    BRLS_BIND(brls::Label, metaLabel, "stream/meta");
    BRLS_BIND(brls::Box, badgeBox, "stream/badge");
    BRLS_BIND(brls::Label, badgeLabel, "stream/badge_text");
    BRLS_BIND(brls::Box, progressBar, "stream/progress");
    BRLS_BIND(brls::Box, progressFill, "stream/progress_fill");
    BRLS_BIND(brls::Box, stackBox, "stream/stack");

    std::string thumbnailUrl_;
    std::string avatarUrl_;
    bool thumbnailLoaded_ = false;
    bool channelShown_ = true;
    bool vertical_ = false;
    bool row_ = false;
};
