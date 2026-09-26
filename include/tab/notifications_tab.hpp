#pragma once

#include <atomic>
#include <string>
#include <vector>

#include <borealis.hpp>

#include "newpipe/models.hpp"
#include "view/auto_tab_frame.hpp"

// The account's notifications (YouTube's bell): a row each, A or a tap plays the video, Y or a
// held finger opens its page. Without a session the tab says how to sign in.
class NotificationsTab : public AttachedView {
public:
    NotificationsTab();

    void onCreate() override;
    brls::View* getDefaultFocus() override;

    static brls::View* create() { return new NotificationsTab(); }

private:
    void refresh();
    void showMessage(const std::string& text);
    brls::Box* makeRow(size_t index);
    void focusRowLater(size_t index);
    newpipe::StreamItem videoOf(const newpipe::NotificationItem& notification) const;
    void playVideo(size_t index);
    void openVideo(size_t index);

    BRLS_BIND(brls::Label, messageLabel, "notifications/message");
    BRLS_BIND(brls::Box, listBox, "notifications/list");

    std::vector<newpipe::NotificationItem> items_;
    std::string focusVideoId_;  // back from a video: the row to focus once the list is in
    unsigned generation_ = 0;
    std::atomic<bool> interactionReady_{false};
};
