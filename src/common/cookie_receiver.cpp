#include "newpipe/cookie_receiver.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <chrono>
#include <random>

#ifndef __SWITCH__
#include <ifaddrs.h>
#endif

#include "newpipe/log.hpp"

namespace newpipe {
namespace {

constexpr int kFirstPort = 8080;
constexpr int kLastPort = 8089;
// A browser's whole cookie jar is far below this; a youtube.com cookies.txt is a few KB.
constexpr size_t kMaxBodyBytes = 1024 * 1024;
constexpr size_t kMaxHeaderBytes = 16 * 1024;
// "Get cookies.txt LOCALLY" (open source, sends nothing anywhere), the extension yt-dlp suggests.
constexpr const char* kStores =
    "<span class=\"stores\">"
    "<a href=\"https://chromewebstore.google.com/detail/get-cookiestxt-locally/cclelndahbckbenkjhflpdbgdldlbecc\">"
    "Chrome / Edge</a>"
    "<a href=\"https://addons.mozilla.org/firefox/addon/get-cookies-txt-locally/\">Firefox</a></span>";

// The page looks like the app: YouTube's dark colors and the app's disc as the logo. Nothing
// is loaded from elsewhere.
constexpr const char* kStyle = R"css(
:root{--bg:#0f0f0f;--card:#181818;--line:#2f2f2f;--chip:#272727;--text:#f1f1f1;--muted:#aaa;--link:#3ea6ff}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);font:16px/1.6 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
main{max-width:660px;margin:0 auto;padding:40px 20px 56px}
header{display:flex;align-items:center;gap:14px;margin-bottom:24px}
header svg{width:52px;height:52px;flex:none}
h1{font-size:24px;margin:0;line-height:1.2}
header p{margin:4px 0 0;color:var(--muted);font-size:15px}
.card{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:26px}
.intro{margin:0 0 16px;color:var(--muted)}
ol{list-style:none;counter-reset:step;margin:0;padding:0}
ol li{counter-increment:step;position:relative;padding:3px 0 16px 46px}
ol li::before{content:counter(step);position:absolute;left:0;top:1px;width:30px;height:30px;border-radius:50%;
background:var(--chip);display:flex;align-items:center;justify-content:center;font-weight:600;font-size:14px}
code{background:var(--chip);border-radius:6px;padding:2px 7px;font:14px ui-monospace,Consolas,monospace}
.stores{display:flex;flex-wrap:wrap;gap:10px;margin-top:10px}
.stores a{padding:7px 18px;border-radius:18px;background:var(--chip);color:var(--text);text-decoration:none;
font-size:14px;font-weight:600}
.stores a:hover{background:#3a3a3a}
.drop{display:block;margin-top:6px;border:2px dashed #3d3d3d;border-radius:14px;padding:22px 16px;text-align:center;
color:var(--muted);cursor:pointer}
.drop:hover,.drop.over{border-color:var(--link)}
.drop strong{display:block;color:var(--text);font-size:17px;margin-bottom:2px}
.drop.chosen{border-style:solid;border-color:#2ba640}
.drop.chosen strong{color:#6fdc8c}
details{margin-top:14px;color:var(--muted);font-size:15px}
summary{cursor:pointer}
textarea{width:100%;height:150px;margin-top:10px;background:#101010;color:var(--text);border:1px solid var(--line);
border-radius:10px;padding:10px;font:13px/1.4 ui-monospace,Consolas,monospace}
button{margin-top:20px;width:100%;padding:14px;border:0;border-radius:26px;background:linear-gradient(135deg,#ff8a24,#e91e63);
color:#fff;font-size:17px;font-weight:700;cursor:pointer}
button:disabled{opacity:.4;cursor:default}
.note{display:flex;gap:8px;margin:18px 0 0;color:var(--muted);font-size:14px}
.note svg{width:18px;height:18px;flex:none;margin-top:2px;fill:currentColor}
.error{background:rgba(255,78,69,.12);border:1px solid rgba(255,78,69,.35);color:#ff8a80;border-radius:12px;
padding:12px 16px;margin-bottom:18px}
.done{text-align:center;padding:44px 24px}
.check{width:76px;height:76px;border-radius:50%;background:#2ba640;display:inline-flex;align-items:center;
justify-content:center;margin-bottom:14px}
.check svg{width:44px;height:44px;fill:#fff}
.done h2{margin:0 0 6px;font-size:22px}
.done p{margin:0;color:var(--muted)}
)css";

constexpr const char* kLogo =
    "<svg viewBox=\"0 0 24 24\" aria-hidden=\"true\"><defs><linearGradient id=\"disc\" x1=\"0\" y1=\"0\" x2=\"1\" y2=\"1\">"
    "<stop offset=\"0\" stop-color=\"#ff8a24\"/><stop offset=\"1\" stop-color=\"#e91e63\"/></linearGradient></defs>"
    "<circle cx=\"12\" cy=\"12\" r=\"12\" fill=\"url(#disc)\"/><path d=\"M10.05 8.84 15.96 12 10.05 15.16Z\" "
    "fill=\"#fff\" stroke=\"#fff\" stroke-width=\"2.96\" stroke-linejoin=\"round\"/></svg>";

// Material icons (Google, Apache 2.0): a lock and a check mark.
constexpr const char* kLockIcon =
    "<svg viewBox=\"0 0 24 24\" aria-hidden=\"true\"><path d=\"M18 8h-1V6c0-2.76-2.24-5-5-5S7 3.24 7 6v2H6c-1.1 0-2 "
    ".9-2 2v10c0 1.1.9 2 2 2h12c1.1 0 2-.9 2-2V10c0-1.1-.9-2-2-2zm-6 9c-1.1 0-2-.9-2-2s.9-2 2-2 2 .9 2 2-.9 2-2 "
    "2zm3.1-9H8.9V6c0-1.71 1.39-3.1 3.1-3.1 1.71 0 3.1 1.39 3.1 3.1v2z\"/></svg>";
constexpr const char* kCheckIcon =
    "<svg viewBox=\"0 0 24 24\" aria-hidden=\"true\"><path d=\"M9 16.17 4.83 12l-1.42 1.41L9 19 21 7l-1.41-1.41z\"/></svg>";

// The file, chosen or dropped, is read into the text box; Send waits for something to send.
// Its own scope: a global "name" would be the window's name, not the element.
constexpr const char* kScript = R"js(
(function(){
var file=document.getElementById('file'),text=document.getElementById('cookies'),drop=document.getElementById('drop'),
label=document.getElementById('name'),send=document.getElementById('send');
function ready(){send.disabled=!text.value.trim();}
function load(f){if(!f)return;var r=new FileReader();r.onload=function(){text.value=r.result;
label.textContent=drop.dataset.chosen+' '+f.name;drop.classList.add('chosen');ready();};r.readAsText(f);}
file.onchange=function(){load(file.files[0]);};
drop.ondragover=function(e){e.preventDefault();drop.classList.add('over');};
drop.ondragleave=function(){drop.classList.remove('over');};
drop.ondrop=function(e){e.preventDefault();drop.classList.remove('over');load(e.dataTransfer.files[0]);};
text.oninput=ready;ready();
})();
)js";

// The console's address on the network ("" without one).
std::string local_ipv4() {
#ifdef __SWITCH__
    in_addr address{};
    address.s_addr = static_cast<in_addr_t>(gethostid());
    if (address.s_addr == 0 || address.s_addr == htonl(INADDR_LOOPBACK)) {
        return {};
    }
    return inet_ntoa(address);
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) {
        return {};
    }
    std::string found;
    for (ifaddrs* entry = list; entry && found.empty(); entry = entry->ifa_next) {
        if (!entry->ifa_addr || entry->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        const auto* in = reinterpret_cast<const sockaddr_in*>(entry->ifa_addr);
        if ((ntohl(in->sin_addr.s_addr) >> 24) == 127) {
            continue;
        }
        char text[INET_ADDRSTRLEN] = {};
        if (inet_ntop(AF_INET, &in->sin_addr, text, sizeof(text))) {
            found = text;
        }
    }
    freeifaddrs(list);
    return found;
#endif
}

std::string html_escape(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            default:
                out += c;
        }
    }
    return out;
}

// A step for the page: escaped, `text` as code, %links% as the store buttons.
std::string rich_text(const std::string& text) {
    const std::string escaped = html_escape(text);
    std::string out;
    bool in_code = false;
    for (const char c : escaped) {
        if (c == '`') {
            out += in_code ? "</code>" : "<code>";
            in_code = !in_code;
        } else {
            out += c;
        }
    }
    if (in_code) {
        out += "</code>";
    }
    const std::string marker = "%links%";
    const size_t links = out.find(marker);
    if (links != std::string::npos) {
        out.replace(links, marker.size(), kStores);
    }
    return out;
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

// A form field of an application/x-www-form-urlencoded body ("+" a space, "%xx" a byte).
std::string form_field(const std::string& body, const std::string& name) {
    const std::string key = name + "=";
    size_t start = 0;
    while (start <= body.size()) {
        size_t end = body.find('&', start);
        if (end == std::string::npos) {
            end = body.size();
        }
        if (body.compare(start, key.size(), key) == 0) {
            std::string value;
            for (size_t i = start + key.size(); i < end; i++) {
                if (body[i] == '+') {
                    value += ' ';
                } else if (body[i] == '%' && i + 2 < end && hex_value(body[i + 1]) >= 0 && hex_value(body[i + 2]) >= 0) {
                    value += static_cast<char>(hex_value(body[i + 1]) * 16 + hex_value(body[i + 2]));
                    i += 2;
                } else {
                    value += body[i];
                }
            }
            return value;
        }
        start = end + 1;
    }
    return {};
}

void send_all(int fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n = send(fd, data.data() + sent, data.size() - sent, 0);
        if (n <= 0) {
            return;
        }
        sent += static_cast<size_t>(n);
    }
}

void respond(int fd, const char* status, const std::string& html) {
    send_all(fd, std::string("HTTP/1.1 ") + status +
                     "\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\nConnection: close"
                     "\r\nContent-Length: " +
                     std::to_string(html.size()) + "\r\n\r\n" + html);
}

}  // namespace

CookieReceiver::~CookieReceiver() {
    stop();
}

bool CookieReceiver::start(Texts texts, Handler handler) {
    stop();
    const std::string ip = local_ipv4();
    if (ip.empty()) {
        log_line("wifi login: no network address");
        return false;
    }
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        log_line("wifi login: socket failed");
        return false;
    }
    const int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    int port = 0;
    for (int candidate = kFirstPort; candidate <= kLastPort && port == 0; candidate++) {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = htons(static_cast<uint16_t>(candidate));
        if (bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) {
            port = candidate;
        }
    }
    if (port == 0 || listen(fd, 4) != 0) {
        log_line("wifi login: no free port");
        close(fd);
        return false;
    }

    // Four characters that cannot be mistaken for each other when typed from the screen.
    static const char kLetters[] = "abcdefghjkmnpqrstuvwxyz23456789";
    std::mt19937 random(static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::uniform_int_distribution<size_t> pick(0, sizeof(kLetters) - 2);
    std::string code;
    for (int i = 0; i < 4; i++) {
        code += kLetters[pick(random)];
    }

    this->texts_ = std::move(texts);
    this->handler_ = std::move(handler);
    this->path_ = "/" + code;
    this->address_ = "http://" + ip + ":" + std::to_string(port) + this->path_;
    this->listen_fd_ = fd;
    this->stop_.store(false);
    this->thread_ = std::thread([this]() { this->run(); });
    logf("wifi login: listening at %s", this->address_.c_str());
    return true;
}

void CookieReceiver::stop() {
    this->stop_.store(true);
    if (this->thread_.joinable()) {
        this->thread_.join();
    }
    if (this->listen_fd_ >= 0) {
        close(this->listen_fd_);
        this->listen_fd_ = -1;
    }
}

// Polls with a short timeout so that stop() is seen within a quarter second.
void CookieReceiver::run() {
    while (!this->stop_.load()) {
        pollfd waiting{};
        waiting.fd = this->listen_fd_;
        waiting.events = POLLIN;
        if (poll(&waiting, 1, 250) <= 0 || !(waiting.revents & POLLIN)) {
            continue;
        }
        const int client = accept(this->listen_fd_, nullptr, nullptr);
        if (client < 0) {
            continue;
        }
        this->serve(client);
        close(client);
    }
}

void CookieReceiver::serve(int client) {
    timeval timeout{};
    timeout.tv_sec = 5;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    std::string request;
    char buffer[4096];
    size_t header_end = std::string::npos;
    while (header_end == std::string::npos && request.size() < kMaxHeaderBytes) {
        const ssize_t got = recv(client, buffer, sizeof(buffer), 0);
        if (got <= 0) {
            return;
        }
        request.append(buffer, static_cast<size_t>(got));
        header_end = request.find("\r\n\r\n");
    }
    if (header_end == std::string::npos) {
        return;
    }
    std::string head = request.substr(0, header_end);
    std::string body = request.substr(header_end + 4);

    const size_t line_end = head.find("\r\n");
    const std::string line = head.substr(0, line_end);
    const size_t method_end = line.find(' ');
    const size_t path_end = method_end == std::string::npos ? std::string::npos : line.find(' ', method_end + 1);
    if (path_end == std::string::npos) {
        return;
    }
    const std::string method = line.substr(0, method_end);
    std::string path = line.substr(method_end + 1, path_end - method_end - 1);
    if (path.size() > 1 && path.back() == '/') {
        path.pop_back();
    }

    std::transform(head.begin(), head.end(), head.begin(), [](unsigned char c) { return std::tolower(c); });
    size_t length = 0;
    const size_t length_at = head.find("\r\ncontent-length:");
    if (length_at != std::string::npos) {
        length = std::strtoul(head.c_str() + length_at + 17, nullptr, 10);
    }
    if (length > kMaxBodyBytes) {
        respond(client, "413 Payload Too Large", this->page(this->texts_.empty, false));
        return;
    }
    while (body.size() < length) {
        const ssize_t got = recv(client, buffer, sizeof(buffer), 0);
        if (got <= 0) {
            return;
        }
        body.append(buffer, static_cast<size_t>(got));
    }

    if (path != this->path_) {
        respond(client, "404 Not Found", "<!doctype html><title>404</title>404");
        return;
    }
    if (method != "POST") {
        respond(client, "200 OK", this->page({}, false));
        return;
    }
    std::string cookies = form_field(body, "cookies");
    // Browsers send a text area's lines ending in CRLF.
    cookies.erase(std::remove(cookies.begin(), cookies.end(), '\r'), cookies.end());
    const bool blank = cookies.find_first_not_of(" \n\t") == std::string::npos;
    const std::string error = blank ? this->texts_.empty : this->handler_(cookies);
    logf("wifi login: cookies %s (%zu bytes)", error.empty() ? "saved" : "refused", cookies.size());
    respond(client, "200 OK", this->page(error.empty() ? this->texts_.saved : error, error.empty()));
}

// The form, under the reason when the last cookies were refused; after a save only the
// confirmation.
std::string CookieReceiver::page(const std::string& message, bool saved) const {
    const Texts& t = this->texts_;
    std::string html = "<!doctype html><html lang=\"" + html_escape(t.lang) + "\"><head><meta charset=\"utf-8\">"
                       "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>" +
                       html_escape(t.title) + "</title><style>" + kStyle + "</style></head><body><main><header>" +
                       kLogo + "<div><h1>YTB Player</h1><p>" + html_escape(t.heading) + "</p></div></header>";
    if (saved) {
        return html + "<div class=\"card done\"><div class=\"check\">" + kCheckIcon + "</div><h2>" +
               html_escape(t.saved_title) + "</h2><p>" + html_escape(message) + "</p></div></main></body></html>";
    }
    if (!message.empty()) {
        html += "<div class=\"error\">" + html_escape(message) + "</div>";
    }
    html += "<div class=\"card\"><p class=\"intro\">" + html_escape(t.intro) + "</p><ol>";
    for (const std::string& step : t.steps) {
        html += "<li>" + rich_text(step) + "</li>";
    }
    html += "</ol><form method=\"post\"><label class=\"drop\" id=\"drop\" data-chosen=\"" + html_escape(t.chosen) +
            "\"><input type=\"file\" id=\"file\" accept=\".txt,.json,text/plain\" hidden><strong id=\"name\">" +
            html_escape(t.choose) + "</strong>" + html_escape(t.drop) + "</label><details><summary>" +
            html_escape(t.paste) + "</summary><textarea name=\"cookies\" id=\"cookies\" placeholder=\"" +
            html_escape(t.placeholder) + "\"></textarea></details><button type=\"submit\" id=\"send\">" +
            html_escape(t.send) + "</button></form><p class=\"note\">" + kLockIcon + "<span>" +
            html_escape(t.privacy) + "</span></p></div></main><script>" + kScript + "</script></body></html>";
    return html;
}

}  // namespace newpipe
