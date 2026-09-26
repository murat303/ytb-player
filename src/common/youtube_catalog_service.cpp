#include "newpipe/youtube_catalog_service.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"
#include "newpipe/content_locale.hpp"
#include "newpipe/log.hpp"

namespace newpipe {
namespace {

using nlohmann::json;

constexpr const char* kSearchApiUrl = "https://www.youtube.com/youtubei/v1/search?prettyPrint=false";
constexpr const char* kBrowseApiUrl = "https://www.youtube.com/youtubei/v1/browse?prettyPrint=false";
constexpr const char* kPlayerApiUrl = "https://www.youtube.com/youtubei/v1/player?prettyPrint=false";
constexpr const char* kChannelFeedUrlPrefix = "https://www.youtube.com/feeds/videos.xml?channel_id=";
constexpr const char* kAndroidClientVersion = "20.10.38";
constexpr const char* kAndroidUserAgent =
    "com.google.android.youtube/20.10.38 (Linux; U; Android 11) gzip";
// The content language and region sent to YouTube (section titles, relative dates, the
// trending region) follow the app's language: see content_locale.hpp.

constexpr const char* kWebUserAgent =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/135.0.0.0 Safari/537.36";
constexpr const char* kWebClientVersion = "2.20250403.01.00";
// The web client of 2026-09-25, for the account's lists (see get_account_playlist); the
// other requests keep the version their parsers were written against.
constexpr const char* kCurrentWebClientVersion = "2.20260925.01.00";

struct FeedPreset {
    const char* id;
    const char* title;
    const char* query;  // Turkish
    const char* query_en;
    const char* query_ko;
    bool allow_short_videos = false;
    const char* search_params = "";
};

// Signed out, "recommended" is a search. Its query brought back almost only Shorts, which are
// filtered out, so the list came up empty; the filter asks for this week's videos of 4-20 min.
// "shorts" is a search with the Shorts filter of the web client (get_shorts_feed).
constexpr std::array<FeedPreset, 5> kFeedPresets = {{
    {"recommended", "Önerilenler", "bugünün popüler videoları", "popular videos today", "오늘의 인기 동영상", false,
     "EgYIAxABGAM="},
    {"shorts", "Shorts", "shorts", "shorts", "shorts", true},
    {"live", "Canlı", "canlı yayın", "live stream", "라이브 방송", true},
    {"music", "Müzik", "müzik klip", "music video", "뮤직비디오", false},
    {"gaming", "Oyun", "oyun fragmanı", "game trailer", "게임 트레일러", false},
}};

const char* preset_query(const FeedPreset& preset) {
    switch (content_language()) {
        case ContentLanguage::turkish:
            return preset.query;
        case ContentLanguage::korean:
            return preset.query_ko;
        default:
            return preset.query_en;
    }
}

std::string get_string(const json& node, const char* key) {
    if (!node.is_object() || !node.contains(key) || node.at(key).is_null()) {
        return {};
    }

    if (node.at(key).is_string()) {
        return node.at(key).get<std::string>();
    }

    return {};
}

std::string get_text(const json& node) {
    if (node.is_null()) {
        return {};
    }

    if (node.is_string()) {
        return node.get<std::string>();
    }

    if (node.is_object()) {
        if (node.contains("content") && node.at("content").is_string()) {
            return node.at("content").get<std::string>();
        }

        if (node.contains("simpleText") && node.at("simpleText").is_string()) {
            return node.at("simpleText").get<std::string>();
        }

        if (node.contains("runs") && node.at("runs").is_array()) {
            std::string text;
            for (const auto& run : node.at("runs")) {
                const std::string part = get_string(run, "text");
                if (!part.empty()) {
                    text += part;
                }
            }
            return text;
        }
    }

    return {};
}

std::string to_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool contains_case_insensitive(const std::string& haystack, const std::string& needle) {
    return to_lower(haystack).find(to_lower(needle)) != std::string::npos;
}

int parse_duration_seconds(const std::string& duration_text) {
    if (duration_text.empty()) {
        return -1;
    }

    int total = 0;
    int current = 0;
    int parts = 0;
    for (char ch : duration_text) {
        if (ch == ':') {
            total = total * 60 + current;
            current = 0;
            parts++;
            continue;
        }

        if (!std::isdigit(static_cast<unsigned char>(ch))) {
            return -1;
        }

        current = current * 10 + (ch - '0');
    }

    if (parts == 0 && current == 0) {
        return -1;
    }

    return total * 60 + current;
}

std::string get_thumbnail_url(const json& renderer) {
    if (!renderer.is_object()) {
        return {};
    }

    const auto thumbnail_it = renderer.find("thumbnail");
    if (thumbnail_it == renderer.end() || !thumbnail_it->is_object()) {
        return {};
    }

    const auto thumbs_it = thumbnail_it->find("thumbnails");
    if (thumbs_it == thumbnail_it->end() || !thumbs_it->is_array() || thumbs_it->empty()) {
        return {};
    }

    const auto& last = thumbs_it->back();
    return get_string(last, "url");
}

std::string get_channel_url(const json& text_node) {
    if (!text_node.is_object() || !text_node.contains("runs") || !text_node.at("runs").is_array()) {
        return {};
    }

    for (const auto& run : text_node.at("runs")) {
        if (!run.is_object() || !run.contains("navigationEndpoint")) {
            continue;
        }

        const auto& endpoint = run.at("navigationEndpoint");
        if (!endpoint.is_object()) {
            continue;
        }

        if (endpoint.contains("browseEndpoint") && endpoint.at("browseEndpoint").is_object()) {
            const auto& browse = endpoint.at("browseEndpoint");
            const std::string canonical = get_string(browse, "canonicalBaseUrl");
            if (!canonical.empty()) {
                return "https://www.youtube.com" + canonical;
            }

            const std::string browse_id = get_string(browse, "browseId");
            if (!browse_id.empty()) {
                return "https://www.youtube.com/channel/" + browse_id;
            }
        }

        if (endpoint.contains("commandMetadata") && endpoint.at("commandMetadata").is_object()) {
            const auto& metadata = endpoint.at("commandMetadata");
            if (metadata.contains("webCommandMetadata") && metadata.at("webCommandMetadata").is_object()) {
                const std::string url = get_string(metadata.at("webCommandMetadata"), "url");
                if (!url.empty() && url[0] == '/') {
                    return "https://www.youtube.com" + url;
                }
            }
        }
    }

    return {};
}

std::string get_channel_id(const json& text_node) {
    if (!text_node.is_object() || !text_node.contains("runs") || !text_node.at("runs").is_array()) {
        return {};
    }

    for (const auto& run : text_node.at("runs")) {
        if (!run.is_object() || !run.contains("navigationEndpoint")) {
            continue;
        }

        const auto& endpoint = run.at("navigationEndpoint");
        if (!endpoint.is_object() || !endpoint.contains("browseEndpoint")
            || !endpoint.at("browseEndpoint").is_object()) {
            continue;
        }

        const std::string browse_id = get_string(endpoint.at("browseEndpoint"), "browseId");
        if (!browse_id.empty()) {
            return browse_id;
        }
    }

    return {};
}

std::string get_thumbnail_url_from_node(const json& thumbnail_node) {
    if (!thumbnail_node.is_object()) {
        return {};
    }

    const auto thumbs_it = thumbnail_node.find("thumbnails");
    if (thumbs_it == thumbnail_node.end() || !thumbs_it->is_array() || thumbs_it->empty()) {
        return {};
    }

    const auto& last = thumbs_it->back();
    return get_string(last, "url");
}

std::string get_thumbnail_url_from_sources(const json& image_node) {
    if (!image_node.is_object()) {
        return {};
    }

    const auto sources_it = image_node.find("sources");
    if (sources_it == image_node.end() || !sources_it->is_array() || sources_it->empty()) {
        return {};
    }

    const auto& last = sources_it->back();
    return get_string(last, "url");
}

std::optional<std::string> find_query_value(const std::string& url, const std::string& key) {
    const std::string pattern = key + "=";
    size_t search_from = 0;
    while (true) {
        const size_t pos = url.find(pattern, search_from);
        if (pos == std::string::npos) {
            return std::nullopt;
        }

        if (pos == 0 || url[pos - 1] == '?' || url[pos - 1] == '&') {
            const size_t value_start = pos + pattern.size();
            const size_t value_end = url.find_first_of("&#", value_start);
            return url.substr(value_start, value_end == std::string::npos ? std::string::npos
                                                                          : value_end - value_start);
        }

        search_from = pos + 1;
    }
}

std::optional<std::string> extract_video_id_from_url(const std::string& url) {
    if (const auto value = find_query_value(url, "v")) {
        return value;
    }

    for (const char* marker : {"/watch/", "/shorts/", "/live/", "youtu.be/"}) {
        const size_t pos = url.find(marker);
        if (pos == std::string::npos) {
            continue;
        }

        const size_t value_start = pos + std::char_traits<char>::length(marker);
        const size_t value_end = url.find_first_of("/?#", value_start);
        if (value_start < url.size()) {
            return url.substr(
                value_start,
                value_end == std::string::npos ? std::string::npos : value_end - value_start);
        }
    }

    return std::nullopt;
}

std::optional<std::string> extract_playlist_id_from_url(const std::string& url) {
    if (const auto value = find_query_value(url, "list"); value.has_value() && !value->empty()) {
        return value;
    }

    return std::nullopt;
}

std::string extract_channel_id_from_url(const std::string& channel_url) {
    const std::string marker = "/channel/";
    const size_t pos = channel_url.find(marker);
    if (pos == std::string::npos) {
        return {};
    }

    const size_t value_start = pos + marker.size();
    const size_t value_end = channel_url.find_first_of("/?#", value_start);
    return channel_url.substr(
        value_start,
        value_end == std::string::npos ? std::string::npos : value_end - value_start);
}

std::string absolutize_youtube_url(const std::string& url) {
    if (url.empty()) {
        return {};
    }

    if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0) {
        return url;
    }

    if (url[0] == '/') {
        return "https://www.youtube.com" + url;
    }

