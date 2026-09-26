#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "newpipe/http_client.hpp"

namespace newpipe {

struct AuthSession {
    std::string cookie_header;
    std::string sapisid;
    std::string source_label;
    std::string source_path;
    std::string display_name;
    // The YouTube channel requests act for, picked in Settings. A Google account can own several
    // channels; without a page id YouTube answers for the one the browser last switched to.
    std::string page_id;
    std::string channel_name;
    std::string photo_url;  // the picked channel's picture, for the page header

    bool authenticated() const { return !cookie_header.empty() && !sapisid.empty(); }
};

std::string default_auth_import_path();
std::string default_auth_session_path();

class AuthStore {
public:
    static AuthStore& instance();

    bool load(std::string* error_message = nullptr);
    bool reload(std::string* error_message = nullptr);

    AuthSession session() const;
    bool has_session() const;

    bool update_from_cookie_header(
        const std::string& cookie_header,
        const std::string& source_label,
        std::string* error_message = nullptr);
    bool import_from_file(
        const std::string& file_path = {},
        std::string* error_message = nullptr);
    bool clear(std::string* error_message = nullptr);
    // Acts for this channel from now on (empty page_id: the session's own choice).
    bool set_identity(
        const std::string& page_id,
        const std::string& channel_name,
        const std::string& photo_url,
        std::string* error_message = nullptr);

    std::vector<HttpHeader> build_youtube_headers(
        const std::string& origin,
        const std::string& referer,
        std::string* error_message = nullptr) const;

private:
    AuthStore() = default;

    bool persist_session(std::string* error_message);

    mutable std::mutex mutex_;
    bool loaded_ = false;
    AuthSession session_;
};

}  // namespace newpipe
