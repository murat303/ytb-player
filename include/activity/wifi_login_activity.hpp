#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include <borealis.hpp>

#include "newpipe/cookie_receiver.hpp"

// Signing in from a computer or phone on the same Wi-Fi: the screen shows an address, the page
// there takes the YouTube cookies, and the Switch signs in with them and closes the screen.
class WifiLoginActivity : public brls::Activity {
public:
    // on_signed_in runs on the UI thread once the session is saved.
    explicit WifiLoginActivity(std::function<void()> on_signed_in);
    ~WifiLoginActivity() override;

    CONTENT_FROM_XML_RES("activity/wifi_login.xml");

    void onContentAvailable() override;

private:
    std::string receive(const std::string& cookies);
    void close();

    std::function<void()> onSignedIn_;
    newpipe::CookieReceiver receiver_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    bool closing_ = false;
    std::atomic<bool> tornDown_{false};
    // Read on the server's thread, so looked up beforehand.
    std::string failedText_;
    std::string closedText_;

    BRLS_BIND(brls::Label, titleLabel, "wifi/title");
    BRLS_BIND(brls::Label, instructionsLabel, "wifi/instructions");
    BRLS_BIND(brls::Label, addressLabel, "wifi/address");
    BRLS_BIND(brls::Label, explainLabel, "wifi/explain");
    BRLS_BIND(brls::Label, statusLabel, "wifi/status");
};