    return url;
}

std::string get_command_url(const json& command_node) {
    if (!command_node.is_object()) {
        return {};
    }

    if (command_node.contains("commandMetadata") && command_node.at("commandMetadata").is_object()) {
        const auto& metadata = command_node.at("commandMetadata");
        if (metadata.contains("webCommandMetadata") && metadata.at("webCommandMetadata").is_object()) {
            return absolutize_youtube_url(get_string(metadata.at("webCommandMetadata"), "url"));
        }
    }

    if (command_node.contains("innertubeCommand") && command_node.at("innertubeCommand").is_object()) {
        return get_command_url(command_node.at("innertubeCommand"));
    }

    return {};
}

std::string format_duration_text_from_seconds(const std::string& raw_seconds) {
    if (raw_seconds.empty()) {
        return {};
    }

    int total = 0;
    try {
        total = std::stoi(raw_seconds);
    } catch (...) {
        return {};
    }

    const int hours = total / 3600;
    const int minutes = (total % 3600) / 60;
    const int seconds = total % 60;

    std::ostringstream stream;
    if (hours > 0) {
        stream << hours << ':';
        if (minutes < 10) {
            stream << '0';
        }
    }
    stream << minutes << ':';
    if (seconds < 10) {
        stream << '0';
    }
    stream << seconds;
    return stream.str();
}

// "575293" -> "575.293 görüntüleme" / "575,293 views" / "조회수 575,293회", the way YouTube's
// feeds write counts in the content language.
std::string format_view_count_text(const std::string& raw_views) {
    if (raw_views.empty() || raw_views.find_first_not_of("0123456789") != std::string::npos) {
        return raw_views;
    }
    const ContentLanguage language = content_language();
    std::string grouped;
    for (size_t i = 0; i < raw_views.size(); i++) {
        if (i > 0 && (raw_views.size() - i) % 3 == 0) {
            grouped += language == ContentLanguage::turkish ? '.' : ',';
        }
        grouped += raw_views[i];
    }
    switch (language) {
        case ContentLanguage::turkish:
            return grouped + " görüntüleme";
        case ContentLanguage::korean:
            return "조회수 " + grouped + "회";
        default:
            return grouped + (raw_views == "1" ? " view" : " views");
    }
}

// Days since 1970-01-01 for a civil date (H. Hinnant's days_from_civil): UTC without timegm.
long long days_from_civil(long long year, unsigned month, unsigned day) {
    year -= month <= 2 ? 1 : 0;
    const long long era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
    const unsigned day_of_year = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
    const unsigned day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146097 + static_cast<long long>(day_of_era) - 719468;
}

// ISO 8601 dates (channel RSS, player microformat: "2026-09-23T13:59:46+00:00" or
// "2026-09-23") written like YouTube's feeds: "2 gün önce", "2 days ago", "2일 전". Anything
// else stays as it is.
std::string relative_time_text(const std::string& iso) {
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    const int fields = std::sscanf(
        iso.c_str(), "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute, &second);
    if ((fields != 3 && fields != 6) || month < 1 || month > 12 || day < 1 || day > 31) {
        return iso;
    }
    long long seconds = days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * 86400LL
        + hour * 3600LL + minute * 60LL + second;
    const size_t time_start = iso.find('T');
    const size_t zone = time_start == std::string::npos ? std::string::npos : iso.find_first_of("+-", time_start);
    int zone_hours = 0, zone_minutes = 0;
    if (zone != std::string::npos && std::sscanf(iso.c_str() + zone + 1, "%d:%d", &zone_hours, &zone_minutes) >= 1) {
        const long long offset = zone_hours * 3600LL + zone_minutes * 60LL;
        seconds += iso[zone] == '+' ? -offset : offset;
    }

    const long long ago = std::max(0LL, static_cast<long long>(std::time(nullptr)) - seconds);
    struct Unit {
        long long seconds;
        const char* turkish;
        const char* english;
        const char* korean;
    };
    static const Unit units[] = {
        {31536000, "yıl", "year", "년"}, {2592000, "ay", "month", "개월"}, {604800, "hafta", "week", "주"},
        {86400, "gün", "day", "일"}, {3600, "saat", "hour", "시간"}, {60, "dakika", "minute", "분"},
    };
    const ContentLanguage language = content_language();
    for (const Unit& unit : units) {
        if (ago < unit.seconds) {
            continue;
        }
        const long long count = ago / unit.seconds;
        const std::string number = std::to_string(count);
        switch (language) {
            case ContentLanguage::turkish:
                return number + " " + unit.turkish + " önce";
            case ContentLanguage::korean:
                return number + unit.korean + " 전";
            default:
                return number + " " + unit.english + (count == 1 ? "" : "s") + " ago";
        }
    }
    return localized("az önce", content_language() == ContentLanguage::korean ? "방금" : "just now");
}

std::string decode_xml_entities(std::string text) {
    const std::array<std::pair<const char*, const char*>, 5> replacements = {{
        {"&amp;", "&"},
        {"&quot;", "\""},
        {"&apos;", "'"},
        {"&lt;", "<"},
        {"&gt;", ">"},
    }};

    for (const auto& [from, to] : replacements) {
        size_t pos = 0;
        while ((pos = text.find(from, pos)) != std::string::npos) {
            text.replace(pos, std::char_traits<char>::length(from), to);
            pos += std::char_traits<char>::length(to);
        }
    }
    return text;
}

std::string extract_xml_tag(const std::string& xml, const std::string& tag) {
    const std::string open_tag = "<" + tag + ">";
    const std::string close_tag = "</" + tag + ">";
    const size_t start = xml.find(open_tag);
    if (start == std::string::npos) {
        return {};
    }
    const size_t value_start = start + open_tag.size();
    const size_t end = xml.find(close_tag, value_start);
    if (end == std::string::npos) {
        return {};
    }
    return decode_xml_entities(xml.substr(value_start, end - value_start));
}

std::string extract_xml_attribute(
    const std::string& xml,
    const std::string& tag_start,
    const std::string& attribute) {
    const size_t start = xml.find(tag_start);
    if (start == std::string::npos) {
        return {};
    }

    const std::string pattern = attribute + "=\"";
    const size_t attr_start = xml.find(pattern, start);
    if (attr_start == std::string::npos) {
        return {};
    }
    const size_t value_start = attr_start + pattern.size();
    const size_t value_end = xml.find('"', value_start);
    if (value_end == std::string::npos) {
        return {};
    }
    return decode_xml_entities(xml.substr(value_start, value_end - value_start));
}

bool is_live_renderer(const json& renderer) {
    const auto check_badge_list = [](const json& badge_list) {
        if (!badge_list.is_array()) {
            return false;
        }

        for (const auto& badge : badge_list) {
            if (!badge.is_object()) {
                continue;
            }

            std::string label;
            if (badge.contains("metadataBadgeRenderer")) {
                label = get_string(badge.at("metadataBadgeRenderer"), "label");
            } else if (badge.contains("thumbnailOverlayTimeStatusRenderer")) {
                label = get_text(badge.at("thumbnailOverlayTimeStatusRenderer").value("text", json::object()));
                if (label.empty()) {
                    label = get_string(badge.at("thumbnailOverlayTimeStatusRenderer"), "style");
                }
            } else if (badge.contains("badgeRenderer")) {
                label = get_string(badge.at("badgeRenderer"), "label");
            } else if (badge.contains("thumbnailOverlayNowPlayingRenderer")) {
                label = "LIVE";
            }

            if (contains_case_insensitive(label, "live")) {
                return true;
            }
        }

        return false;
    };

    return check_badge_list(renderer.value("badges", json::array()))
        || check_badge_list(renderer.value("thumbnailOverlays", json::array()));
}

std::string get_lockup_duration_text(const json& content_image) {
    const auto thumbnail_view = content_image.value("thumbnailViewModel", json::object());
    for (const auto& overlay : thumbnail_view.value("overlays", json::array())) {
        const auto overlay_model = overlay.value("thumbnailBottomOverlayViewModel", json::object());
        for (const auto& badge : overlay_model.value("badges", json::array())) {
            const std::string text = get_text(badge.value("thumbnailBadgeViewModel", json::object()).value(
                "text", json::object()));
            if (!text.empty()) {
                return text;
            }
        }
    }

    return {};
}

void parse_lockup_metadata_rows(
    const json& metadata_rows,
    std::string& channel_name,
    std::string& view_count_text,
    std::string& published_text) {
    if (!metadata_rows.is_array()) {
        return;
    }

    std::vector<std::vector<std::string>> rows;
    for (const auto& row : metadata_rows) {
        std::vector<std::string> parts;
        for (const auto& part : row.value("metadataParts", json::array())) {
            const std::string text = get_text(part.value("text", json::object()));
            if (!text.empty()) {
                parts.push_back(text);
            }
        }
        rows.push_back(std::move(parts));
    }

    // A channel's own page leaves the channel out: its only row is "views • date".
    if (rows.size() == 1 && rows[0].size() >= 2) {
        view_count_text = rows[0].front();
        published_text = rows[0].back();
        return;
    }
    if (!rows.empty() && !rows[0].empty()) {
        channel_name = rows[0].front();
    }
    if (rows.size() >= 2 && !rows[1].empty()) {
        view_count_text = rows[1].front();
        if (rows[1].size() >= 2) {
            published_text = rows[1].back();
        }
    }
}

std::optional<StreamItem> parse_lockup_stream_item(const json& lockup, bool allow_short_videos) {
    if (!lockup.is_object()) {
        return std::nullopt;
    }

    const std::string content_id = get_string(lockup, "contentId");
    if (content_id.empty()) {
        return std::nullopt;
    }

    const std::string content_type = get_string(lockup, "contentType");
    if (!content_type.empty() && !contains_case_insensitive(content_type, "video")) {
        return std::nullopt;
    }

    StreamItem item;
    item.id = content_id;

    const auto command = lockup.value("rendererContext", json::object())
                             .value("commandContext", json::object())
                             .value("onTap", json::object())
                             .value("innertubeCommand", json::object());
    item.url = get_command_url(command);
    if (item.url.empty()) {
        item.url = "https://www.youtube.com/watch?v=" + item.id;
    }
    if (!allow_short_videos && item.url.find("/shorts/") != std::string::npos) {
        return std::nullopt;
    }

    const auto metadata = lockup.value("metadata", json::object()).value("lockupMetadataViewModel", json::object());
    item.title = get_text(metadata.value("title", json::object()));

    parse_lockup_metadata_rows(
        metadata.value("metadata", json::object())
            .value("contentMetadataViewModel", json::object())
            .value("metadataRows", json::array()),
        item.channel_name,
        item.view_count_text,
        item.published_text);

    const auto avatar_command = metadata.value("image", json::object())
                                    .value("decoratedAvatarViewModel", json::object())
                                    .value("rendererContext", json::object())
                                    .value("commandContext", json::object())
                                    .value("onTap", json::object())
                                    .value("innertubeCommand", json::object());
    item.channel_url = get_command_url(avatar_command);
    item.channel_id =
        get_string(avatar_command.value("browseEndpoint", json::object()), "browseId");
    const json avatar_sources = metadata.value("image", json::object())
                                    .value("decoratedAvatarViewModel", json::object())
                                    .value("avatar", json::object())
                                    .value("avatarViewModel", json::object())
                                    .value("image", json::object())
                                    .value("sources", json::array());
    if (avatar_sources.is_array() && !avatar_sources.empty()) {
        item.channel_avatar_url = get_string(avatar_sources.back(), "url");
    }

    item.thumbnail_url = get_thumbnail_url_from_sources(
        lockup.value("contentImage", json::object())
            .value("thumbnailViewModel", json::object())
            .value("image", json::object()));
    item.duration_text = get_lockup_duration_text(lockup.value("contentImage", json::object()));
    item.is_live = contains_case_insensitive(item.duration_text, "live")
        || contains_case_insensitive(item.duration_text, "재생 중")
        || contains_case_insensitive(item.duration_text, "canli")
        || contains_case_insensitive(item.duration_text, "canlı");
    if (item.is_live) {
        item.duration_text.clear();
    }

    if (item.title.empty()) {
        return std::nullopt;
    }

    const int duration_seconds = parse_duration_seconds(item.duration_text);
    if (!allow_short_videos && !item.is_live && duration_seconds > 0 && duration_seconds < 120) {
        return std::nullopt;
    }

    return item;
}

// The channel's round picture beside a video in the older renderers (videoRenderer,
// compactVideoRenderer, the ANDROID client's videoWithContextRenderer).
std::string get_channel_avatar_url(const json& renderer) {
    const json candidates[] = {
        renderer.value("channelThumbnailSupportedRenderers", json::object())
            .value("channelThumbnailWithLinkRenderer", json::object())
            .value("thumbnail", json::object()),
        renderer.value("channelThumbnail", json::object())
            .value("channelThumbnailWithLinkRenderer", json::object())
            .value("thumbnail", json::object()),
        renderer.value("channelThumbnail", json::object()),
    };
    for (const json& node : candidates) {
        const json thumbnails = node.value("thumbnails", json::array());
        if (thumbnails.is_array() && !thumbnails.empty()) {
            const std::string url = get_string(thumbnails.back(), "url");
            if (!url.empty()) {
                return url.rfind("//", 0) == 0 ? "https:" + url : url;
            }
        }
    }
    return {};
}

std::optional<StreamItem> parse_stream_item(const json& renderer, bool allow_short_videos) {
    if (!renderer.is_object()) {
        return std::nullopt;
    }

    StreamItem item;
    item.id = get_string(renderer, "videoId");
    if (item.id.empty()) {
        return std::nullopt;
    }

    item.url = "https://www.youtube.com/watch?v=" + item.id;
    item.title = get_text(renderer.value("title", json::object()));
    item.channel_name = get_text(renderer.value("longBylineText", json::object()));
    if (item.channel_name.empty()) {
        item.channel_name = get_text(renderer.value("shortBylineText", json::object()));
    }
    item.channel_id = get_channel_id(renderer.value("longBylineText", json::object()));
    if (item.channel_id.empty()) {
        item.channel_id = get_channel_id(renderer.value("shortBylineText", json::object()));
    }
    item.channel_url = get_channel_url(renderer.value("longBylineText", json::object()));
    if (item.channel_url.empty()) {
        item.channel_url = get_channel_url(renderer.value("shortBylineText", json::object()));
    }
    item.thumbnail_url = get_thumbnail_url(renderer);
    item.channel_avatar_url = get_channel_avatar_url(renderer);
    item.duration_text = get_text(renderer.value("lengthText", json::object()));
    item.view_count_text = get_text(renderer.value("viewCountText", json::object()));
    item.published_text = get_text(renderer.value("publishedTimeText", json::object()));
    item.is_live = is_live_renderer(renderer);

    if (item.title.empty()) {
        return std::nullopt;
    }

    const int duration_seconds = parse_duration_seconds(item.duration_text);
    if (!allow_short_videos && !item.is_live && duration_seconds > 0 && duration_seconds < 120) {
        return std::nullopt;
    }

    return item;
}

std::optional<StreamItem> parse_playlist_stream_item(const json& renderer, bool allow_short_videos) {
    if (!renderer.is_object()) {
        return std::nullopt;
    }

    StreamItem item;
    item.id = get_string(renderer, "videoId");
    if (item.id.empty() || !renderer.value("isPlayable", true)) {
        return std::nullopt;
    }

    item.url = get_command_url(renderer.value("navigationEndpoint", json::object()));
    if (item.url.empty()) {
        item.url = "https://www.youtube.com/watch?v=" + item.id;
    }
    item.title = get_text(renderer.value("title", json::object()));
    item.channel_name = get_text(renderer.value("shortBylineText", json::object()));
    item.channel_id = get_channel_id(renderer.value("shortBylineText", json::object()));
    item.channel_url = get_channel_url(renderer.value("shortBylineText", json::object()));
    item.thumbnail_url = get_thumbnail_url_from_node(renderer.value("thumbnail", json::object()));
    item.duration_text = get_text(renderer.value("lengthText", json::object()));
    if (item.duration_text.empty()) {
        item.duration_text = format_duration_text_from_seconds(get_string(renderer, "lengthSeconds"));
    }
    item.is_live = item.duration_text.empty();

    if (item.title.empty()) {
        return std::nullopt;
    }

    const int duration_seconds = parse_duration_seconds(item.duration_text);
    if (!allow_short_videos && !item.is_live && duration_seconds > 0 && duration_seconds < 120) {
        return std::nullopt;
    }

    return item;
}

// The last (largest) url of an image's "sources" list, or the first at least `width` wide.
std::string pick_image_source(const json& image, int width = 0) {
    const json sources = image.value("sources", json::array());
    if (!sources.is_array() || sources.empty()) {
        return {};
    }
    if (width > 0) {
        for (const auto& source : sources) {
            if (source.value("width", 0) >= width) {
                return get_string(source, "url");
            }
        }
    }
    return get_string(sources.back(), "url");
}

// A Short on a channel's Shorts tab (shortsLockupViewModel): the video id, the title and the
// view count from the overlay, the upright thumbnail.
std::optional<StreamItem> parse_shorts_lockup_item(const json& lockup) {
    if (!lockup.is_object()) {
        return std::nullopt;
    }
    const json endpoint = lockup.value("onTap", json::object())
                              .value("innertubeCommand", json::object())
                              .value("reelWatchEndpoint", json::object());
    StreamItem item;
    item.id = get_string(endpoint, "videoId");
    if (item.id.empty()) {
        return std::nullopt;
    }
    item.url = "https://www.youtube.com/shorts/" + item.id;
    item.reel_sequence = get_string(endpoint, "sequenceParams");
    const json overlay = lockup.value("overlayMetadata", json::object());
    item.title = get_text(overlay.value("primaryText", json::object()));
    item.view_count_text = get_text(overlay.value("secondaryText", json::object()));
    if (item.title.empty()) {
        // "leaking the newest lttstore products..., 167 bin görüntüleme - kısa videoyu oynat"
        const std::string text = get_string(lockup, "accessibilityText");
        item.title = text.substr(0, text.rfind(", "));
    }
    // Sources: 405x720 (upright, 9:16) and a 405x608 crop; the card is drawn at 9:16.
    const json image = lockup.value("thumbnailViewModel", json::object())
                           .value("thumbnailViewModel", json::object())
                           .value("image", json::object());
    item.thumbnail_url = pick_image_source(image, 400);
    if (item.thumbnail_url.empty()) {
        item.thumbnail_url = "https://i.ytimg.com/vi/" + item.id + "/hqdefault.jpg";
    }
    item.duration_text = "Shorts";
    return item;
}

// A playlist on a channel's Playlists tab (a lockup of the playlist type): its id, title,
// cover and video count ("25 video" on the cover's badge).
std::optional<StreamItem> parse_lockup_playlist_item(const json& lockup) {
    if (!lockup.is_object() || !contains_case_insensitive(get_string(lockup, "contentType"), "playlist")) {
        return std::nullopt;
    }
    StreamItem item;
    item.id = get_string(lockup, "contentId");
    if (item.id.empty()) {
        return std::nullopt;
    }
    item.is_playlist = true;
    item.url = "https://www.youtube.com/playlist?list=" + item.id;
    item.title = get_text(lockup.value("metadata", json::object())
                              .value("lockupMetadataViewModel", json::object())
                              .value("title", json::object()));
    const json thumbnail = lockup.value("contentImage", json::object())
                               .value("collectionThumbnailViewModel", json::object())
                               .value("primaryThumbnail", json::object())
                               .value("thumbnailViewModel", json::object());
    item.thumbnail_url = get_thumbnail_url_from_sources(thumbnail.value("image", json::object()));
    for (const auto& overlay : thumbnail.value("overlays", json::array())) {
        for (const auto& badge : overlay.value("thumbnailOverlayBadgeViewModel", json::object())
                                     .value("thumbnailBadges", json::array())) {
            const std::string text =
                get_text(badge.value("thumbnailBadgeViewModel", json::object()).value("text", json::object()));
            if (!text.empty()) {
                item.duration_text = text;
            }
        }
    }
    return item.title.empty() ? std::nullopt : std::optional<StreamItem>(item);
}

// The items of a channel tab: videos (lockups or the older renderers), Shorts and playlists.
void collect_channel_tab_items(const json& node, std::unordered_set<std::string>& seen_ids,
                               std::vector<StreamItem>& out_items) {
    if (out_items.size() >= 120) {
        return;
    }
    if (node.is_array()) {
        for (const auto& child : node) {
            collect_channel_tab_items(child, seen_ids, out_items);
        }
        return;
    }
    if (!node.is_object()) {
        return;
    }
    std::optional<StreamItem> item;
    if (node.contains("shortsLockupViewModel")) {
        item = parse_shorts_lockup_item(node.at("shortsLockupViewModel"));
    } else if (node.contains("lockupViewModel")) {
        const json& lockup = node.at("lockupViewModel");
        item = parse_lockup_playlist_item(lockup);
        if (!item.has_value()) {
            item = parse_lockup_stream_item(lockup, true);
        }
    } else {
        for (const char* key : {"videoRenderer", "gridVideoRenderer"}) {
            if (node.contains(key)) {
                item = parse_stream_item(node.at(key), true);
            }
        }
    }
    if (item.has_value()) {
        if (seen_ids.insert(item->id).second) {
            out_items.push_back(*item);
        }
        return;
    }
    for (const auto& entry : node.items()) {
        collect_channel_tab_items(entry.value(), seen_ids, out_items);
    }
}

// The top of a playlist's page (pageHeaderViewModel): the name, "... tarafından" beside the
// owner's picture, "Oynatma listesi • 25 video • 1,2 B görüntüleme", the description, the cover.
PlaylistInfo parse_playlist_header(const json& root, const std::string& playlist_id) {
    PlaylistInfo info;
    info.id = playlist_id;
    const json header = root.value("header", json::object())
                            .value("pageHeaderRenderer", json::object())
                            .value("content", json::object())
                            .value("pageHeaderViewModel", json::object());
    info.title = get_text(header.value("title", json::object())
                              .value("dynamicTextViewModel", json::object())
                              .value("text", json::object()));
    info.description = get_text(header.value("description", json::object())
                                    .value("descriptionPreviewViewModel", json::object())
                                    .value("description", json::object()));
    info.thumbnail_url = pick_image_source(header.value("heroImage", json::object())
                                               .value("contentPreviewImageViewModel", json::object())
                                               .value("image", json::object()));
    const json rows = header.value("metadata", json::object())
                          .value("contentMetadataViewModel", json::object())
                          .value("metadataRows", json::array());
    for (const auto& row : rows) {
        for (const auto& part : row.value("metadataParts", json::array())) {
            const std::string owner = get_text(part.value("avatarStack", json::object())
                                                   .value("avatarStackViewModel", json::object())
                                                   .value("text", json::object()));
            if (!owner.empty()) {
                info.owner = owner;
                continue;
            }
            const std::string text = get_text(part.value("text", json::object()));
            // "Oynatma listesi" names the kind of page; the counts follow it.
            if (text.empty() || contains_case_insensitive(text, "oynatma listesi")
                || contains_case_insensitive(text, "playlist")) {
                continue;
            }
            info.meta += (info.meta.empty() ? "" : " • ") + text;
        }
    }
    return info;
}

// The top of a channel's page (pageHeaderViewModel) and its tabs, from any of its browse answers.
ChannelInfo parse_channel_header(const json& root) {
    ChannelInfo info;
    const json header = root.value("header", json::object())
                            .value("pageHeaderRenderer", json::object())
                            .value("content", json::object())
                            .value("pageHeaderViewModel", json::object());
    info.name = get_text(header.value("title", json::object())
                             .value("dynamicTextViewModel", json::object())
                             .value("text", json::object()));
    // The picture is drawn 88 px wide: the first source at least that big.
    info.avatar_url = pick_image_source(header.value("image", json::object())
                                            .value("decoratedAvatarViewModel", json::object())
                                            .value("avatar", json::object())
                                            .value("avatarViewModel", json::object())
                                            .value("image", json::object()),
                                        100);
    // Wide enough for the screen, not the 2560 px one.
    info.banner_url = pick_image_source(
        header.value("banner", json::object()).value("imageBannerViewModel", json::object()).value("image", json::object()),
        1200);
    info.description = get_text(header.value("description", json::object())
                                    .value("descriptionPreviewViewModel", json::object())
                                    .value("description", json::object()));
    // Row one: "@handle"; row two: "16,9 Mn abone", "7,9 B video".
    const json rows = header.value("metadata", json::object())
                          .value("contentMetadataViewModel", json::object())
                          .value("metadataRows", json::array());
    std::vector<std::string> parts;
    for (const auto& row : rows) {
        for (const auto& part : row.value("metadataParts", json::array())) {
            const std::string text = get_text(part.value("text", json::object()));
            if (!text.empty()) {
                parts.push_back(text);
            }
        }
    }
    for (const std::string& part : parts) {
        if (!part.empty() && part[0] == '@' && info.handle.empty()) {
            info.handle = part;
        } else if (contains_case_insensitive(part, "abone") || contains_case_insensitive(part, "subscriber")) {
            info.subscribers = part;
        } else if (contains_case_insensitive(part, "video")) {
            info.video_count = part;
        }
    }
    // The tabs this app can show, with the params YouTube gives them (decoded).
    const json tabs = root.value("contents", json::object())
                          .value("twoColumnBrowseResultsRenderer", json::object())
                          .value("tabs", json::array());
    for (const auto& tab : tabs) {
        const json renderer = tab.value("tabRenderer", json::object());
        std::string params =
            get_string(renderer.value("endpoint", json::object()).value("browseEndpoint", json::object()), "params");
        for (const auto& [encoded, plain] : {std::pair{"%3D", "="}, std::pair{"%2F", "/"}, std::pair{"%2B", "+"}}) {
            for (size_t at = params.find(encoded); at != std::string::npos; at = params.find(encoded, at)) {
                params.replace(at, 3, plain);
            }
        }
        // "videos", "shorts", "streams", "playlists" in base64, cut before the character that
        // also holds the next byte.
        const bool supported = params.rfind("EgZ2aWRlb3", 0) == 0 || params.rfind("EgZzaG9ydH", 0) == 0
            || params.rfind("EgdzdHJlYW1z", 0) == 0 || params.rfind("EglwbGF5bGlzdH", 0) == 0;
        const std::string title = get_string(renderer, "title");
        if (supported && !title.empty()) {
            info.tabs.push_back({title, params});
        }
    }
    return info;
}

void collect_stream_items(
    const json& node,
    bool allow_short_videos,
    size_t limit,
    std::unordered_set<std::string>& seen_ids,
    std::vector<StreamItem>& out_items) {
    if (out_items.size() >= limit) {
        return;
    }

    if (node.is_object()) {
        if (const auto item = parse_lockup_stream_item(node, allow_short_videos);
            item.has_value() && seen_ids.insert(item->id).second) {
            out_items.push_back(*item);
            if (out_items.size() >= limit) {
                return;
            }
        }

        for (const char* key : {"compactVideoRenderer", "videoRenderer", "gridVideoRenderer"}) {
            if (node.contains(key)) {
                const auto item = parse_stream_item(node.at(key), allow_short_videos);
                if (item.has_value() && seen_ids.insert(item->id).second) {
                    out_items.push_back(*item);
                    if (out_items.size() >= limit) {
                        return;
                    }
                }
            }
        }

        // A Shorts shelf (subscriptions, the home feed): Shorts with their place in YouTube's
        // Shorts player, so that they open one after another.
        if (allow_short_videos && node.contains("shortsLockupViewModel")) {
            const auto item = parse_shorts_lockup_item(node.at("shortsLockupViewModel"));
            if (item.has_value() && seen_ids.insert(item->id).second) {
                out_items.push_back(*item);
            }
            return;
        }

        for (const auto& entry : node.items()) {
            collect_stream_items(entry.value(), allow_short_videos, limit, seen_ids, out_items);
            if (out_items.size() >= limit) {
                return;
            }
        }
        return;
    }

    if (node.is_array()) {
        for (const auto& child : node) {
            collect_stream_items(child, allow_short_videos, limit, seen_ids, out_items);
            if (out_items.size() >= limit) {
                return;
            }
        }
    }
}

// Next-page token of the main list. WEB browse pages end their item list with a
// continuationItemRenderer; the ANDROID search API keeps a nextContinuationData beside the
// section list instead. Shelves inside a feed may carry their own tokens, so the token that
// belongs to the longest list wins.
void find_next_page_token(const json& node, std::string& token, size_t& list_size) {
    if (node.is_array()) {
        if (!node.empty() && node.back().is_object() && node.back().contains("continuationItemRenderer")) {
            const std::string candidate = get_string(
                node.back()
                    .at("continuationItemRenderer")
                    .value("continuationEndpoint", json::object())
                    .value("continuationCommand", json::object()),
                "token");
            if (!candidate.empty() && node.size() > list_size) {
                token = candidate;
                list_size = node.size();
            }
        }
        for (const auto& child : node) {
            find_next_page_token(child, token, list_size);
        }
        return;
    }
    if (!node.is_object()) {
        return;
    }

    const auto contents = node.find("contents");
    const auto continuations = node.find("continuations");
    if (contents != node.end() && contents->is_array()
        && continuations != node.end() && continuations->is_array()) {
        for (const auto& entry : *continuations) {
            const std::string candidate =
                get_string(entry.value("nextContinuationData", json::object()), "continuation");
            if (!candidate.empty() && contents->size() + 1 > list_size) {
                token = candidate;
                list_size = contents->size() + 1;
            }
        }
    }
    for (const auto& entry : node.items()) {
        find_next_page_token(entry.value(), token, list_size);
    }
}

std::string find_next_page_token(const json& root) {
    std::string token;
    size_t list_size = 0;
    find_next_page_token(root, token, list_size);
    return token;
}

json android_search_client_context() {
    return {{"client",
             {{"clientName", "ANDROID"},
              {"clientVersion", kAndroidClientVersion},
              {"androidSdkVersion", 30},
              {"hl", content_hl()},
              {"gl", content_gl()}}}};
}

std::vector<HttpHeader> android_search_headers() {
    return {
        {"Content-Type", "application/json"},
        {"User-Agent", kAndroidUserAgent},
        {"X-Youtube-Client-Name", "3"},
        {"X-Youtube-Client-Version", kAndroidClientVersion},
        {"Origin", "https://www.youtube.com"},
    };
}

json web_browse_client_context() {
    return {{"client",
             {{"clientName", "WEB"},
              {"clientVersion", kWebClientVersion},
              {"hl", content_hl()},
              {"gl", content_gl()},
              {"utcOffsetMinutes", utc_offset_minutes()},
              {"platform", "DESKTOP"},
              {"screenWidthPoints", 1280},
              {"screenHeightPoints", 720},
              {"screenPixelDensity", 1}}}};
}

void collect_playlist_items(
    const json& node,
    bool allow_short_videos,
    size_t limit,
    std::unordered_set<std::string>& seen_ids,
    std::vector<StreamItem>& out_items) {
    if (out_items.size() >= limit) {
        return;
    }

    if (node.is_object()) {
        if (node.contains("playlistVideoRenderer")) {
            const auto item = parse_playlist_stream_item(node.at("playlistVideoRenderer"), allow_short_videos);
            if (item.has_value() && seen_ids.insert(item->id).second) {
                out_items.push_back(*item);
                if (out_items.size() >= limit) {
                    return;
                }
            }
        }
        // Since 2026 YouTube lists a playlist's videos as lockups, like channel pages do,
        // for old client versions too.
        if (node.contains("lockupViewModel")) {
            const auto item = parse_lockup_stream_item(node.at("lockupViewModel"), allow_short_videos);
            if (item.has_value() && seen_ids.insert(item->id).second) {
                out_items.push_back(*item);
                if (out_items.size() >= limit) {
                    return;
                }
            }
        }

        for (const auto& entry : node.items()) {
            collect_playlist_items(entry.value(), allow_short_videos, limit, seen_ids, out_items);
            if (out_items.size() >= limit) {
                return;
            }
        }
        return;
    }

    if (node.is_array()) {
        for (const auto& child : node) {
            collect_playlist_items(child, allow_short_videos, limit, seen_ids, out_items);
            if (out_items.size() >= limit) {
                return;
            }
        }
    }
}

std::optional<std::string> extract_json_assignment(
    const std::string& text,
    const std::string& marker) {
    const size_t marker_pos = text.find(marker);
    if (marker_pos == std::string::npos) {
        return std::nullopt;
    }

    size_t json_start = std::string::npos;
    int depth = 0;
    bool in_string = false;
    bool escaped = false;

    for (size_t i = marker_pos + marker.size(); i < text.size(); i++) {
        const char ch = text[i];
        if (json_start == std::string::npos) {
            if (ch == '{' || ch == '[') {
                json_start = i;
                depth = 1;
            }
            continue;
        }

        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_string = false;
            }
            continue;
        }

        if (ch == '"') {
            in_string = true;
            continue;
        }

        if (ch == '{' || ch == '[') {
            depth++;
            continue;
        }

        if (ch == '}' || ch == ']') {
            depth--;
            if (depth == 0) {
                return text.substr(json_start, i - json_start + 1);
            }
        }
    }

