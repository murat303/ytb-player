#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace newpipe {

// Signing in from another device: a one-page web server on the local network. The page takes
// the YouTube cookies (a cookies.txt file or pasted text) and hands them to the handler, whose
// answer ("" when saved, else why not) the page shows. Only the address with the random code
// the Switch shows is answered, and only while start() runs.
class CookieReceiver {
public:
    // The page's texts, in the app's language.
    struct Texts {
        std::string lang;     // "tr", "en", "ko"
        std::string title;    // the browser tab's
        std::string heading;  // under the logo
        std::string intro;
        // In a step, `text` is shown as code and "%links%" becomes buttons to the cookie
        // extension's stores; not "{links}", which the translations' formatting would take for
        // an argument.
        std::vector<std::string> steps;
        std::string choose;  // the drop area: "Choose cookies.txt"
        std::string drop;    // "or drag and drop it here"
        std::string chosen;  // "Chosen:" before the file's name
        std::string paste;   // opens the text box
        std::string placeholder;
        std::string send;
        std::string saved_title;
        std::string saved;  // under saved_title
        std::string empty;
        std::string privacy;
    };
    // Runs on the server's thread.
    using Handler = std::function<std::string(const std::string& cookies)>;

    CookieReceiver() = default;
    CookieReceiver(const CookieReceiver&) = delete;
    CookieReceiver& operator=(const CookieReceiver&) = delete;
    ~CookieReceiver();

    // Listens on port 8080 (or the next free one up to 8089); false without a network address.
    bool start(Texts texts, Handler handler);
    // Stops listening and waits for the server's thread.
    void stop();

    // "http://192.168.1.23:8080/k7p3" once started.
    const std::string& address() const { return address_; }

private:
    void run();
    void serve(int client);
    std::string page(const std::string& message, bool saved) const;

    Texts texts_;
    Handler handler_;
    std::string path_;
    std::string address_;
    int listen_fd_ = -1;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

}  // namespace newpipe
