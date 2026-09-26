#pragma once

#include <atomic>
#include <optional>
#include <string>
#include <vector>

namespace newpipe {

struct HttpHeader {
    std::string name;
    std::string value;
};

class HttpClient {
public:
    virtual ~HttpClient() = default;

    virtual std::optional<std::string> get(
        const std::string& url,
        const std::vector<HttpHeader>& headers = {}) = 0;

    virtual std::optional<std::string> post(
        const std::string& url,
        const std::string& body,
        const std::vector<HttpHeader>& headers = {}) = 0;
};

class HttpsHttpClient final : public HttpClient {
public:
    // A request in flight gives up soon (within about a second) once *flag becomes true.
    void set_abort_flag(const std::atomic<bool>* flag) { abort_flag_ = flag; }

    std::optional<std::string> get(
        const std::string& url,
        const std::vector<HttpHeader>& headers = {}) override;

    std::optional<std::string> post(
        const std::string& url,
        const std::string& body,
        const std::vector<HttpHeader>& headers = {}) override;

private:
    const std::atomic<bool>* abort_flag_ = nullptr;
};

}  // namespace newpipe