    return std::nullopt;
}

std::optional<json> fetch_watch_page_initial_data(HttpClient* client, const std::string& watch_url) {
    const std::string watch_url_with_flags =
        watch_url + (watch_url.find('?') == std::string::npos ? "?" : "&")
        + "bpctr=9999999999&has_verified=1";
    const auto response = client->get(
        watch_url_with_flags,
        {
            {"User-Agent", kWebUserAgent},
            {"Accept-Language", accept_language()},
            {"Cookie", pref_cookie()},
        });
    if (!response.has_value() || response->empty()) {
        return std::nullopt;
    }

    std::optional<std::string> json_text;
    for (const char* marker : {"var ytInitialData = ", "window[\"ytInitialData\"] = ", "ytInitialData = "}) {
        json_text = extract_json_assignment(*response, marker);
        if (json_text.has_value()) {
            break;
        }
    }
    if (!json_text.has_value()) {
        return std::nullopt;
    }

    const json initial_data = json::parse(*json_text, nullptr, false);
    if (initial_data.is_discarded()) {
        return std::nullopt;
    }

    return initial_data;
}

std::string extract_comments_continuation_token(const json& initial_data) {
    const json contents = initial_data.value("contents", json::object())
                              .value("twoColumnWatchNextResults", json::object())
                              .value("results", json::object())
                              .value("results", json::object())
                              .value("contents", json::array());

    std::string token;
    const auto walk = [&](const auto& self, const json& node) -> void {
        if (!token.empty() || node.is_null()) {
            return;
        }

        if (node.is_object()) {
            if (node.contains("continuationItemRenderer")
                && node.at("continuationItemRenderer").is_object()) {
                token = get_string(
                    node.at("continuationItemRenderer")
                        .value("continuationEndpoint", json::object())
                        .value("continuationCommand", json::object()),
                    "token");
                if (!token.empty()) {
                    return;
                }
            }

            for (const auto& entry : node.items()) {
                self(self, entry.value());
                if (!token.empty()) {
                    return;
                }
            }
            return;
        }

        if (node.is_array()) {
            for (const auto& child : node) {
                self(self, child);
                if (!token.empty()) {
                    return;
                }
            }
        }
    };

    walk(walk, contents);
    return token;
}

