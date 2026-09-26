#if defined(__SWITCH__)
#include <switch.h>
#endif

#include <borealis.hpp>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>

#include "activity/main_activity.hpp"
#include "newpipe/app_paths.hpp"
#include "newpipe/auth_store.hpp"
#include "newpipe/content_locale.hpp"
#include "newpipe/image_loader.hpp"
#include "newpipe/i18n.hpp"
#include "newpipe/log.hpp"
#include "newpipe/runtime.hpp"
#include "newpipe/settings_store.hpp"
#if defined(__SWITCH__) || defined(NEWPIPE_DESKTOP_PLAYER)
#include "newpipe/switch_player.hpp"
#endif
#include "tab/home_tab.hpp"
#include "tab/search_tab.hpp"
#include "tab/settings_tab.hpp"
#include "tab/subscriptions_tab.hpp"
#include "tab/library_tab.hpp"
#include "tab/notifications_tab.hpp"
#include "view/auto_tab_frame.hpp"
#include "view/page_header.hpp"
#include "view/svg_image.hpp"

namespace {

void configure_theme() {
    brls::Application::getPlatform()->setThemeVariant(brls::ThemeVariant::DARK);

    // YouTube's dark palette: a near-black ground, #272727 surfaces (chips, boxes, the focused
    // item), light text and a white focus frame instead of borealis' cyan.
    auto& dark = brls::Theme::getDarkTheme();
    dark.addColor("brls/clear", nvgRGB(15, 15, 15));
    dark.addColor("brls/background", nvgRGB(15, 15, 15));
    dark.addColor("brls/text", nvgRGB(241, 241, 241));
    dark.addColor("brls/highlight/background", nvgRGB(39, 39, 39));
    dark.addColor("brls/highlight/color1", nvgRGB(241, 241, 241));
    dark.addColor("brls/highlight/color2", nvgRGB(160, 160, 160));
    dark.addColor("brls/click_pulse", nvgRGBA(255, 255, 255, 30));
    dark.addColor("brls/applet_frame/separator", nvgRGB(48, 48, 48));
    dark.addColor("brls/sidebar/background", nvgRGB(15, 15, 15));
    dark.addColor("brls/sidebar/separator", nvgRGB(15, 15, 15));
    dark.addColor("brls/sidebar/active_item", nvgRGB(241, 241, 241));
    dark.addColor("brls/list/listItem_value_color", nvgRGB(62, 166, 255));
    dark.addColor("brls/button/primary_enabled_background", nvgRGB(241, 241, 241));
    dark.addColor("brls/button/primary_enabled_text", nvgRGB(15, 15, 15));
    dark.addColor("brls/button/default_enabled_background", nvgRGB(39, 39, 39));
    dark.addColor("brls/button/highlight_enabled_text", nvgRGB(62, 166, 255));
    dark.addColor("brls/button/highlight_disabled_text", nvgRGB(62, 166, 255));
    dark.addColor("brls/slider/line_filled", nvgRGB(255, 0, 0));
    dark.addColor("brls/spinner/bar_color", nvgRGBA(241, 241, 241, 90));

    brls::Theme::getDarkTheme().addColor("color/newpipe", nvgRGB(241, 241, 241));
    brls::Theme::getDarkTheme().addColor("color/newpipe_bg", nvgRGB(15, 15, 15));
    brls::Theme::getDarkTheme().addColor("color/newpipe_card", nvgRGB(39, 39, 39));
    brls::Theme::getDarkTheme().addColor("color/grey_1", nvgRGB(28, 28, 28));
    brls::Theme::getDarkTheme().addColor("color/grey_2", nvgRGB(36, 38, 42));
    brls::Theme::getDarkTheme().addColor("color/grey_3", nvgRGBA(160, 160, 160, 160));

    brls::Theme::getLightTheme().addColor("color/newpipe", nvgRGB(216, 67, 21));
    brls::Theme::getLightTheme().addColor("color/newpipe_bg", nvgRGB(248, 248, 248));
    brls::Theme::getLightTheme().addColor("color/newpipe_card", nvgRGB(255, 255, 255));
    brls::Theme::getLightTheme().addColor("color/grey_1", nvgRGB(255, 255, 255));
    brls::Theme::getLightTheme().addColor("color/grey_2", nvgRGB(235, 236, 238));
    brls::Theme::getLightTheme().addColor("color/grey_3", nvgRGBA(200, 200, 200, 16));

    brls::getStyle().addMetric("brls/tab_frame/sidebar_width", 160);
}

void register_views() {
    brls::Application::registerXMLView("AutoTabFrame", AutoTabFrame::create);
    brls::Application::registerXMLView("SVGImage", SVGImage::create);
    brls::Application::registerXMLView("PageHeader", PageHeader::create);
    brls::Application::registerXMLView("HomeTab", HomeTab::create);
    brls::Application::registerXMLView("SearchTab", SearchTab::create);
    brls::Application::registerXMLView("SubscriptionsTab", SubscriptionsTab::create);
    brls::Application::registerXMLView("LibraryTab", LibraryTab::create);
    brls::Application::registerXMLView("NotificationsTab", NotificationsTab::create);
    brls::Application::registerXMLView("SettingsTab", SettingsTab::create);
}

bool run_borealis_ui() {
    bool image_loader_started = false;
    newpipe::log_line("main: borealis init begin");

    if (!brls::Application::init()) {
        newpipe::log_line("main: Application::init failed");
        return false;
    }
    // What YouTube is asked for (and the service's own texts) follows the app's locale.
    newpipe::set_content_language(brls::Application::getLocale());
    newpipe::logf("main: locale=%s content=%s-%s", brls::Application::getLocale().c_str(),
                  newpipe::content_hl(), newpipe::content_gl());

    newpipe::log_line("main: createWindow");
    brls::Application::createWindow(newpipe::tr("app/title"));
    brls::Application::setGlobalQuit(false);
    configure_theme();

    newpipe::log_line("main: register XML views");
    register_views();

    newpipe::log_line("main: start ImageLoader");
    newpipe::ImageLoader::instance().start();
    image_loader_started = true;

    newpipe::log_line("main: push MainActivity");
    brls::Application::pushActivity(new MainActivity());

    const std::string playback_error = newpipe::take_last_playback_error();
    if (!playback_error.empty()) {
        brls::sync([playback_error]() {
            auto* dialog =
                new brls::Dialog(newpipe::tr("app/playback_failed", playback_error));
            dialog->addButton(newpipe::tr("hints/ok"), []() {});
            dialog->setCancelable(true);
            dialog->open();
        });
    }

    newpipe::log_line("main: enter mainLoop");
    try {
        while (brls::Application::mainLoop()) {
        }
    } catch (...) {
        if (image_loader_started) {
            newpipe::ImageLoader::instance().stop();
        }
        throw;
    }
    newpipe::log_line("main: mainLoop exit");

    if (image_loader_started) {
        newpipe::ImageLoader::instance().stop();
    }
    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
    (void) argc;
    (void) argv;

    const int copied_files = newpipe::prepare_app_folder();
    newpipe::init_log();
    newpipe::log_line("main: start");
    if (copied_files > 0) {
        newpipe::logf("main: copied %d files from switch_newpipe_* into the app folder", copied_files);
    }
#if defined(__SWITCH__)
    // A crash or HOME-close during a direct download leaves these on the SD card (they can
    // reach hundreds of MB); nothing reuses them across runs. The last three are where
    // builds before 1.1 kept them.
    for (const char* name : {"stream.cache", "audio.cache", "selected.m3u8"}) {
        std::remove(newpipe::app_file_path(name).c_str());
    }
    std::remove("sdmc:/switch/switch_newpipe_stream.cache");
    std::remove("sdmc:/switch/switch_newpipe_audio.cache");
    std::remove("sdmc:/switch/switch_newpipe_selected.m3u8");
#endif
    try {
        std::string auth_error;
        if (!newpipe::AuthStore::instance().load(&auth_error) && !auth_error.empty()) {
            newpipe::logf("main: auth load failed error=%s", auth_error.c_str());
        }
        std::string settings_error;
        if (!newpipe::SettingsStore::instance().load(&settings_error) && !settings_error.empty()) {
            newpipe::logf("main: settings load failed error=%s", settings_error.c_str());
        }
        brls::Logger::setLogLevel(brls::LogLevel::LOG_DEBUG);
        while (true) {
            brls::Platform::APP_LOCALE_DEFAULT = newpipe::locale_from_setting(
                newpipe::SettingsStore::instance().settings().language);
            newpipe::clear_pending_playback();
            if (!run_borealis_ui()) {
                newpipe::shutdown_log();
                return EXIT_FAILURE;
            }

#if defined(__SWITCH__) || defined(NEWPIPE_DESKTOP_PLAYER)
            auto pending_playback = newpipe::take_pending_playback();
            if (!pending_playback.has_value()) {
                break;
            }

            // Autoplay: the player queues the next video itself, which plays without the UI
            // in between.
            while (pending_playback.has_value()) {
                newpipe::logf("main: launch player title=%s", pending_playback->title.c_str());
                std::string playback_error;
                if (!newpipe::run_switch_player(*pending_playback, playback_error)) {
                    newpipe::logf("main: player failed error=%s", playback_error.c_str());
                    newpipe::set_last_playback_error(playback_error.empty() ? "unknown error" : playback_error);
                } else {
                    newpipe::log_line("main: player finished");
                }
                pending_playback = newpipe::take_pending_playback();
            }
            continue;
#else
            break;
#endif
        }

        newpipe::shutdown_log();
        return EXIT_SUCCESS;
    } catch (const std::exception& ex) {
        newpipe::logf("main: exception: %s", ex.what());
    } catch (...) {
        newpipe::log_line("main: unknown exception");
    }

    newpipe::shutdown_log();
    return EXIT_FAILURE;
}
