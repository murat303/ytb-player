#include "activity/wifi_login_activity.hpp"

#include <chrono>
#include <future>

#include "newpipe/content_locale.hpp"
#include "newpipe/i18n.hpp"
#include "newpipe/log.hpp"
#include "newpipe/youtube_catalog_service.hpp"

WifiLoginActivity::WifiLoginActivity(std::function<void()> on_signed_in)
    : onSignedIn_(std::move(on_signed_in)) {
}

WifiLoginActivity::~WifiLoginActivity() {
    *alive_ = false;
    this->tornDown_.store(true);
    // Before the members go: the server's thread may be inside receive().
    this->receiver_.stop();
}

void WifiLoginActivity::onContentAvailable() {
    this->registerAction(newpipe::tr("hints/back"), brls::BUTTON_B, [this](brls::View*) {
        this->close();
        return true;
    });

    this->failedText_ = newpipe::tr("wifi_login/failed");
    this->closedText_ = newpipe::tr("wifi_login/closed");
    this->titleLabel->setText(newpipe::tr("wifi_login/title"));
    this->instructionsLabel->setText(newpipe::tr("wifi_login/instructions"));
    this->explainLabel->setText(newpipe::tr("wifi_login/explain"));

    newpipe::CookieReceiver::Texts texts;
    texts.lang = newpipe::content_hl();
    texts.title = newpipe::tr("wifi_login/page/title");
    texts.heading = newpipe::tr("wifi_login/page/heading");
    texts.intro = newpipe::tr("wifi_login/page/intro");
    for (const char* step : {"step1", "step2", "step3", "step4", "step5"}) {
        texts.steps.push_back(newpipe::tr(std::string("wifi_login/page/") + step));
    }
    texts.choose = newpipe::tr("wifi_login/page/choose");
    texts.drop = newpipe::tr("wifi_login/page/drop");
    texts.chosen = newpipe::tr("wifi_login/page/chosen");
    texts.paste = newpipe::tr("wifi_login/page/paste");
    texts.placeholder = newpipe::tr("wifi_login/page/placeholder");
    texts.send = newpipe::tr("wifi_login/page/send");
    texts.saved_title = newpipe::tr("wifi_login/page/saved_title");
    texts.saved = newpipe::tr("wifi_login/page/saved");
    texts.empty = newpipe::tr("wifi_login/page/empty");
    texts.privacy = newpipe::tr("wifi_login/page/privacy");
    if (!this->receiver_.start(texts, [this](const std::string& cookies) { return this->receive(cookies); })) {
        this->instructionsLabel->setVisibility(brls::Visibility::GONE);
        this->explainLabel->setVisibility(brls::Visibility::GONE);
        this->addressLabel->setText(newpipe::tr("wifi_login/no_network"));
        this->addressLabel->setFontSize(22);
        this->addressLabel->setTextColor(nvgRGB(0xF1, 0xF1, 0xF1));
        return;
    }
    this->addressLabel->setText(this->receiver_.address());
    this->statusLabel->setText(newpipe::tr("wifi_login/waiting"));
}

// On the server's thread: the session is saved on the UI thread, and the page waits for the
// outcome to show it.
std::string WifiLoginActivity::receive(const std::string& cookies) {
    auto outcome = std::make_shared<std::promise<std::string>>();
    std::future<std::string> answer = outcome->get_future();
    std::shared_ptr<bool> alive = this->alive_;
    brls::sync([this, alive, outcome, cookies]() {
        if (!*alive) {
            outcome->set_value(std::string());  // nobody reads it any more
            return;
        }
        if (this->closing_) {
            outcome->set_value(this->closedText_);
            return;
        }
        newpipe::YouTubeCatalogService service;
        std::string error;
        if (!service.update_auth_session_from_cookie(cookies, "wifi", &error)) {
            outcome->set_value(error.empty() ? this->failedText_ : error);
            return;
        }
        outcome->set_value(std::string());
        newpipe::log_line("wifi login: signed in");
        this->statusLabel->setText(newpipe::tr("wifi_login/done"));
        this->statusLabel->setTextColor(nvgRGB(0x8B, 0xC3, 0x4A));
        brls::Application::notify(newpipe::tr("wifi_login/done"));
        if (this->onSignedIn_) {
            this->onSignedIn_();
        }
        brls::delay(1500, [this, alive]() {
            if (*alive) {
                this->close();
            }
        });
    });
    // The UI thread answers at its next frame. While the screen is being torn down it cannot (it
    // waits for this thread), so that is checked between short waits.
    for (int waited = 0; waited < 80; waited++) {
        if (answer.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready) {
            return answer.get();
        }
        if (this->tornDown_.load()) {
            return this->closedText_;
        }
    }
    return this->failedText_;
}

void WifiLoginActivity::close() {
    if (this->closing_) {
        return;
    }
    this->closing_ = true;
    brls::Application::popActivity();
}