// The comments panel of a watch page: "10 Mn" beside its title and the orders ("En popüler",
// "En yeni") with the token of each one's first page.
void read_comments_panel(const json& initial_data, CommentPage& page) {
    for (const auto& panel : initial_data.value("engagementPanels", json::array())) {
        const json section = panel.value("engagementPanelSectionListRenderer", json::object());
        if (get_string(section, "panelIdentifier") != "engagement-panel-comments-section") {
            continue;
        }
        const json header =
            section.value("header", json::object()).value("engagementPanelTitleHeaderRenderer", json::object());
        page.count_text = get_text(header.value("contextualInfo", json::object()));
        const json entries = header.value("menu", json::object())
                                 .value("sortFilterSubMenuRenderer", json::object())
                                 .value("subMenuItems", json::array());
        for (const auto& entry : entries) {
            CommentSort sort;
            sort.title = get_string(entry, "title");
            sort.token = get_string(
                entry.value("serviceEndpoint", json::object()).value("continuationCommand", json::object()), "token");
            sort.selected = entry.value("selected", false);
            if (!sort.title.empty() && !sort.token.empty()) {
                page.sorts.push_back(std::move(sort));
            }
        }
        return;
    }
}

// The related videos of a watch page. The same page names the channel's avatar and subscriber
// count, which the detail screen shows; they are handed back when `detail` is given.
std::vector<StreamItem> fetch_watch_related_items(
    HttpClient* client,
    const std::string& watch_url,
    const std::string& current_video_id,
    bool allow_short_videos,
    StreamDetail* detail = nullptr) {
    const auto initial_data = fetch_watch_page_initial_data(client, watch_url);
    if (!initial_data.has_value()) {
        return {};
    }

    if (detail) {
        const json contents = initial_data->value("contents", json::object())
                                  .value("twoColumnWatchNextResults", json::object())
                                  .value("results", json::object())
                                  .value("results", json::object())
                                  .value("contents", json::array());
        for (const auto& entry : contents) {
            if (!entry.is_object() || !entry.contains("videoSecondaryInfoRenderer")) {
                continue;
            }
            const json owner = entry.at("videoSecondaryInfoRenderer")
                                   .value("owner", json::object())
                                   .value("videoOwnerRenderer", json::object());
            detail->channel_subscriber_count_text = get_text(owner.value("subscriberCountText", json::object()));
            const json avatars = owner.value("thumbnail", json::object()).value("thumbnails", json::array());
            if (avatars.is_array() && !avatars.empty()) {
                // The largest (176 px) stays sharp docked, where the avatar is drawn at 60 px.
                detail->channel_avatar_url = get_string(avatars.back(), "url");
            }
        }
    }

    const json secondary_results = initial_data->value("contents", json::object())
                                       .value("twoColumnWatchNextResults", json::object())
                                       .value("secondaryResults", json::object())
                                       .value("secondaryResults", json::object())
                                       .value("results", json::array());

    std::vector<StreamItem> items;
    std::unordered_set<std::string> seen_ids;
    if (!current_video_id.empty()) {
        seen_ids.insert(current_video_id);
    }
    collect_stream_items(secondary_results, allow_short_videos, 16, seen_ids, items);
    return items;
}

// Chapters from a watch page (/next): the markers of YouTube's player bar. Description
// chapters (from "0:00 Intro" lines) win over the automatic ones.
std::vector<Chapter> parse_chapters(const json& root) {
    const json markers = root.value("playerOverlays", json::object())
                             .value("playerOverlayRenderer", json::object())
                             .value("decoratedPlayerBarRenderer", json::object())
                             .value("decoratedPlayerBarRenderer", json::object())
                             .value("playerBar", json::object())
                             .value("multiMarkersPlayerBarRenderer", json::object())
                             .value("markersMap", json::array());
    if (!markers.is_array()) {
        return {};
    }
    for (const char* kind : {"DESCRIPTION_CHAPTERS", "AUTO_CHAPTERS"}) {
        for (const auto& marker : markers) {
            if (get_string(marker, "key") != kind) {
                continue;
            }
            std::vector<Chapter> chapters;
            const json entries = marker.value("value", json::object()).value("chapters", json::array());
            for (const auto& entry : entries) {
                const json renderer = entry.value("chapterRenderer", json::object());
                if (!renderer.contains("timeRangeStartMillis") || !renderer.at("timeRangeStartMillis").is_number()) {
                    continue;
                }
                Chapter chapter;
                chapter.start = renderer.at("timeRangeStartMillis").get<double>() / 1000.0;
                chapter.title = get_text(renderer.value("title", json::object()));
                chapters.push_back(chapter);
            }
            if (!chapters.empty()) {
                std::sort(chapters.begin(), chapters.end(),
                          [](const Chapter& a, const Chapter& b) { return a.start < b.start; });
                return chapters;
            }
        }
    }
    return {};
}

// The page id a channel switcher entry acts with: its pageIdToken, else the channel half of a
// "channel||user" datasync id (the account's own channel has "user||" and no page id).
std::string find_account_page_id(const json& node) {
    if (node.is_object()) {
        if (node.contains("pageIdToken") && node.at("pageIdToken").is_object()) {
            const std::string page_id = get_string(node.at("pageIdToken"), "pageId");
            if (!page_id.empty()) {
                return page_id;
            }
        }
        if (node.contains("datasyncIdToken") && node.at("datasyncIdToken").is_object()) {
            const std::string sync_id = get_string(node.at("datasyncIdToken"), "datasyncIdToken");
            const size_t bar = sync_id.find("||");
            if (bar != std::string::npos && bar > 0 && bar + 2 < sync_id.size()) {
                return sync_id.substr(0, bar);
            }
        }
        for (const auto& entry : node.items()) {
            const std::string page_id = find_account_page_id(entry.value());
            if (!page_id.empty()) {
                return page_id;
            }
        }
    } else if (node.is_array()) {
        for (const auto& child : node) {
            const std::string page_id = find_account_page_id(child);
            if (!page_id.empty()) {
                return page_id;
            }
        }
    }
    return {};
}

// The entries of YouTube's account / channel switcher ("accountItem" in the account menu,
// "accountItemRenderer" on the channel switcher page).
void collect_accounts(const json& node, std::vector<YouTubeAccount>& out) {
    if (node.is_array()) {
        for (const auto& child : node) {
            collect_accounts(child, out);
        }
        return;
    }
    if (!node.is_object()) {
        return;
    }
    for (const char* key : {"accountItem", "accountItemRenderer"}) {
        if (!node.contains(key) || !node.at(key).is_object()) {
            continue;
        }
        const json& item = node.at(key);
        YouTubeAccount account;
        account.name = get_text(item.value("accountName", json::object()));
        account.handle = get_text(item.value("channelHandle", json::object()));
        if (account.handle.empty()) {
            account.handle = get_text(item.value("accountByline", json::object()));
        }
        account.selected = item.contains("isSelected") && item.at("isSelected").is_boolean()
            && item.at("isSelected").get<bool>();
        account.page_id = find_account_page_id(item.value("serviceEndpoint", json::object()));
        const json photos = item.value("accountPhoto", json::object()).value("thumbnails", json::array());
        if (photos.is_array() && !photos.empty()) {
            account.photo_url = get_string(photos.back(), "url");
        }
        if (!account.name.empty()) {
            out.push_back(account);
        }
    }
    for (const auto& entry : node.items()) {
        collect_accounts(entry.value(), out);
    }
}

