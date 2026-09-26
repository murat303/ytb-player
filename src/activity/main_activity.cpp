#include "activity/main_activity.hpp"

#include "newpipe/i18n.hpp"
#include "newpipe/settings_store.hpp"
#include "tab/settings_tab.hpp"
#include "view/page_header.hpp"
#include "view/stream_grid.hpp"

namespace {

size_t startup_tab_index(const std::string& tab_id) {
    if (tab_id == "search") {
        return 1;
    }
    if (tab_id == "subscriptions") {
        return 2;
    }
    if (tab_id == "library") {
        return 3;
    }
    if (tab_id == "notifications") {
        return 4;
    }
    if (tab_id == "settings") {
        return 5;
    }
    return 0;
}

}  // namespace

void MainActivity::onContentAvailable() {
    this->registerAction(newpipe::tr("common/info"), brls::ControllerButton::BUTTON_Y, [this](brls::View*) {
        auto* dialog = new brls::Dialog(newpipe::tr("app/info_body", APP_VERSION));
        dialog->addButton(newpipe::tr("hints/ok"), []() {});
        dialog->setCancelable(true);
        dialog->open();
        return true;
    });

    // The page headers' search, bell and account icons open those tabs.
    PageHeader::setActions(
        [this]() {
            if (this->tabsFrame) {
                this->tabsFrame->focusTab(static_cast<int>(startup_tab_index("search")));
            }
        },
        [this]() {
            if (this->tabsFrame) {
                this->tabsFrame->focusTab(static_cast<int>(startup_tab_index("notifications")));
            }
        },
        [this]() {
            if (this->tabsFrame) {
                SettingsTab::showAccount();
                this->tabsFrame->focusTab(static_cast<int>(startup_tab_index("settings")));
            }
        });

    if (this->tabsFrame) {
        size_t index = startup_tab_index(newpipe::SettingsStore::instance().settings().startup_tab);
        // Back from a video: reopen the tab it was started from; its grid restores the position.
        int& return_tab = stream_grid_state::return_tab();
        if (return_tab >= 0) {
            index = static_cast<size_t>(return_tab);
            return_tab = -1;
        }
        this->tabsFrame->setDefaultTabIndex(index);
        this->tabsFrame->focusTab(static_cast<int>(index));
    }
}
