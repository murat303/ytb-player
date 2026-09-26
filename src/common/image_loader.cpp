#include "newpipe/image_loader.hpp"

#include <chrono>

#include "newpipe/http_client.hpp"
#include "newpipe/log.hpp"

namespace newpipe {

namespace {

// mqdefault thumbnails are 10-20 KB, so this is about 5 MB while still covering several
// screens of scrolling in both directions.
constexpr size_t kMaxCachedImages = 300;

// nanovg/stb_image can't decode WebP. YouTube serves WebP not only for vi_webp/*.webp but
// also for vi/<id>/hq720.jpg?sqp=... (the .jpg name is misleading), and vi_lc/<id>/*_ko.jpg
// has no plain-JPEG form at all. Every ytimg video thumbnail (vi, vi_webp, vi_lc, an_webp...)
// is therefore mapped to the query-less vi/<id>/mqdefault.jpg: always a real JPEG, always
// 16:9, and 320x180 keeps texture memory low on long feeds.
std::string rewrite_ytimg_thumbnail_url(const std::string& url) {
    const std::string host = "ytimg.com/";
    const auto host_pos = url.find(host);
    if (host_pos == std::string::npos) {
        return {};
    }
    const auto kind_start = host_pos + host.size();
    const auto kind_end = url.find('/', kind_start);
    if (kind_end == std::string::npos) {
        return {};
    }
    const std::string kind = url.substr(kind_start, kind_end - kind_start);
    if (kind.rfind("vi", 0) != 0 && kind != "an_webp") {
        return {};
    }
    const auto id_end = url.find('/', kind_end + 1);
    if (id_end == std::string::npos || id_end == kind_end + 1) {
        return {};
    }
    return "https://i.ytimg.com/vi/" + url.substr(kind_end + 1, id_end - kind_end - 1) + "/mqdefault.jpg";
}

std::string rewrite_unsupported_image_url(const std::string& url) {
    const std::string thumbnail = rewrite_ytimg_thumbnail_url(url);
    if (!thumbnail.empty()) {
        return thumbnail;
    }
    std::string rewritten = url;
    const std::string webp_path = "/vi_webp/";
    const std::string jpg_path = "/vi/";
    auto pos = rewritten.find(webp_path);
    if (pos != std::string::npos) {
        rewritten.replace(pos, webp_path.size(), jpg_path);
    }
    const std::string webp_ext = ".webp";
    if (rewritten.size() >= webp_ext.size()) {
        const auto ext_pos = rewritten.rfind(webp_ext);
        if (ext_pos != std::string::npos) {
            const auto query_pos = rewritten.find('?', ext_pos);
            if (query_pos == std::string::npos
                && ext_pos + webp_ext.size() == rewritten.size()) {
                rewritten.replace(ext_pos, webp_ext.size(), ".jpg");
            }
        }
    }
    return rewritten;
}

}  // namespace

ImageLoader& ImageLoader::instance() {
    static ImageLoader loader;
    return loader;
}

ImageLoader::~ImageLoader() {
    stop();
}

void ImageLoader::start() {
    if (running_) {
        return;
    }

    running_ = true;
    log_line("image: start worker");
    thread_ = std::thread([this]() { worker(); });
}

void ImageLoader::stop() {
    running_ = false;
    if (thread_.joinable()) {
        thread_.join();
    }
    log_line("image: stop worker");
}

void ImageLoader::load(const std::string& url, brls::Image* target) {
    if (!target) {
        return;
    }

    const std::string fetch_url = rewrite_unsupported_image_url(url);
    target->setImageAsync([fetch_url, this](std::function<void(const std::string&, size_t)> cb) {
        std::string cached;
        if (tryGetCached(fetch_url, &cached)) {
            cb(cached, cached.size());
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push({fetch_url, cb});
    });
}

bool ImageLoader::tryGetCached(const std::string& url, std::string* out) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = cache_.find(url);
    if (it == cache_.end()) {
        return false;
    }
    *out = it->second;
    return true;
}

void ImageLoader::putCache(const std::string& url, const std::string& data) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    if (cache_.emplace(url, data).second) {
        cache_order_.push_back(url);
    }
    // The loader lives for the whole app run (it survives the UI teardown around playback),
    // so without a cap every thumbnail ever seen stayed in memory.
    while (cache_order_.size() > kMaxCachedImages) {
        cache_.erase(cache_order_.front());
        cache_order_.pop_front();
    }
}

void ImageLoader::worker() {
    HttpsHttpClient client;
    log_line("image: worker entered");

    while (running_) {
        AsyncRequest request;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!queue_.empty()) {
                request = std::move(queue_.front());
                queue_.pop();
            }
        }

        if (request.url.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        std::string cached;
        if (tryGetCached(request.url, &cached)) {
            request.callback(cached, cached.size());
            continue;
        }

        auto data = client.get(request.url);
        if (data.has_value() && !data->empty()) {
            logf("image: fetched %s bytes=%zu", request.url.c_str(), data->size());
            putCache(request.url, *data);
            request.callback(*data, data->size());
        } else {
            logf("image: fetch failed %s", request.url.c_str());
            request.callback("", 0);
        }
    }

    log_line("image: worker exit");
}

}  // namespace newpipe