// A comment page from YouTube holds about 20 threads; this only guards against a runaway one.
constexpr size_t kCommentPageLimit = 100;

std::optional<CommentPage> fetch_comments_page(
    HttpClient* client,
    const std::string& continuation_token,
    size_t limit) {
    if (continuation_token.empty()) {
        return std::nullopt;
    }

    json payload = {
        {"continuation", continuation_token},
        {"context",
         {{"client",
           {{"clientName", "WEB"},
            {"clientVersion", kWebClientVersion},
            {"hl", content_hl()},
            {"gl", content_gl()},
            {"platform", "DESKTOP"}}}}},
    };

    const auto response = client->post(
        "https://www.youtube.com/youtubei/v1/next?prettyPrint=false",
        payload.dump(),
        {
            {"Content-Type", "application/json"},
            {"User-Agent", kWebUserAgent},
            {"X-Youtube-Client-Name", "1"},
            {"X-Youtube-Client-Version", kWebClientVersion},
            {"Origin", "https://www.youtube.com"},
        });
    if (!response.has_value() || response->empty()) {
        return std::nullopt;
    }

    const json root = json::parse(*response, nullptr, false);
    if (root.is_discarded()) {
        return std::nullopt;
    }

    CommentPage page;
    // The comments in the page's order: threads (a video's comments, each with the token of
    // its replies and a note when pinned) or plain comment views (a page of replies). The
    // texts themselves are entities further down, found by the comment's id.
    struct Place {
        std::string id;
        std::string replies_token;
        std::string pinned_text;
    };
    std::vector<Place> order;
    // The first page has one reload command for the header (the title) and one for the
    // comment threads; later pages have an append action. The thread list ends with a
    // continuationItemRenderer that carries the token of the next page.
    const json endpoints = root.value("onResponseReceivedEndpoints", json::array());
    for (const auto& endpoint : endpoints) {
        if (!endpoint.is_object()) {
            continue;
        }
        for (const char* key : {"reloadContinuationItemsCommand", "appendContinuationItemsAction"}) {
            const json items = endpoint.value(key, json::object()).value("continuationItems", json::array());
            if (!items.is_array() || items.empty()) {
                continue;
            }
            if (page.title.empty() && items.front().is_object() && items.front().contains("commentsHeaderRenderer")) {
                page.title = get_text(
                    items.front().at("commentsHeaderRenderer").value("countText", json::object()));
            }
            for (const auto& entry : items) {
                if (!entry.is_object()) {
                    continue;
                }
                Place place;
                if (entry.contains("commentThreadRenderer")) {
                    const json thread = entry.at("commentThreadRenderer");
                    const json view =
                        thread.value("commentViewModel", json::object()).value("commentViewModel", json::object());
                    place.id = get_string(view, "commentId");
                    place.pinned_text = get_string(view, "pinnedText");
                    const json replies = thread.value("replies", json::object())
                                             .value("commentRepliesRenderer", json::object())
                                             .value("contents", json::array());
                    for (const auto& reply : replies) {
                        const std::string token = get_string(reply.value("continuationItemRenderer", json::object())
                                                                 .value("continuationEndpoint", json::object())
                                                                 .value("continuationCommand", json::object()),
                                                             "token");
                        if (!token.empty()) {
                            place.replies_token = token;
                        }
                    }
                } else if (entry.contains("commentViewModel")) {
                    place.id = get_string(entry.at("commentViewModel"), "commentId");
                }
                if (!place.id.empty()) {
                    order.push_back(std::move(place));
                }
            }
            const json& last = items.back();
            if (last.is_object() && last.contains("continuationItemRenderer")) {
                const json renderer = last.at("continuationItemRenderer");
                std::string token = get_string(
                    renderer.value("continuationEndpoint", json::object()).value("continuationCommand", json::object()),
                    "token");
                if (token.empty()) {
                    token = get_string(renderer.value("button", json::object())
                                           .value("buttonRenderer", json::object())
                                           .value("command", json::object())
                                           .value("continuationCommand", json::object()),
                                       "token");
                }
                if (!token.empty()) {
                    page.next_page_token = token;
                }
            }
        }
    }

    const json mutations = root.value("frameworkUpdates", json::object())
                               .value("entityBatchUpdate", json::object())
                               .value("mutations", json::array());
    std::vector<std::pair<std::string, CommentItem>> comments;
    for (const auto& mutation : mutations) {
        if (!mutation.is_object() || !mutation.contains("payload") || !mutation.at("payload").is_object()) {
            continue;
        }

        const json payload_node = mutation.at("payload");
        if (!payload_node.contains("commentEntityPayload")
            || !payload_node.at("commentEntityPayload").is_object()) {
            continue;
        }

        const json comment = payload_node.at("commentEntityPayload");
        const json author = comment.value("author", json::object());
        CommentItem item;
        item.body = get_text(comment.value("properties", json::object()).value("content", json::object()));
        item.published_text = get_string(comment.value("properties", json::object()), "publishedTime");
        item.author_name = get_string(author, "displayName");
        item.author_url = get_command_url(
            author.value("channelCommand", json::object()).value("innertubeCommand", json::object()));
        item.author_thumbnail_url = get_string(author, "avatarThumbnailUrl");
        item.like_count_text = get_string(comment.value("toolbar", json::object()), "likeCountNotliked");
        item.reply_count_text = get_string(comment.value("toolbar", json::object()), "replyCount");
        item.is_verified = author.value("isVerified", false);
        item.is_creator = author.value("isCreator", false);

        if (item.body.empty()) {
            continue;
        }
        comments.emplace_back(get_string(comment.value("properties", json::object()), "commentId"), std::move(item));
    }

    // In the page's order when it names its comments, else as the entities came.
    for (const Place& place : order) {
        for (auto& [id, item] : comments) {
            if (id == place.id && !item.body.empty()) {
                item.replies_token = place.replies_token;
                item.pinned_text = place.pinned_text;
                page.items.push_back(std::move(item));
                item.body.clear();  // taken
                break;
            }
        }
        if (page.items.size() >= limit) {
            break;
        }
    }
    if (order.empty()) {
        for (auto& entry : comments) {
            if (page.items.size() >= limit) {
                break;
            }
            page.items.push_back(std::move(entry.second));
        }
    }

    if (page.items.empty()) {
        return std::nullopt;
    }

    if (page.title.empty()) {
        page.title = "Yorumlar";
    }
    return page;
}

std::string extract_playlist_title(const json& root) {
    std::string title;
    const auto walk = [&](const auto& self, const json& node) -> void {
        if (!title.empty() || node.is_null()) {
            return;
        }

        if (node.is_object()) {
            if (node.contains("playlistSidebarPrimaryInfoRenderer")
                && node.at("playlistSidebarPrimaryInfoRenderer").is_object()) {
                title = get_text(
                    node.at("playlistSidebarPrimaryInfoRenderer").value("title", json::object()));
                if (!title.empty()) {
                    return;
                }
            }

            for (const auto& entry : node.items()) {
                self(self, entry.value());
                if (!title.empty()) {
                    return;
                }
            }
            return;
        }

        if (node.is_array()) {
            for (const auto& child : node) {
                self(self, child);
                if (!title.empty()) {
                    return;
                }
            }
        }
    };

    walk(walk, root);
    return title;
}

}  // namespace

YouTubeCatalogService::YouTubeCatalogService(HttpClient* client, AuthStore* auth_store)
    : client_(client ? client : &owned_client_)
    , auth_store_(auth_store ? auth_store : &AuthStore::instance()) {
    std::string ignored_error;
    this->auth_store_->load(&ignored_error);
}

bool YouTubeCatalogService::load_auth_session(std::string* error_message) {
    return this->auth_store_->load(error_message);
}

bool YouTubeCatalogService::reload_auth_session(std::string* error_message) {
    if (!this->auth_store_->reload(error_message)) {
        return false;
    }
    this->invalidate_auth_caches();
    return this->auth_store_->load(error_message);
}

bool YouTubeCatalogService::has_auth_session() const {
    return this->auth_store_->has_session();
}

AuthSession YouTubeCatalogService::auth_session() const {
    return this->auth_store_->session();
}

bool YouTubeCatalogService::import_auth_session_from_file(
    const std::string& file_path,
    std::string* error_message) {
    if (!this->auth_store_->import_from_file(file_path, error_message)) {
        return false;
    }
    this->invalidate_auth_caches();
    return true;
}

bool YouTubeCatalogService::update_auth_session_from_cookie(
    const std::string& cookie_header,
    const std::string& source_label,
    std::string* error_message) {
    if (!this->auth_store_->update_from_cookie_header(cookie_header, source_label, error_message)) {
        return false;
    }
    this->invalidate_auth_caches();
    return true;
}

bool YouTubeCatalogService::clear_auth_session(std::string* error_message) {
    if (!this->auth_store_->clear(error_message)) {
        return false;
    }
    this->invalidate_auth_caches();
    return true;
}

std::vector<Kiosk> YouTubeCatalogService::list_kiosks() const {
    // "Kısa videoları gizle" leaves the Shorts category out too.
    const bool hide_shorts = SettingsStore::instance().settings().hide_short_videos;
    std::vector<Kiosk> kiosks;
    kiosks.reserve(kFeedPresets.size());
    for (const auto& preset : kFeedPresets) {
        if (hide_shorts && std::string(preset.id) == "shorts") {
            continue;
        }
        kiosks.push_back(Kiosk{preset.id, preset.title});
    }
    return kiosks;
}

std::optional<HomeFeed> YouTubeCatalogService::fetch_home_feed(
    const std::string& kiosk_id,
    const std::string& title,
    const std::string& query,
    bool allow_short_videos,
    const std::string& search_params) const {
    const auto cache_it = home_feed_cache_.find(kiosk_id);
    if (cache_it != home_feed_cache_.end()) {
        error_message_.clear();
        return cache_it->second;
    }

    // Take the whole first page: the next-page token continues after it, so trimmed items would never show.
    const auto results = fetch_search_results(query, 60, allow_short_videos, search_params);
    if (!error_message_.empty()) {
        logf("youtube: home feed failed id=%s error=%s", kiosk_id.c_str(), error_message_.c_str());
        return std::nullopt;
    }

    HomeFeed feed;
    feed.kiosk = {kiosk_id, title};
    feed.items = results.items;
    feed.next_page_token = results.next_page_token;
    feed.next_page_uses_search = true;
    feed.next_page_allows_shorts = allow_short_videos;
    home_feed_cache_[kiosk_id] = feed;
    logf("youtube: home feed id=%s query=%s items=%zu",
         kiosk_id.c_str(),
         query.c_str(),
         feed.items.size());
    return feed;
}

std::optional<HomeFeed> YouTubeCatalogService::get_home_feed(const std::string& kiosk_id) const {
    if (kiosk_id == "shorts") {
        return this->get_shorts_feed("shorts");
    }
    const AppSettings settings = SettingsStore::instance().settings();
    for (const auto& preset : kFeedPresets) {
        if (kiosk_id == preset.id) {
            if (kiosk_id == "recommended" && this->auth_store_->has_session()) {
                const auto personalized_feed = this->fetch_authenticated_browse_feed(
                    "FEwhat_to_watch",
                    preset.title,
                    "https://www.youtube.com/",
                    60,
                    preset.allow_short_videos && !settings.hide_short_videos);
                if (personalized_feed.has_value()) {
                    return personalized_feed;
                }

                logf(
                    "youtube: personalized home fallback error=%s",
                    this->error_message_.c_str());
                this->error_message_.clear();
            }

            return fetch_home_feed(
                preset.id,
                preset.title,
                preset_query(preset),
                preset.allow_short_videos && !settings.hide_short_videos,
                preset.search_params);
        }
    }

    error_message_ = localized("Desteklenmeyen ana sayfa kategorisi", "Unsupported Home category");
    return std::nullopt;
}

std::optional<HomeFeed> YouTubeCatalogService::fetch_authenticated_browse_feed(
    const std::string& browse_id,
    const std::string& title,
    const std::string& referer,
    size_t limit,
    bool allow_short_videos) const {
    const auto cache_it = this->authenticated_browse_cache_.find(browse_id);
    if (cache_it != this->authenticated_browse_cache_.end()) {
        this->error_message_.clear();
        return cache_it->second;
    }

    std::string auth_error;
    if (!this->auth_store_->load(&auth_error)) {
        this->error_message_ = auth_error.empty() ? localized("Giriş oturumu yüklenemedi", "Could not load the login session") : auth_error;
        return std::nullopt;
    }
    if (!this->auth_store_->has_session()) {
        this->error_message_ = localized("Abonelik akışı için giriş yapman gerekiyor", "Sign in to see your subscriptions");
        return std::nullopt;
    }

    json payload = {
        {"browseId", browse_id},
        {"context", web_browse_client_context()},
    };

    auto headers =
        this->auth_store_->build_youtube_headers("https://www.youtube.com", referer, &auth_error);
    if (!auth_error.empty()) {
        this->error_message_ = auth_error;
        return std::nullopt;
    }

    headers.push_back({"Content-Type", "application/json"});
    headers.push_back({"User-Agent", kWebUserAgent});
    headers.push_back({"X-Youtube-Client-Name", "1"});
    headers.push_back({"X-Youtube-Client-Version", kWebClientVersion});

    const auto response = this->client_->post(kBrowseApiUrl, payload.dump(), headers);
    if (!response.has_value() || response->empty()) {
        this->error_message_ = localized("Abonelik akışı isteği başarısız", "The subscriptions request failed");
        return std::nullopt;
    }

    const json root = json::parse(*response, nullptr, false);
    if (root.is_discarded()) {
        this->error_message_ = localized("Abonelik akışı yanıtı çözümlenemedi", "Could not read the subscriptions feed");
        return std::nullopt;
    }

    HomeFeed feed;
    feed.kiosk = {browse_id, title};
    std::unordered_set<std::string> seen_ids;
    collect_stream_items(root, allow_short_videos, limit, seen_ids, feed.items);
    if (feed.items.empty()) {
        this->error_message_ = localized("Abonelik akışında video bulunamadı", "No videos in the subscriptions feed");
        return std::nullopt;
    }
    feed.next_page_token = find_next_page_token(root);
    feed.next_page_uses_search = false;
    feed.next_page_allows_shorts = allow_short_videos;

    this->cache_stream_details(feed.items);
    this->authenticated_browse_cache_[browse_id] = feed;
    this->error_message_.clear();
    logf("youtube: authenticated browse id=%s items=%zu", browse_id.c_str(), feed.items.size());
    return feed;
}

std::optional<StreamDetail> YouTubeCatalogService::fetch_stream_detail_from_player(
    const std::string& url) const {
    const auto video_id = extract_video_id_from_url(url);
    if (!video_id.has_value()) {
        this->error_message_ = localized("Video kimliği bulunamadı", "No video ID found");
        return std::nullopt;
    }

    json payload = {
        {"videoId", *video_id},
        {"contentCheckOk", true},
        {"racyCheckOk", true},
        {"context",
         {{"client",
           {{"clientName", "ANDROID"},
            {"clientVersion", kAndroidClientVersion},
            {"androidSdkVersion", 30},
            {"hl", content_hl()},
            {"gl", content_gl()}}}}},
    };

    const auto response = this->client_->post(
        kPlayerApiUrl,
        payload.dump(),
        {
            {"Content-Type", "application/json"},
            {"User-Agent", kAndroidUserAgent},
            {"X-Youtube-Client-Name", "3"},
            {"X-Youtube-Client-Version", kAndroidClientVersion},
            {"Origin", "https://www.youtube.com"},
        });

    if (!response.has_value() || response->empty()) {
        this->error_message_ = localized("Video ayrıntıları isteği başarısız", "The video details request failed");
        return std::nullopt;
    }

    const json root = json::parse(*response, nullptr, false);
    if (root.is_discarded()) {
        this->error_message_ = localized("Video ayrıntıları çözümlenemedi", "Could not read the video details");
        return std::nullopt;
    }

    const json playability = root.value("playabilityStatus", json::object());
    const std::string playability_status = get_string(playability, "status");
    if (!playability_status.empty() && playability_status != "OK") {
        this->error_message_ = localized("Video ayrıntıları yüklenemedi: ", "Could not load the video details: ") + playability_status;
        return std::nullopt;
    }

    StreamDetail detail;
    const auto cached_it = this->detail_cache_.find(url);
    if (cached_it != this->detail_cache_.end()) {
        detail = cached_it->second;
    }

    const json video_details = root.value("videoDetails", json::object());
    const json microformat = root.value("microformat", json::object())
                                 .value("playerMicroformatRenderer", json::object());

    detail.item.id = video_id.value_or(detail.item.id);
    detail.item.url = "https://www.youtube.com/watch?v=" + detail.item.id;
    if (detail.item.title.empty()) {
        detail.item.title = get_string(video_details, "title");
    }
    if (detail.item.channel_name.empty()) {
        detail.item.channel_name = get_string(video_details, "author");
    }
    if (detail.item.channel_id.empty()) {
        detail.item.channel_id = get_string(video_details, "channelId");
    }
    if (detail.item.channel_url.empty() && !detail.item.channel_id.empty()) {
        detail.item.channel_url = "https://www.youtube.com/channel/" + detail.item.channel_id;
    }
    if (detail.item.thumbnail_url.empty()) {
        detail.item.thumbnail_url = get_thumbnail_url_from_node(video_details.value("thumbnail", json::object()));
    }
    if (detail.item.duration_text.empty()) {
        detail.item.duration_text = format_duration_text_from_seconds(get_string(video_details, "lengthSeconds"));
    }
    if (detail.item.view_count_text.empty()) {
        detail.item.view_count_text = format_view_count_text(get_string(video_details, "viewCount"));
    }
    if (detail.item.published_text.empty()) {
        detail.item.published_text = get_string(microformat, "publishDate");
        if (detail.item.published_text.empty()) {
            detail.item.published_text = get_string(microformat, "uploadDate");
        }
        detail.item.published_text = relative_time_text(detail.item.published_text);
    }
    detail.item.is_live = video_details.value("isLiveContent", detail.item.is_live);

    if (detail.description.empty()) {
        detail.description = get_string(video_details, "shortDescription");
    }

    if (detail.related_items.empty() && !detail.item.title.empty()) {
        const AppSettings settings = SettingsStore::instance().settings();
        const std::string saved_error = this->error_message_;
        detail.related_items = fetch_watch_related_items(
            this->client_,
            detail.item.url,
            detail.item.id,
            !settings.hide_short_videos,
            &detail);

        if (detail.related_items.empty()) {
            SearchResults related_results = this->fetch_search_results(
                detail.item.title + " " + detail.item.channel_name,
                16,
                !settings.hide_short_videos);
            this->error_message_.clear();

            for (const auto& candidate : related_results.items) {
                if (candidate.id == detail.item.id) {
                    continue;
                }
                detail.related_items.push_back(candidate);
                if (detail.related_items.size() >= 12) {
                    break;
                }
            }
        }

        if (detail.related_items.empty()) {
            this->error_message_ = saved_error;
        }
    }

    this->cache_stream_detail(detail);
    this->error_message_.clear();
    logf("youtube: detail url=%s related=%zu", detail.item.url.c_str(), detail.related_items.size());
    return detail;
}

std::optional<HomeFeed> YouTubeCatalogService::fetch_channel_feed_from_rss(
    const StreamItem& item,
    size_t limit) const {
    std::string channel_id = item.channel_id;
    if (channel_id.empty()) {
        channel_id = extract_channel_id_from_url(item.channel_url);
    }
    if (channel_id.empty() && !item.url.empty()) {
        const auto detail = this->get_stream_detail(item.url);
        if (detail.has_value()) {
            channel_id = detail->item.channel_id;
        }
    }
    if (channel_id.empty()) {
        this->error_message_ = localized("Kanal kimliği bulunamadı", "No channel ID found");
        return std::nullopt;
    }

    const auto cache_it = this->channel_feed_cache_.find(channel_id);
    if (cache_it != this->channel_feed_cache_.end()) {
        this->error_message_.clear();
        return cache_it->second;
    }

    const auto response = this->client_->get(kChannelFeedUrlPrefix + channel_id);
    if (!response.has_value() || response->empty()) {
        this->error_message_ = localized("Kanal akışı isteği başarısız", "The channel request failed");
        return std::nullopt;
    }

    HomeFeed feed;
    feed.kiosk = {channel_id, item.channel_name.empty() ? "Kanal videoları" : item.channel_name + " videoları"};

    size_t search_from = 0;
    while (feed.items.size() < limit) {
        const size_t entry_start = response->find("<entry>", search_from);
        if (entry_start == std::string::npos) {
            break;
        }

        const size_t entry_end = response->find("</entry>", entry_start);
        if (entry_end == std::string::npos) {
            break;
        }

        const std::string entry = response->substr(entry_start, entry_end - entry_start);
        search_from = entry_end + 8;

        StreamItem entry_item;
        entry_item.id = extract_xml_tag(entry, "yt:videoId");
        if (entry_item.id.empty()) {
            continue;
        }

        entry_item.url = "https://www.youtube.com/watch?v=" + entry_item.id;
        entry_item.title = extract_xml_tag(entry, "title");
        entry_item.channel_name = extract_xml_tag(entry, "name");
        entry_item.channel_url = extract_xml_tag(entry, "uri");
        entry_item.channel_id = extract_xml_tag(entry, "yt:channelId");
        entry_item.thumbnail_url = extract_xml_attribute(entry, "<media:thumbnail", "url");
        entry_item.view_count_text =
            format_view_count_text(extract_xml_attribute(entry, "<media:statistics", "views"));
        entry_item.published_text = relative_time_text(extract_xml_tag(entry, "published"));
        entry_item.is_live = false;

        if (entry_item.title.empty()) {
            continue;
        }

        feed.items.push_back(entry_item);
    }

    if (feed.items.empty()) {
        this->error_message_ = localized("Kanalda video bulunamadı", "No videos on this channel");
        return std::nullopt;
    }

    this->cache_stream_details(feed.items);
    this->channel_feed_cache_[channel_id] = feed;
    this->error_message_.clear();
    logf("youtube: channel feed id=%s items=%zu", channel_id.c_str(), feed.items.size());
    return feed;
}

std::optional<HomeFeed> YouTubeCatalogService::fetch_playlist_feed_from_browse(
    const std::string& playlist_id,
    size_t limit) const {
    if (playlist_id.empty()) {
        this->error_message_ = localized("Oynatma listesi kimliği bulunamadı", "No playlist ID found");
        return std::nullopt;
    }

    const auto cache_it = this->playlist_feed_cache_.find(playlist_id);
    if (cache_it != this->playlist_feed_cache_.end()) {
        this->error_message_.clear();
        return cache_it->second;
    }

    json payload = {
        {"browseId", "VL" + playlist_id},
        {"context",
         {{"client",
           {{"clientName", "WEB"},
            {"clientVersion", kWebClientVersion},
            {"hl", content_hl()},
            {"gl", content_gl()},
            {"platform", "DESKTOP"}}}}},
    };

    const auto response = this->client_->post(
        kBrowseApiUrl,
        payload.dump(),
        {
            {"Content-Type", "application/json"},
            {"User-Agent", kWebUserAgent},
            {"X-Youtube-Client-Name", "1"},
            {"X-Youtube-Client-Version", kWebClientVersion},
            {"Origin", "https://www.youtube.com"},
        });
    if (!response.has_value() || response->empty()) {
        this->error_message_ = localized("Oynatma listesi isteği başarısız", "The playlist request failed");
        return std::nullopt;
    }

    const json root = json::parse(*response, nullptr, false);
    if (root.is_discarded()) {
        this->error_message_ = localized("Oynatma listesi çözümlenemedi", "Could not read the playlist");
        return std::nullopt;
    }

    if (root.contains("alerts") && root.at("alerts").is_array() && !root.at("alerts").empty()) {
        const std::string alert_text = get_text(
            root.at("alerts").front().value("alertRenderer", json::object()).value("text", json::object()));
        if (!alert_text.empty()) {
            this->error_message_ = alert_text;
            return std::nullopt;
        }
    }

    HomeFeed feed;
    const std::string playlist_title = extract_playlist_title(root);
    feed.kiosk = {
        playlist_id,
        playlist_title.empty() ? "Oynatma listesi" : playlist_title,
    };

    feed.playlist = parse_playlist_header(root, playlist_id);
    if (feed.playlist->title.empty()) {
        feed.playlist->title = feed.kiosk.title;
    }

    const AppSettings settings = SettingsStore::instance().settings();
    std::unordered_set<std::string> seen_ids;
    collect_playlist_items(root, !settings.hide_short_videos, limit, seen_ids, feed.items);
    if (feed.items.empty()) {
        this->error_message_ = localized("Oynatma listesinde video bulunamadı", "No videos in this playlist");
        return std::nullopt;
    }

    this->cache_stream_details(feed.items);
    this->playlist_feed_cache_[playlist_id] = feed;
    this->error_message_.clear();
    logf("youtube: playlist feed id=%s items=%zu", playlist_id.c_str(), feed.items.size());
    return feed;
}

std::optional<CommentPage> YouTubeCatalogService::fetch_comments_from_watch(
    const StreamItem& item,
    size_t limit) const {
    const std::string cache_key = !item.url.empty() ? item.url : item.id;
    const auto cache_it = this->comments_cache_.find(cache_key);
    if (cache_it != this->comments_cache_.end()) {
        this->error_message_.clear();
        return cache_it->second;
    }

    if (item.url.empty()) {
        this->error_message_ = localized("Yorum adresi yok", "No comments address");
        return std::nullopt;
    }

    const auto initial_data = fetch_watch_page_initial_data(this->client_, item.url);
    if (!initial_data.has_value()) {
        this->error_message_ = localized("Yorum sayfası isteği başarısız", "The comments request failed");
        return std::nullopt;
    }

    const std::string continuation_token = extract_comments_continuation_token(*initial_data);
    if (continuation_token.empty()) {
        this->error_message_ = localized("Yorum devam anahtarı bulunamadı", "No more comments found");
        return std::nullopt;
    }

    auto page = fetch_comments_page(this->client_, continuation_token, limit);
    if (!page.has_value()) {
        this->error_message_ = localized("Yorumlar çözümlenemedi", "Could not read the comments");
        return std::nullopt;
    }
    read_comments_panel(*initial_data, *page);

    this->comments_cache_[cache_key] = *page;
    this->error_message_.clear();
    logf("youtube: comments url=%s items=%zu", item.url.c_str(), page->items.size());
    return page;
}

SearchResults YouTubeCatalogService::fetch_search_results(
    const std::string& query,
    size_t limit,
    bool allow_short_videos,
    const std::string& search_params) const {
    error_message_.clear();

    SearchResults results;
    results.query = query;
    if (query.empty()) {
        return results;
    }

    json payload = {
        {"query", query},
        {"context", android_search_client_context()},
    };
    if (!search_params.empty()) {
        payload["params"] = search_params;
    }

    const auto response = client_->post(kSearchApiUrl, payload.dump(), android_search_headers());

    if (!response.has_value() || response->empty()) {
        error_message_ = localized("YouTube arama isteği başarısız", "The YouTube search request failed");
        return results;
    }

    const json root = json::parse(*response, nullptr, false);
    if (root.is_discarded()) {
        error_message_ = localized("YouTube arama yanıtı çözümlenemedi", "Could not read the YouTube search results");
        return results;
    }

    std::unordered_set<std::string> seen_ids;
    collect_stream_items(root, allow_short_videos, limit, seen_ids, results.items);
    if (results.items.empty()) {
        error_message_ = localized("YouTube aramasında sonuç yok", "No YouTube search results");
        return results;
    }
    results.next_page_token = find_next_page_token(root);

    this->cache_stream_details(results.items);

    logf("youtube: search query=%s items=%zu", query.c_str(), results.items.size());
    return results;
}

std::optional<HomeFeed> YouTubeCatalogService::get_next_page(
    const std::string& token,
    bool uses_search,
    bool allow_short_videos) const {
    error_message_.clear();
    if (token.empty()) {
        error_message_ = localized("Sonraki sayfa yok", "No next page");
        return std::nullopt;
    }

    std::optional<std::string> response;
    if (uses_search) {
        const json payload = {
            {"continuation", token},
            {"context", android_search_client_context()},
        };
        response = client_->post(kSearchApiUrl, payload.dump(), android_search_headers());
    } else {
        // Signed in when there is a session; public pages (a channel) continue without one.
        const json payload = {
            {"continuation", token},
            {"context", web_browse_client_context()},
        };
        response = this->post_web_browse(payload.dump(), false);
    }

    if (!response.has_value() || response->empty()) {
        error_message_ = localized("Sonraki sayfa isteği başarısız", "The next page request failed");
        return std::nullopt;
    }
    const json root = json::parse(*response, nullptr, false);
    if (root.is_discarded()) {
        error_message_ = localized("Sonraki sayfa çözümlenemedi", "Could not read the next page");
        return std::nullopt;
    }

    HomeFeed page;
    std::unordered_set<std::string> seen_ids;
    collect_stream_items(root, allow_short_videos, 80, seen_ids, page.items);
    page.next_page_token = find_next_page_token(root);
    page.next_page_uses_search = uses_search;
    page.next_page_allows_shorts = allow_short_videos;
    logf("youtube: next page search=%d items=%zu more=%d",
         uses_search ? 1 : 0,
         page.items.size(),
         page.next_page_token.empty() ? 0 : 1);
    return page;
}

std::optional<HomeFeed> YouTubeCatalogService::get_subscriptions_feed() const {
    const AppSettings settings = SettingsStore::instance().settings();
    return this->fetch_authenticated_browse_feed(
        "FEsubscriptions",
        "Aboneliklerden son videolar",
        "https://www.youtube.com/feed/subscriptions",
        60,
        !settings.hide_short_videos);
}

std::optional<HomeFeed> YouTubeCatalogService::get_related_feed(const StreamItem& item) const {
    const std::string cache_key = !item.url.empty() ? item.url : item.id;
    const auto cache_it = this->related_feed_cache_.find(cache_key);
    if (cache_it != this->related_feed_cache_.end()) {
        this->error_message_.clear();
        return cache_it->second;
    }

    const auto detail = this->get_stream_detail(item.url);
    if (!detail.has_value() || detail->related_items.empty()) {
        this->error_message_ = localized("İlgili video bulunamadı", "No related videos found");
        return std::nullopt;
    }

    HomeFeed feed;
    feed.kiosk = {"related", "İlgili videolar"};
    feed.items = detail->related_items;
    this->cache_stream_details(feed.items);
    this->related_feed_cache_[cache_key] = feed;
    this->error_message_.clear();
    return feed;
}

std::optional<std::string> YouTubeCatalogService::post_web_browse(
    const std::string& payload,
    bool require_session,
    const std::string& client_version,
    const std::string& url) const {
    std::vector<HttpHeader> headers;
    std::string auth_error;
    if (this->auth_store_->load(&auth_error) && this->auth_store_->has_session()) {
        headers = this->auth_store_->build_youtube_headers(
            "https://www.youtube.com", "https://www.youtube.com/", &auth_error);
    }
    if (headers.empty()) {
        if (require_session) {
            this->error_message_ = auth_error.empty() ? localized("Giriş oturumu yok", "Not signed in") : auth_error;
            return std::nullopt;
        }
        headers.push_back({"Origin", "https://www.youtube.com"});
    }
    headers.push_back({"Content-Type", "application/json"});
    headers.push_back({"User-Agent", kWebUserAgent});
    headers.push_back({"X-Youtube-Client-Name", "1"});
    headers.push_back({"X-Youtube-Client-Version", client_version.empty() ? kWebClientVersion : client_version});
    return this->client_->post(url.empty() ? std::string(kBrowseApiUrl) : url, payload, headers);
}

std::optional<std::string> YouTubeCatalogService::fetch_playlist_page_data(const std::string& playlist_id) const {
    std::string auth_error;
    if (!this->auth_store_->load(&auth_error) || !this->auth_store_->has_session()) {
        return std::nullopt;
    }
    std::string cookie;
    for (const auto& header : this->auth_store_->build_youtube_headers(
             "https://www.youtube.com", "https://www.youtube.com/", &auth_error)) {
        if (header.name == "Cookie") {
            cookie = header.value;
        }
    }
    if (cookie.empty()) {
        return std::nullopt;
    }
    const auto page = this->client_->get(
        "https://www.youtube.com/playlist?list=" + playlist_id,
        {
            {"User-Agent", kWebUserAgent},
            {"Accept-Language", accept_language()},
            {"Cookie", cookie},
        });
    if (!page.has_value() || page->empty()) {
        return std::nullopt;
    }
    for (const char* marker : {"var ytInitialData = ", "window[\"ytInitialData\"] = ", "ytInitialData = "}) {
        if (auto data = extract_json_assignment(*page, marker)) {
            return data;
        }
    }
    logf("youtube: playlist page %s has no ytInitialData (bytes=%zu)", playlist_id.c_str(), page->size());
    return std::nullopt;
}

// The "Videos" tab of a channel through the browse API: every video, page by page (the
// RSS feed has only the newest 15; it stays the fallback).
std::optional<HomeFeed> YouTubeCatalogService::get_channel_feed(const StreamItem& item) const {
    std::string channel_id = item.channel_id;
    const size_t at = item.channel_url.find("/channel/");
    if (channel_id.empty() && at != std::string::npos) {
        channel_id = item.channel_url.substr(at + 9, 24);
    }
    if (!channel_id.empty()) {
        const json payload = {
            {"browseId", channel_id},
            {"params", "EgZ2aWRlb3PyBgQKAjoA"},
            {"context", web_browse_client_context()},
        };
        const auto response = this->post_web_browse(payload.dump(), false);
        const json root = response.has_value() ? json::parse(*response, nullptr, false) : json();
        if (root.is_object()) {
            HomeFeed feed;
            const std::string title = get_string(
                root.value("metadata", json::object()).value("channelMetadataRenderer", json::object()), "title");
            feed.kiosk = {channel_id, title.empty() ? item.channel_name : title};
            ChannelInfo channel = parse_channel_header(root);
            channel.id = channel_id;
            if (channel.name.empty()) {
                channel.name = feed.kiosk.title;
            }
            feed.channel = channel;
            const AppSettings settings = SettingsStore::instance().settings();
            std::unordered_set<std::string> seen_ids;
            collect_stream_items(root, !settings.hide_short_videos, 60, seen_ids, feed.items);
            feed.next_page_token = find_next_page_token(root);
            feed.next_page_allows_shorts = !settings.hide_short_videos;
            if (!feed.items.empty()) {
                for (auto& entry : feed.items) {
                    if (entry.channel_name.empty()) {
                        entry.channel_name = feed.kiosk.title;
                    }
                }
                this->cache_stream_details(feed.items);
                this->error_message_.clear();
                logf("youtube: channel page id=%s items=%zu more=%d", channel_id.c_str(), feed.items.size(),
                     feed.next_page_token.empty() ? 0 : 1);
                return feed;
            }
        }
        logf("youtube: channel page failed id=%s, using the RSS feed", channel_id.c_str());
    }
    return this->fetch_channel_feed_from_rss(item, 24);
}

// One tab of a channel's page (Videos, Shorts, Live, Playlists) with the params its header
// gave; `continuation` instead asks for the tab's next page.
std::optional<HomeFeed> YouTubeCatalogService::get_channel_tab(
    const std::string& channel_id,
    const std::string& params,
    const std::string& continuation) const {
    json payload = {{"context", web_browse_client_context()}};
    if (continuation.empty()) {
        payload["browseId"] = channel_id;
        payload["params"] = params;
    } else {
        payload["continuation"] = continuation;
    }
    const auto response = this->post_web_browse(payload.dump(), false);
    const json root = response.has_value() ? json::parse(*response, nullptr, false) : json();
    if (!root.is_object()) {
        this->error_message_ = localized("Kanal sekmesi yüklenemedi", "Could not load the channel tab");
        return std::nullopt;
    }
    HomeFeed feed;
    feed.kiosk = {channel_id, {}};
    std::unordered_set<std::string> seen_ids;
    collect_channel_tab_items(root, seen_ids, feed.items);
    feed.next_page_token = find_next_page_token(root);
    feed.next_page_allows_shorts = true;
    this->cache_stream_details(feed.items);
    this->error_message_.clear();
    logf("youtube: channel tab id=%s next=%d items=%zu more=%d", channel_id.c_str(), continuation.empty() ? 0 : 1,
         feed.items.size(), feed.next_page_token.empty() ? 0 : 1);
    return feed;
}

// The web client's endpoints other than browse (search, reel), without a session.
static std::optional<std::string> post_web_api(HttpClient* client, const char* endpoint, json payload) {
    payload["context"] = {{"client",
                           {{"clientName", "WEB"},
                            {"clientVersion", kWebClientVersion},
                            {"hl", content_hl()},
                            {"gl", content_gl()},
                            {"platform", "DESKTOP"}}}};
    return client->post(
        std::string("https://www.youtube.com/youtubei/v1/") + endpoint + "?prettyPrint=false",
        payload.dump(),
        {
            {"Content-Type", "application/json"},
            {"User-Agent", kWebUserAgent},
            {"X-Youtube-Client-Name", "1"},
            {"X-Youtube-Client-Version", kWebClientVersion},
            {"Origin", "https://www.youtube.com"},
        });
}

std::optional<HomeFeed> YouTubeCatalogService::get_shorts_sequence(const std::string& params) const {
    if (params.empty()) {
        return std::nullopt;
    }
    const auto response = post_web_api(this->client_, "reel/reel_watch_sequence", {{"sequenceParams", params}});
    const json root = response.has_value() ? json::parse(*response, nullptr, false) : json();
    if (!root.is_object()) {
        this->error_message_ = localized("Shorts sırası yüklenemedi", "Could not load the Shorts queue");
        return std::nullopt;
    }
    HomeFeed feed;
    feed.kiosk = {"shorts", "Shorts"};
    for (const auto& entry : root.value("entries", json::array())) {
        const json endpoint =
            entry.value("command", json::object()).value("reelWatchEndpoint", json::object());
        StreamItem item;
        item.id = get_string(endpoint, "videoId");
        if (item.id.empty()) {
            continue;
        }
        item.url = "https://www.youtube.com/shorts/" + item.id;
        item.thumbnail_url = "https://i.ytimg.com/vi/" + item.id + "/hqdefault.jpg";
        item.duration_text = "Shorts";
        feed.items.push_back(std::move(item));
    }
    feed.next_page_token = get_string(root.value("continuationEndpoint", json::object())
                                          .value("continuationCommand", json::object()),
                                      "token");
    this->error_message_.clear();
    logf("youtube: shorts sequence items=%zu more=%d", feed.items.size(), feed.next_page_token.empty() ? 0 : 1);
    return feed;
}

std::optional<HomeFeed> YouTubeCatalogService::get_shorts_feed(const std::string& query) const {
    // "EgIQCQ==": the search's "Shorts" type filter.
    const auto response = post_web_api(this->client_, "search", {{"query", query}, {"params", "EgIQCQ=="}});
    const json root = response.has_value() ? json::parse(*response, nullptr, false) : json();
    if (!root.is_object()) {
        this->error_message_ = localized("Shorts yüklenemedi", "Could not load Shorts");
        return std::nullopt;
    }
    HomeFeed feed;
    feed.kiosk = {"shorts", "Shorts"};
    std::unordered_set<std::string> seen_ids;
    collect_channel_tab_items(root, seen_ids, feed.items);
    this->cache_stream_details(feed.items);
    this->error_message_.clear();
    logf("youtube: shorts feed query=%s items=%zu", query.c_str(), feed.items.size());
    return feed;
}

// On the console the browse API answered WL and LL with HTTP 500 (2026-09-25) while the
// subscription feed with the same session worked. So: the browse API as today's web client,
// then the list's page on youtube.com, which is what a signed-in browser loads.
std::optional<HomeFeed> YouTubeCatalogService::get_account_playlist(const std::string& playlist_id) const {
    json context = web_browse_client_context();
    context["client"]["clientVersion"] = kCurrentWebClientVersion;
    const json payload = {
        {"browseId", "VL" + playlist_id},
        {"context", context},
    };
    const auto response = this->post_web_browse(payload.dump(), true, kCurrentWebClientVersion);
    if (!response.has_value() && !this->has_auth_session()) {
        return std::nullopt;  // post_web_browse said why
    }
    const AppSettings settings = SettingsStore::instance().settings();
    HomeFeed feed;
    json root = response.has_value() ? json::parse(*response, nullptr, false) : json();
    std::unordered_set<std::string> seen_ids;
    if (root.is_object()) {
        collect_playlist_items(root, !settings.hide_short_videos, 200, seen_ids, feed.items);
    }
    if (feed.items.empty()) {
        logf("youtube: account list %s: browse gave %s, reading the web page", playlist_id.c_str(),
             root.is_object() ? "no videos" : "no answer");
        if (const auto page = this->fetch_playlist_page_data(playlist_id)) {
            root = json::parse(*page, nullptr, false);
            if (root.is_object()) {
                collect_playlist_items(root, !settings.hide_short_videos, 200, seen_ids, feed.items);
            }
        }
    }
    if (!root.is_object()) {
        this->error_message_ = localized("Liste isteği başarısız", "The list request failed");
        return std::nullopt;
    }
    feed.kiosk = {playlist_id, extract_playlist_title(root)};
    feed.playlist = parse_playlist_header(root, playlist_id);
    if (feed.playlist->title.empty()) {
        feed.playlist->title = feed.kiosk.title;
    }
    this->cache_stream_details(feed.items);
    this->error_message_.clear();
    logf("youtube: account list %s items=%zu", playlist_id.c_str(), feed.items.size());
    return feed;
}

// The bell's list, as the web client asks for it: notificationRenderer items (the text, the
// time, the channel's and the video's pictures, the video) wherever the menu puts them.
std::vector<NotificationItem> YouTubeCatalogService::list_notifications() const {
    json context = web_browse_client_context();
    context["client"]["clientVersion"] = kCurrentWebClientVersion;
    const json payload = {
        {"notificationsMenuRequestType", "NOTIFICATIONS_MENU_REQUEST_TYPE_INBOX"},
        {"context", context},
    };
    const auto response = this->post_web_browse(
        payload.dump(), true, kCurrentWebClientVersion,
        "https://www.youtube.com/youtubei/v1/notification/get_notification_menu?prettyPrint=false");
    const json root = response.has_value() ? json::parse(*response, nullptr, false) : json();
    std::vector<NotificationItem> items;
    if (!root.is_object()) {
        logf("youtube: notifications: %s", response.has_value() ? "unreadable answer" : this->error_message_.c_str());
        if (response.has_value()) {
            this->error_message_ = localized("Bildirimler çözümlenemedi", "Could not read the notifications");
        }
        return items;
    }
    const auto last_url = [](const json& image) {
        const json list = image.value("thumbnails", json::array());
        return list.is_array() && !list.empty() ? get_string(list.back(), "url") : std::string();
    };
    const auto walk = [&](const auto& self, const json& node) -> void {
        if (items.size() >= 100) {
            return;
        }
        if (node.is_array()) {
            for (const auto& child : node) {
                self(self, child);
            }
            return;
        }
        if (!node.is_object()) {
            return;
        }
        if (node.contains("notificationRenderer") && node.at("notificationRenderer").is_object()) {
            const json& renderer = node.at("notificationRenderer");
            NotificationItem item;
            item.text = get_text(renderer.value("shortMessage", json::object()));
            item.time = get_text(renderer.value("sentTimeText", json::object()));
            item.avatar_url = last_url(renderer.value("thumbnail", json::object()));
            item.thumbnail_url = last_url(renderer.value("videoThumbnail", json::object()));
            const json endpoint = renderer.value("navigationEndpoint", json::object());
            item.video_id = get_string(endpoint.value("watchEndpoint", json::object()), "videoId");
            if (item.video_id.empty()) {
                item.video_id = get_string(endpoint.value("reelWatchEndpoint", json::object()), "videoId");
            }
            item.unread = !renderer.value("read", true);
            if (!item.text.empty()) {
                items.push_back(std::move(item));
            }
            return;
        }
        for (const auto& entry : node.items()) {
            self(self, entry.value());
        }
    };
    walk(walk, root);
    this->error_message_.clear();
    logf("youtube: notifications=%zu", items.size());
    return items;
}

// youtube.com/feed/playlists: a grid of playlist lockups (the account's own lists and the ones
// it saved), as today's web client gets it.
std::vector<StreamItem> YouTubeCatalogService::list_account_playlists() const {
    json context = web_browse_client_context();
    context["client"]["clientVersion"] = kCurrentWebClientVersion;
    const json payload = {
        {"browseId", "FEplaylist_aggregation"},
        {"context", context},
    };
    const auto response = this->post_web_browse(payload.dump(), true, kCurrentWebClientVersion);
    const json root = response.has_value() ? json::parse(*response, nullptr, false) : json();
    if (!root.is_object()) {
        logf("youtube: account playlists: %s", response.has_value() ? "unreadable answer" : "no answer");
        return {};
    }
    std::unordered_set<std::string> seen_ids;
    std::vector<StreamItem> found;
    collect_channel_tab_items(root, seen_ids, found);
    std::vector<StreamItem> playlists;
    for (auto& item : found) {
        if (item.is_playlist && item.id != "WL" && item.id != "LL") {
            playlists.push_back(std::move(item));
        }
    }
    this->error_message_.clear();
    logf("youtube: account playlists=%zu (items=%zu)", playlists.size(), found.size());
    return playlists;
}

std::optional<HomeFeed> YouTubeCatalogService::get_playlist_feed(const StreamItem& item) const {
    const auto playlist_id = extract_playlist_id_from_url(item.url);
    if (!playlist_id.has_value()) {
        this->error_message_ = localized("Bu video bir oynatma listesinde değil", "This video is not in a playlist");
        return std::nullopt;
    }

    return this->fetch_playlist_feed_from_browse(*playlist_id, 48);
}

std::optional<CommentPage> YouTubeCatalogService::get_comments(const StreamItem& item) const {
    return this->fetch_comments_from_watch(item, kCommentPageLimit);
}

std::optional<CommentPage> YouTubeCatalogService::get_comments_page(const std::string& token) const {
    auto page = fetch_comments_page(this->client_, token, kCommentPageLimit);
    if (!page.has_value()) {
        this->error_message_ = localized("Yorumlar çözümlenemedi", "Could not read the comments");
        return std::nullopt;
    }
    this->error_message_.clear();
    logf("youtube: comments next page items=%zu more=%d", page->items.size(), page->next_page_token.empty() ? 0 : 1);
    return page;
}

std::vector<Chapter> YouTubeCatalogService::get_chapters(const std::string& video_id) const {
    if (video_id.empty()) {
        return {};
    }
    json payload = {
        {"videoId", video_id},
        {"context",
         {{"client",
           {{"clientName", "WEB"},
            {"clientVersion", kWebClientVersion},
            {"hl", content_hl()},
            {"gl", content_gl()},
            {"platform", "DESKTOP"}}}}},
    };
    const auto response = this->client_->post(
        "https://www.youtube.com/youtubei/v1/next?prettyPrint=false",
        payload.dump(),
        {
            {"Content-Type", "application/json"},
            {"User-Agent", kWebUserAgent},
            {"X-Youtube-Client-Name", "1"},
            {"X-Youtube-Client-Version", kWebClientVersion},
            {"Origin", "https://www.youtube.com"},
        });
    if (!response.has_value() || response->empty()) {
        return {};
    }
    const json root = json::parse(*response, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        return {};
    }
    auto chapters = parse_chapters(root);
    logf("youtube: chapters video=%s count=%zu", video_id.c_str(), chapters.size());
    return chapters;
}

std::vector<YouTubeAccount> YouTubeCatalogService::list_accounts() const {
    std::string auth_error;
    if (!this->auth_store_->load(&auth_error) || !this->auth_store_->has_session()) {
        this->error_message_ = localized("Giriş oturumu yok", "Not signed in");
        return {};
    }
    // The channel switcher lists every channel of the account; the plain account menu is the
    // fallback when it names none.
    for (const char* request_type : {"ACCOUNTS_LIST_REQUEST_TYPE_CHANNEL_SWITCHER", ""}) {
        json payload = {
            {"context",
             {{"client",
               {{"clientName", "WEB"},
                {"clientVersion", kCurrentWebClientVersion},
                {"hl", content_hl()},
                {"gl", content_gl()}}}}},
        };
        if (*request_type) {
            payload["requestType"] = request_type;
            payload["callCircumstance"] = "SWITCHING_USERS_FULL";
        }
        auto headers = this->auth_store_->build_youtube_headers(
            "https://www.youtube.com", "https://www.youtube.com/", &auth_error);
        headers.push_back({"Content-Type", "application/json"});
        headers.push_back({"User-Agent", kWebUserAgent});
        headers.push_back({"X-Youtube-Client-Name", "1"});
        headers.push_back({"X-Youtube-Client-Version", kCurrentWebClientVersion});
        const auto response = this->client_->post(
            "https://www.youtube.com/youtubei/v1/account/accounts_list?prettyPrint=false", payload.dump(), headers);
        const json root = response.has_value() ? json::parse(*response, nullptr, false) : json();
        std::vector<YouTubeAccount> accounts;
        collect_accounts(root, accounts);
        if (!accounts.empty()) {
            for (const auto& account : accounts) {
                logf("youtube: account %s page_id=%s selected=%d", account.handle.c_str(),
                     account.page_id.empty() ? "no" : "yes", account.selected ? 1 : 0);
            }
            this->error_message_.clear();
            return accounts;
        }
        std::string keys;
        if (root.is_object()) {
            for (const auto& entry : root.items()) {
                keys += entry.key() + " ";
            }
        }
        logf("youtube: accounts_list %s gave no accounts (keys: %s)", *request_type ? request_type : "default",
             keys.c_str());
    }
    this->error_message_ = localized("Hesap listesi alınamadı", "Could not get the account list");
    return {};
}

SearchResults YouTubeCatalogService::search(const std::string& query) const {
    const AppSettings settings = SettingsStore::instance().settings();
    return fetch_search_results(query, 60, !settings.hide_short_videos);
}

std::optional<StreamDetail> YouTubeCatalogService::get_stream_detail(const std::string& url) const {
    const auto it = detail_cache_.find(url);
    if (it != detail_cache_.end()
        && (!it->second.description.empty() || !it->second.related_items.empty()
            || !it->second.item.channel_id.empty())) {
        error_message_.clear();
        return it->second;
    }

    return this->fetch_stream_detail_from_player(url);
}

void YouTubeCatalogService::cache_stream_details(const std::vector<StreamItem>& items) const {
    for (const auto& item : items) {
        StreamDetail detail;
        const auto it = this->detail_cache_.find(item.url);
        if (it != this->detail_cache_.end()) {
            detail = it->second;
        }
        detail.item = item;
        this->detail_cache_[item.url] = detail;
    }
}

void YouTubeCatalogService::cache_stream_detail(const StreamDetail& detail) const {
    this->detail_cache_[detail.item.url] = detail;
}

void YouTubeCatalogService::clear_feed_caches() {
    home_feed_cache_.clear();
    authenticated_browse_cache_.clear();
}

void YouTubeCatalogService::invalidate_auth_caches() {
    this->authenticated_browse_cache_.clear();
}

}  // namespace newpipe
