#include "newpipe/switch_player.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <fcntl.h>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#ifdef __SWITCH__
#include <switch.h>
#endif

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengles2.h>
#include <curl/curl.h>
#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_gl.h>
#include <mpv/stream_cb.h>

#include "newpipe/app_paths.hpp"
#include "newpipe/content_locale.hpp"
#include "newpipe/i18n.hpp"
#include "newpipe/library_store.hpp"
#include "newpipe/playback_helper.hpp"
#include "newpipe/runtime.hpp"
#include "newpipe/sponsorblock.hpp"
#include "newpipe/subtitles.hpp"
#include "newpipe/youtube_catalog_service.hpp"
#include "newpipe/log.hpp"
#include "newpipe/osd_font.hpp"
#include "newpipe/settings_store.hpp"
#include "newpipe/ump.hpp"
#include "newpipe/watch_progress_store.hpp"
#include "newpipe/youtube_resolver.hpp"

namespace newpipe {
namespace {

constexpr const char* kUserAgent =
    "Mozilla/5.0 (Nintendo Switch; Switch-NewPipe)";
constexpr const char* kDownloadUserAgent =
    "com.google.android.youtube/20.10.38 (Linux; U; Android 11) gzip";
constexpr const char* kUmpDownloadUserAgent =
    "com.google.android.apps.youtube.vr.oculus/1.65.10 "
    "(Linux; U; Android 12L; eureka-user Build/SQ3A.220605.009.A1) gzip";
constexpr const char* kProtocolPrefix = "switchcache://";
constexpr float kPi = 3.14159265f;
constexpr size_t kInitialStreamBufferBytes = 512 * 1024;
constexpr double kShortSeekSeconds = 10.0;
constexpr double kLongSeekSeconds = 60.0;
// Playback speeds cycled with Y; the choice carries over to the next videos of this run.
constexpr double kPlaybackSpeeds[] = {1.0, 1.25, 1.5, 1.75, 2.0, 0.75};
double g_playback_speed = 1.0;
// The video the loop was switched on for: another quality reopens it, looping still.
std::string g_loop_video_id;

std::string format_speed(double speed) {
    char text[16];
    std::snprintf(text, sizeof(text), "%.2f", speed);
    std::string value = text;
    while (!value.empty() && value.back() == '0') {
        value.pop_back();
    }
    if (!value.empty() && value.back() == '.') {
        value.pop_back();
    }
    return value;
}

// mpv hands the path of an HLS playlist to FFmpeg unchanged, and FFmpeg reads the "sdmc:"
// prefix as an unknown protocol ("avformat_open_input() failed"). So on the Switch the trimmed
// playlist never opened and every video fell back to the full master: all ~20 variant
// playlists fetched before the first frame, and the VP9 track picked. A path relative to the
// working directory carries no prefix.
std::string local_playlist_url(const std::string& path) {
#ifdef __SWITCH__
    const size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && chdir(path.substr(0, slash).c_str()) == 0) {
        return path.substr(slash + 1);
    }
#endif
    return path;
}

std::string translate_loading_text(const std::string& value) {
    if (value == "RESOLVING YOUTUBE STREAM") {
        return newpipe::tr("player/loading/resolving_youtube_stream");
    }
    if (value == "CONTACTING PLAYER API") {
        return newpipe::tr("player/loading/contacting_player_api");
    }
    if (value == "SELECTING PLAYABLE FORMAT") {
        return newpipe::tr("player/loading/selecting_playable_format");
    }
    if (value == "REQUESTING HLS STREAM") {
        return newpipe::tr("player/loading/requesting_hls_stream");
    }
    if (value == "REQUESTING AVC STREAM") {
        return newpipe::tr("player/loading/requesting_avc_stream");
    }
    if (value == "REQUESTING PROGRESSIVE STREAM") {
        return newpipe::tr("player/loading/requesting_progressive_stream");
    }
    if (value == "REQUESTING UMP STREAM") {
        return newpipe::tr("player/loading/requesting_ump_stream");
    }
    return value;
}

// The video's thumbnail behind the loading screen (16:9 RGBA); filled by a helper thread.
struct LoadingPictureState {
    std::mutex mutex;
    std::vector<unsigned char> pixels;
    int width = 0;
    int height = 0;
};

// The video picked to play after the current one; filled by a helper thread.
struct NextVideoState {
    std::mutex mutex;
    bool done = false;
    std::optional<StreamItem> item;
    // Its thumbnail as RGBA pixels, handed to a texture by the countdown panel.
    std::vector<unsigned char> thumbnail;
    int thumbnail_width = 0;
    int thumbnail_height = 0;
};

constexpr int kAutoplayCountdownSeconds = 5;

// Shorts play one after another, as in YouTube's Shorts player: down (or a finger moving up)
// goes to the next, up to the previous, the end of one starts the next. Every Short is a
// player run of its own, so the list lives here between runs: the Shorts in order, the one
// playing and the params of the ones after the last (from YouTube's sequence of the first).
struct ShortsQueue {
    std::vector<StreamItem> items;
    size_t index = 0;
    std::string more;
};

ShortsQueue& shorts_queue() {
    static ShortsQueue queue;
    return queue;
}

// More Shorts, or the title of one that came without it, fetched by a helper thread.
struct ShortsFetchState {
    std::mutex mutex;
    bool done = false;
    std::vector<StreamItem> items;
    std::string more;
    std::string title;
};

// A vertical move of the finger this long is a swipe to the next or previous Short.
constexpr int kShortsSwipePixels = 90;

// SponsorBlock segments of the current video, filled by a helper thread.
struct SponsorState {
    std::mutex mutex;
    bool done = false;
    std::vector<SkipSegment> segments;
};

// Chapters of the current video, filled by a helper thread.
struct ChapterState {
    std::mutex mutex;
    bool done = false;
    std::vector<Chapter> chapters;
};

// Subtitles of the current video, filled by a helper thread.
struct SubtitleState {
    std::mutex mutex;
    bool done = false;
    std::optional<CaptionTrack> track;
    std::vector<SubtitleCue> cues;
};

// Turkish names for the subtitle languages YouTube lists most (its own names are English).
std::string turkish_language_name(const std::string& code, const std::string& fallback) {
    static const std::pair<const char*, const char*> names[] = {
        {"tr", "Türkçe"}, {"en", "İngilizce"}, {"de", "Almanca"}, {"fr", "Fransızca"},
        {"es", "İspanyolca"}, {"it", "İtalyanca"}, {"ar", "Arapça"}, {"ru", "Rusça"},
        {"ja", "Japonca"}, {"ko", "Korece"}, {"pt", "Portekizce"}, {"nl", "Felemenkçe"},
    };
    for (const auto& [key, name] : names) {
        if (same_language(code, key)) {
            return name;
        }
    }
    return fallback.empty() ? code : fallback;
}

// Loudness YouTube normalizes to; like YouTube, only louder videos are turned down.
constexpr double kTargetLoudnessLufs = -14.0;

struct StreamSession {
    class SwitchPlayer* player = nullptr;
    size_t position = 0;
    bool is_audio = false;
};

struct FileDownloadContext {
    int fd = -1;
    size_t offset = 0;
    std::atomic<size_t>* progress = nullptr;
};

struct CurlByteBuffer {
    std::vector<uint8_t> bytes;
};

struct Glyph {
    char ch;
    std::array<const char*, 7> rows;
};

constexpr std::array<const char*, 7> kBlankRows = {
    "00000",
    "00000",
    "00000",
    "00000",
    "00000",
    "00000",
    "00000",
};

constexpr Glyph kGlyphs[] = {
    {'A', {"01110", "10001", "10001", "11111", "10001", "10001", "10001"}},
    {'B', {"11110", "10001", "10001", "11110", "10001", "10001", "11110"}},
    {'C', {"01110", "10001", "10000", "10000", "10000", "10001", "01110"}},
    {'D', {"11100", "10010", "10001", "10001", "10001", "10010", "11100"}},
    {'E', {"11111", "10000", "10000", "11110", "10000", "10000", "11111"}},
    {'F', {"11111", "10000", "10000", "11110", "10000", "10000", "10000"}},
    {'G', {"01110", "10001", "10000", "10111", "10001", "10001", "01110"}},
    {'H', {"10001", "10001", "10001", "11111", "10001", "10001", "10001"}},
    {'I', {"11111", "00100", "00100", "00100", "00100", "00100", "11111"}},
    {'J', {"00111", "00010", "00010", "00010", "00010", "10010", "01100"}},
    {'K', {"10001", "10010", "10100", "11000", "10100", "10010", "10001"}},
    {'L', {"10000", "10000", "10000", "10000", "10000", "10000", "11111"}},
    {'M', {"10001", "11011", "10101", "10101", "10001", "10001", "10001"}},
    {'N', {"10001", "11001", "10101", "10011", "10001", "10001", "10001"}},
    {'O', {"01110", "10001", "10001", "10001", "10001", "10001", "01110"}},
    {'P', {"11110", "10001", "10001", "11110", "10000", "10000", "10000"}},
    {'Q', {"01110", "10001", "10001", "10001", "10101", "10010", "01101"}},
    {'R', {"11110", "10001", "10001", "11110", "10100", "10010", "10001"}},
    {'S', {"01111", "10000", "10000", "01110", "00001", "00001", "11110"}},
    {'T', {"11111", "00100", "00100", "00100", "00100", "00100", "00100"}},
    {'U', {"10001", "10001", "10001", "10001", "10001", "10001", "01110"}},
    {'V', {"10001", "10001", "10001", "10001", "10001", "01010", "00100"}},
    {'W', {"10001", "10001", "10001", "10101", "10101", "10101", "01010"}},
    {'X', {"10001", "10001", "01010", "00100", "01010", "10001", "10001"}},
    {'Y', {"10001", "10001", "01010", "00100", "00100", "00100", "00100"}},
    {'Z', {"11111", "00001", "00010", "00100", "01000", "10000", "11111"}},
    {'0', {"01110", "10001", "10011", "10101", "11001", "10001", "01110"}},
    {'1', {"00100", "01100", "00100", "00100", "00100", "00100", "01110"}},
    {'2', {"01110", "10001", "00001", "00010", "00100", "01000", "11111"}},
    {'3', {"11110", "00001", "00001", "01110", "00001", "00001", "11110"}},
    {'4', {"00010", "00110", "01010", "10010", "11111", "00010", "00010"}},
    {'5', {"11111", "10000", "10000", "11110", "00001", "00001", "11110"}},
    {'6', {"01110", "10000", "10000", "11110", "10001", "10001", "01110"}},
    {'7', {"11111", "00001", "00010", "00100", "01000", "01000", "01000"}},
    {'8', {"01110", "10001", "10001", "01110", "10001", "10001", "01110"}},
    {'9', {"01110", "10001", "10001", "01111", "00001", "00001", "01110"}},
    {'-', {"00000", "00000", "00000", "11111", "00000", "00000", "00000"}},
    {'.', {"00000", "00000", "00000", "00000", "00000", "01100", "01100"}},
    {':', {"00000", "01100", "01100", "00000", "01100", "01100", "00000"}},
    {'/', {"00001", "00010", "00100", "01000", "10000", "00000", "00000"}},
    {'?', {"01110", "10001", "00001", "00010", "00100", "00000", "00100"}},
    {'(', {"00010", "00100", "01000", "01000", "01000", "00100", "00010"}},
    {')', {"01000", "00100", "00010", "00010", "00010", "00100", "01000"}},
    {' ', {"00000", "00000", "00000", "00000", "00000", "00000", "00000"}},
};

const Glyph& glyph_for_char(char ch) {
    const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    for (const auto& glyph : kGlyphs) {
        if (glyph.ch == upper) {
            return glyph;
        }
    }

    static constexpr Glyph kBlank = {' ', kBlankRows};
    return kBlank;
}

std::string trim_text(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

// Base letter of a Latin-1 / Turkish letter for the OSD bitmap font, 0 when there is none.
char osd_base_letter(unsigned int code_point) {
    switch (code_point) {
        case 0x011E: case 0x011F: return 'G';  // Gg with breve
        case 0x0130: case 0x0131: return 'I';  // dotted capital I, dotless i
        case 0x015E: case 0x015F: return 'S';  // Ss with cedilla
        case 0x2018: case 0x2019: return '\'';
        case 0x2013: case 0x2014: return '-';
        default: break;
    }
    if (code_point < 0xC0 || code_point > 0xFF) {
        return 0;
    }
    static const char kLatin1[] =
        "AAAAAAACEEEEIIII"  // C0-CF
        "DNOOOOOxOUUUUYTS"  // D0-DF
        "AAAAAAACEEEEIIII"  // E0-EF
        "DNOOOOO/OUUUUYTY"; // F0-FF
    return kLatin1[code_point - 0xC0];
}

// The OSD bitmap font only has A-Z, digits and a few symbols. Every non-ASCII byte used to
// become a blank, so a Turkish title read "ANLATT  KLAR  N" and emoji left wide gaps. Letters
// now map to their base letter and characters without one (emoji, CJK) are dropped.
std::string uppercase_ascii(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size();) {
        const unsigned char lead = static_cast<unsigned char>(value[i]);
        if (lead < 0x80) {
            out.push_back(lead >= 32 && lead <= 126 ? static_cast<char>(std::toupper(lead)) : ' ');
            i++;
            continue;
        }
        const size_t length = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
        unsigned int code_point = 0;
        if (length == 2 && i + 1 < value.size()) {
            code_point = ((lead & 0x1Fu) << 6) | (static_cast<unsigned char>(value[i + 1]) & 0x3Fu);
        } else if (length == 3 && i + 2 < value.size()) {
            code_point = ((lead & 0x0Fu) << 12)
                | ((static_cast<unsigned char>(value[i + 1]) & 0x3Fu) << 6)
                | (static_cast<unsigned char>(value[i + 2]) & 0x3Fu);
        }
        i += length;
        const char base = osd_base_letter(code_point);
        if (base != 0) {
            out.push_back(base);
        }
    }
    return trim_text(out);
}

// Counts characters, not bytes: a cut inside a UTF-8 sequence would break the letter.
std::string clamp_text(const std::string& text, size_t max_length) {
    const size_t keep = max_length <= 3 ? max_length : max_length - 3;
    size_t characters = 0;
    size_t keep_bytes = text.size();
    for (size_t i = 0; i < text.size(); i++) {
        if ((static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) {
            continue;
        }
        if (characters == keep) {
            keep_bytes = i;
        }
        characters++;
    }
    if (characters <= max_length) {
        return text;
    }
    return text.substr(0, keep_bytes) + (max_length <= 3 ? "" : "...");
}

std::string trim(std::string value) {
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), value.end());
    return value;
}

std::vector<std::string> split_header_fields(const std::string& header_fields) {
    std::vector<std::string> fields;
    size_t start = 0;
    while (start < header_fields.size()) {
        size_t end = header_fields.find(',', start);
        if (end == std::string::npos) {
            end = header_fields.size();
        }

        std::string field = trim(header_fields.substr(start, end - start));
        if (!field.empty()) {
            fields.push_back(std::move(field));
        }

        start = end + 1;
    }

    return fields;
}

std::string format_megabytes(double bytes) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.1f MB", bytes / (1024.0 * 1024.0));
    return buffer;
}

double clamp_double(double value, double min_value, double max_value) {
    return std::max(min_value, std::min(max_value, value));
}

std::string format_playback_time(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) {
        return "--:--";
    }

    const int total_seconds = static_cast<int>(seconds);
    const int hours = total_seconds / 3600;
    const int minutes = (total_seconds % 3600) / 60;
    const int secs = total_seconds % 60;

    char buffer[32];
    if (hours > 0) {
        std::snprintf(buffer, sizeof(buffer), "%d:%02d:%02d", hours, minutes, secs);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%02d:%02d", minutes, secs);
    }
    return buffer;
}

bool contains_case_insensitive(std::string haystack, std::string needle) {
    std::transform(haystack.begin(), haystack.end(), haystack.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return haystack.find(needle) != std::string::npos;
}

bool is_manifest_url(const std::string& url) {
    return contains_case_insensitive(url, ".m3u8")
        || contains_case_insensitive(url, ".mpd")
        || contains_case_insensitive(url, "/manifest/")
        || contains_case_insensitive(url, "manifest.googlevideo.com");
}

std::optional<uint64_t> find_query_u64(const std::string& url, const std::string& key) {
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
            const std::string value = url.substr(
                value_start,
                value_end == std::string::npos ? std::string::npos : value_end - value_start);
            try {
                return static_cast<uint64_t>(std::stoull(value));
            } catch (...) {
                return std::nullopt;
            }
        }

        search_from = pos + 1;
    }
}

size_t write_at_fd(int fd, const char* data, size_t total_size, size_t offset) {
    if (::lseek(fd, static_cast<off_t>(offset), SEEK_SET) < 0) {
        return 0;
    }

    size_t written_total = 0;
    while (written_total < total_size) {
        const ssize_t written =
            ::write(fd, data + written_total, total_size - written_total);
        if (written <= 0) {
            break;
        }
        written_total += static_cast<size_t>(written);
    }
    return written_total;
}

size_t read_at_fd(int fd, char* data, size_t total_size, size_t offset) {
    if (::lseek(fd, static_cast<off_t>(offset), SEEK_SET) < 0) {
        return 0;
    }

    size_t read_total = 0;
    while (read_total < total_size) {
        const ssize_t read = ::read(fd, data + read_total, total_size - read_total);
        if (read <= 0) {
            break;
        }
        read_total += static_cast<size_t>(read);
    }
    return read_total;
}

size_t write_download_chunk(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* context = static_cast<FileDownloadContext*>(userdata);
    if (!context || context->fd < 0) {
        return 0;
    }

    const size_t total_size = size * nmemb;
    const size_t written = write_at_fd(
        context->fd, static_cast<const char*>(ptr), total_size, context->offset);
    context->offset += written;
    if (context->progress) {
        context->progress->store(context->offset);
    }
    return written;
}

size_t append_curl_bytes(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buffer = static_cast<CurlByteBuffer*>(userdata);
    if (!buffer) {
        return 0;
    }
    const size_t total_size = size * nmemb;
    const auto* first = static_cast<const uint8_t*>(ptr);
    buffer->bytes.insert(buffer->bytes.end(), first, first + total_size);
    return total_size;
}

bool write_text_file(const std::string& path, const std::string& body) {
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) {
        return false;
    }

    const size_t written = std::fwrite(body.data(), 1, body.size(), file);
    std::fclose(file);
    return written == body.size();
}

// The player's TrueType font while a player runs (set in run(), cleared in cleanup()).
OsdFont* g_osd_font = nullptr;

bool osd_font_ready() {
    return g_osd_font && g_osd_font->ready();
}

// Pixel height of the TrueType text for the scale the layout uses (5x7 cells of scale px).
int osd_pixel_height(int scale) {
    return scale * 9;
}

// Text as the overlay shows it: unchanged with the TrueType font (Turkish letters, mixed
// case), capital ASCII for the bitmap fallback.
std::string osd_text(const std::string& value) {
    if (!osd_font_ready()) {
        return uppercase_ascii(value);
    }
    std::string out = value;
    for (char& ch : out) {
        if (static_cast<unsigned char>(ch) < 0x20) {
            ch = ' ';
        }
    }
    return out;
}

int measure_text_width(const std::string& text, int scale) {
    if (text.empty()) {
        return 0;
    }
    if (osd_font_ready()) {
        return g_osd_font->measure(text, osd_pixel_height(scale));
    }

    return static_cast<int>(uppercase_ascii(text).size()) * scale * 6 - scale;
}

// A rectangle in window pixels (y down): where something was last drawn, for touch hit tests.
struct ScreenRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    bool contains(int px, int py) const {
        return w > 0 && px >= x && px < x + w && py >= y && py < y + h;
    }
};

// The round button in the middle of the player: a translucent dark disc with a white play
// triangle or pause bars, as RGBA pixels (size x size), 4x4 supersampled for smooth edges.
std::vector<unsigned char> make_round_button(int size, bool pause) {
    std::vector<unsigned char> pixels(static_cast<size_t>(size) * size * 4, 0);
    const float s = static_cast<float>(size);
    auto in_icon = [&](float x, float y) {
        if (pause) {
            const bool rows = y >= s * 0.30f && y <= s * 0.70f;
            return rows && ((x >= s * 0.34f && x <= s * 0.45f) || (x >= s * 0.55f && x <= s * 0.66f));
        }
        // Triangle (0.38, 0.28) - (0.38, 0.72) - (0.74, 0.50), pointing right.
        if (x < s * 0.38f || x > s * 0.74f) {
            return false;
        }
        const float half = (s * 0.74f - x) / (s * 0.36f) * (s * 0.22f);
        return std::fabs(y - s * 0.50f) <= half;
    };
    for (int py = 0; py < size; py++) {
        for (int px = 0; px < size; px++) {
            float disc = 0.0f;
            float icon = 0.0f;
            for (int sy = 0; sy < 4; sy++) {
                for (int sx = 0; sx < 4; sx++) {
                    const float x = px + (sx + 0.5f) / 4.0f;
                    const float y = py + (sy + 0.5f) / 4.0f;
                    const float dx = x - s / 2.0f;
                    const float dy = y - s / 2.0f;
                    if (dx * dx + dy * dy <= s * s / 4.0f) {
                        disc += 1.0f / 16.0f;
                        icon += in_icon(x, y) ? 1.0f / 16.0f : 0.0f;
                    }
                }
            }
            // White icon over a black disc at 55%.
            const float background = 0.55f * (disc - icon);
            const float alpha = icon + background;
            const float white = alpha > 0.0f ? icon / alpha : 0.0f;
            unsigned char* out = &pixels[(static_cast<size_t>(py) * size + px) * 4];
            out[0] = out[1] = out[2] = static_cast<unsigned char>(std::lround(white * 255.0f));
            out[3] = static_cast<unsigned char>(std::lround(alpha * 255.0f));
        }
    }
    return pixels;
}

// A filled circle as coverage (size x size, one byte per pixel), 4x4 supersampled edges:
// the progress bar's knob, drawn in red through OsdFont::draw_mask.
std::vector<unsigned char> make_disc_mask(int size) {
    std::vector<unsigned char> coverage(static_cast<size_t>(size) * size, 0);
    const float radius = size / 2.0f;
    for (int py = 0; py < size; py++) {
        for (int px = 0; px < size; px++) {
            int inside = 0;
            for (int sy = 0; sy < 4; sy++) {
                for (int sx = 0; sx < 4; sx++) {
                    const float dx = px + (sx + 0.5f) / 4.0f - radius;
                    const float dy = py + (sy + 0.5f) / 4.0f - radius;
                    inside += dx * dx + dy * dy <= radius * radius ? 1 : 0;
                }
            }
            coverage[static_cast<size_t>(py) * size + px] = static_cast<unsigned char>(inside * 255 / 16);
        }
    }
    return coverage;
}

// The loading spinner as YouTube draws it: a thin ring with a quarter left open. The overlay
// cannot rotate a texture, so there is one mask per step of a turn.
constexpr int kSpinnerSteps = 36;

std::vector<unsigned char> make_arc_mask(int size, float thickness, float start) {
    std::vector<unsigned char> coverage(static_cast<size_t>(size) * size, 0);
    const float center = size / 2.0f;
    const float middle = center - 1.0f - thickness / 2.0f;
    const float sweep = 1.5f * kPi;
    for (int py = 0; py < size; py++) {
        for (int px = 0; px < size; px++) {
            const float dx = px + 0.5f - center;
            const float dy = py + 0.5f - center;
            const float distance = std::sqrt(dx * dx + dy * dy);
            // Soft over a pixel at the ring's edges and at the arc's two ends.
            const float ring = std::clamp(thickness / 2.0f + 0.5f - std::fabs(distance - middle), 0.0f, 1.0f);
            if (ring <= 0.0f) {
                continue;
            }
            float angle = std::fmod(std::atan2(dy, dx) - start, 2.0f * kPi);
            if (angle < 0.0f) {
                angle += 2.0f * kPi;
            }
            const float along = angle <= sweep ? std::min(angle, sweep - angle)
                                               : -std::min(angle - sweep, 2.0f * kPi - angle);
            const float arc = std::clamp(0.5f + along * middle, 0.0f, 1.0f);
            coverage[static_cast<size_t>(py) * size + px] = static_cast<unsigned char>(ring * arc * 255.0f);
        }
    }
    return coverage;
}

// Text cut (by whole characters) to max_width, with an ellipsis when something was cut.
std::string fit_osd_text(OsdFont& font, const std::string& text, int pixel_height, int max_width) {
    if (font.measure(text, pixel_height) <= max_width) {
        return text;
    }
    std::string cut = text;
    while (!cut.empty()) {
        size_t last = cut.size() - 1;
        while (last > 0 && (static_cast<unsigned char>(cut[last]) & 0xC0) == 0x80) {
            last--;
        }
        cut.erase(last);
        while (!cut.empty() && cut.back() == ' ') {
            cut.pop_back();
        }
        if (font.measure(cut + "\u2026", pixel_height) <= max_width) {
            return cut + "\u2026";
        }
    }
    return "\u2026";
}

// A title in at most two lines, broken between words; the second one is cut to fit.
std::vector<std::string> wrap_osd_title(OsdFont& font, const std::string& text, int pixel_height, int max_width) {
    if (font.measure(text, pixel_height) <= max_width) {
        return {text};
    }
    size_t split = 0;
    for (size_t space = text.find(' '); space != std::string::npos; space = text.find(' ', space + 1)) {
        if (font.measure(text.substr(0, space), pixel_height) > max_width) {
            break;
        }
        split = space;
    }
    if (split == 0) {
        return {fit_osd_text(font, text, pixel_height, max_width)};
    }
    return {text.substr(0, split), fit_osd_text(font, text.substr(split + 1), pixel_height, max_width)};
}

void fill_rect(int x, int y, int width, int height, int screen_height, float r, float g, float b) {
    if (width <= 0 || height <= 0) {
        return;
    }

    glEnable(GL_SCISSOR_TEST);
    glScissor(x, screen_height - y - height, width, height);
    glClearColor(r, g, b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
}

void draw_text_line(
    int x,
    int y,
    int scale,
    int screen_height,
    const std::string& text,
    float r,
    float g,
    float b) {
    if (osd_font_ready()) {
        GLint viewport[4] = {0, 0, 0, 0};
        glGetIntegerv(GL_VIEWPORT, viewport);
        g_osd_font->draw(text, x, y, osd_pixel_height(scale), viewport[2], screen_height, r, g, b);
        return;
    }
    // The bitmap font has capital ASCII only; texts with Turkish letters are folded here.
    int cursor_x = x;
    for (char ch : uppercase_ascii(text)) {
        const Glyph& glyph = glyph_for_char(ch);
        for (size_t row = 0; row < glyph.rows.size(); row++) {
            for (int col = 0; col < 5; col++) {
                if (glyph.rows[row][col] == '1') {
                    fill_rect(
                        cursor_x + col * scale,
                        y + static_cast<int>(row) * scale,
                        scale,
                        scale,
                        screen_height,
                        r,
                        g,
                        b);
                }
            }
        }
        cursor_x += scale * 6;
    }
}

const char* end_reason_to_string(mpv_end_file_reason reason) {
    switch (reason) {
        case MPV_END_FILE_REASON_EOF:
            return "eof";
        case MPV_END_FILE_REASON_STOP:
            return "stop";
        case MPV_END_FILE_REASON_QUIT:
            return "quit";
        case MPV_END_FILE_REASON_ERROR:
            return "error";
        case MPV_END_FILE_REASON_REDIRECT:
            return "redirect";
        default:
            return "unknown";
    }
}

class SwitchPlayer {
public:
    explicit SwitchPlayer(const PlaybackRequest& request)
        : request_(request) {
    }

    ~SwitchPlayer() {
        if (g_osd_font == &osd_font_) {
            g_osd_font = nullptr;
        }
        cleanup();
    }

    bool run(std::string& error) {
        logf("player: run begin title=%s request_url=%s", request_.title.c_str(), request_.url.c_str());
#ifndef __SWITCH__
        // Screenshot tests can compare with the old bitmap font.
        const bool use_font = std::getenv("NEWPIPE_OSD_BITMAP") == nullptr;
#else
        const bool use_font = true;
#endif
        if (use_font && osd_font_.load()) {
            g_osd_font = &osd_font_;
            osd_title_ = clamp_text(osd_text(request_.title), 52);
            start_loading_picture_fetch();
        }
#ifdef __SWITCH__
        appletSetMediaPlaybackState(true);
#endif
        set_loading_status(
            newpipe::tr("player/loading/preparing_playback"),
            clamp_text(osd_text(request_.title), 40));

        if (!init_sdl(error)) {
            logf("player: init_sdl failed error=%s", error.c_str());
            return false;
        }

        start_prepare();
        if (!wait_for_prepare(error)) {
            logf("player: prepare failed error=%s", error.c_str());
            return false;
        }

        autoplay_enabled_ = SettingsStore::instance().settings().autoplay_next;
        if (const auto video_id = YouTubeResolver::extract_video_id(request_.url)) {
            video_id_ = *video_id;
            if (SettingsStore::instance().settings().skip_sponsors && !active_is_live_) {
                start_sponsor_fetch();
            }
            if (!active_is_live_) {
                start_chapter_fetch();
            }
            subtitles_on_ = SettingsStore::instance().settings().subtitles_enabled;
            if (subtitles_on_) {
                start_subtitle_fetch();
            }
            if (!active_is_live_) {
                resume_from_ = WatchProgressStore::instance().resume_position(video_id_);
            }
            if (request_.url.find("/shorts/") != std::string::npos) {
                begin_shorts();
            }
        }

        if (!init_mpv(error)) {
            logf("player: init_mpv failed error=%s", error.c_str());
            return false;
        }

        set_loading_status(
            newpipe::tr("player/loading/opening_media_stream"),
            active_quality_label_.empty() ? newpipe::tr("player/loading/waiting_for_mpv_load")
                                          : clamp_text(osd_text(active_quality_label_), 40));
        if (!load_file(error)) {
            logf("player: load_file failed error=%s", error.c_str());
            return false;
        }

        player_input_ready_at_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        mpv_events_pending_.store(true);
        loop();
        save_watch_progress();
        // The loop is kept only for the reopen with another quality.
        if (!reopening_) {
            g_loop_video_id.clear();
        }
        error = terminal_error_;
        logf("player: run end ok=%d", terminal_error_.empty() ? 1 : 0);
        return terminal_error_.empty();
    }

private:
    static void* get_proc_address(void*, const char* name) {
        return SDL_GL_GetProcAddress(name);
    }

    static void on_mpv_wakeup(void* context) {
        static_cast<SwitchPlayer*>(context)->mpv_events_pending_.store(true);
    }

    static void on_render_update(void* context) {
        static_cast<SwitchPlayer*>(context)->render_update_pending_.store(true);
    }

    static int stream_open(void* context, char* uri, mpv_stream_cb_info* info) {
        auto* player = static_cast<SwitchPlayer*>(context);
        std::string requested_uri = uri ? uri : "";
        if (requested_uri.rfind(kProtocolPrefix, 0) != 0) {
            logf("player: unsupported stream uri=%s", requested_uri.c_str());
            return MPV_ERROR_LOADING_FAILED;
        }

        auto* stream = new StreamSession();
        stream->player = player;
        stream->is_audio = requested_uri == std::string(kProtocolPrefix) + "audio";
        if (stream->is_audio) {
            std::lock_guard<std::mutex> lock(player->audio_mutex_);
            if (player->audio_cache_fd_ < 0) {
                log_line("player: audio cache handle missing");
                delete stream;
                return MPV_ERROR_LOADING_FAILED;
            }
            logf("player: stream opened cached path=%s kind=audio", player->audio_cache_path_.c_str());
        } else {
            std::lock_guard<std::mutex> lock(player->stream_mutex_);
            if (player->stream_cache_fd_ < 0) {
                log_line("player: stream cache handle missing");
                delete stream;
                return MPV_ERROR_LOADING_FAILED;
            }
            logf("player: stream opened cached path=%s kind=video", player->stream_cache_path_.c_str());
        }
        info->cookie = stream;
        info->read_fn = &SwitchPlayer::stream_read;
        info->seek_fn = &SwitchPlayer::stream_seek;
        // size_fn stays null on purpose. The demuxer does not need the total size
        // to seek by byte offset, and with an unknown size ffmpeg cannot attempt
        // SEEK_END. That matters for fragmented MP4: a seekable stream with a known
        // size makes mov_read_mfra() jump to the end of the file for the fragment
        // index, which for a partially downloaded cache file is the one offset we
        // can never serve.
        info->size_fn = nullptr;
        info->close_fn = &SwitchPlayer::stream_close;
        return 0;
    }

    // Seeks are served straight out of the download cache file, so only bytes that
    // already landed can be reached. Targets past the buffered edge are rejected
    // instead of waited for: the downloader walks the stream strictly forward, so
    // blocking on a far target would stall the demuxer for as long as the rest of
    // the download takes.
    static int64_t stream_seek(void* cookie, int64_t offset) {
        auto* stream = static_cast<StreamSession*>(cookie);
        if (!stream || !stream->player) {
            return MPV_ERROR_GENERIC;
        }
        if (offset < 0) {
            return MPV_ERROR_UNSUPPORTED;
        }

        auto* player = stream->player;
        const auto target = static_cast<uint64_t>(offset);
        if (stream->is_audio) {
            std::lock_guard<std::mutex> lock(player->audio_mutex_);
            if (player->audio_cache_fd_ < 0 || target > player->audio_downloaded_bytes_.load()) {
                return MPV_ERROR_UNSUPPORTED;
            }
        } else {
            std::lock_guard<std::mutex> lock(player->stream_mutex_);
            if (player->stream_cache_fd_ < 0 || target > player->streamed_bytes_) {
                return MPV_ERROR_UNSUPPORTED;
            }
        }

        stream->position = static_cast<size_t>(target);
        return offset;
    }

    static int64_t stream_read(void* cookie, char* buffer, uint64_t bytes) {
        auto* stream = static_cast<StreamSession*>(cookie);
        if (!stream || !stream->player) {
            return MPV_ERROR_GENERIC;
        }

        auto* player = stream->player;
        if (stream->is_audio) {
            std::unique_lock<std::mutex> lock(player->audio_mutex_);
            if (player->audio_cache_fd_ < 0) {
                return MPV_ERROR_GENERIC;
            }
            while (stream->position >= player->audio_downloaded_bytes_.load() && !player->audio_download_done_.load()) {
                player->audio_cv_.wait_for(lock, std::chrono::milliseconds(100));
            }

            const size_t available = player->audio_downloaded_bytes_.load() > stream->position
                ? player->audio_downloaded_bytes_.load() - stream->position
                : 0;
            const bool failed = player->audio_download_done_.load() && !player->audio_download_success_.load();
            if (available == 0) {
                return failed ? MPV_ERROR_GENERIC : 0;
            }

            const size_t to_read = std::min<size_t>(available, static_cast<size_t>(bytes));
            const size_t read = read_at_fd(player->audio_cache_fd_, buffer, to_read, stream->position);
            if (read == 0) {
                return MPV_ERROR_GENERIC;
            }
            stream->position += read;
            return static_cast<int64_t>(read);
        }

        std::unique_lock<std::mutex> lock(player->stream_mutex_);
        if (player->stream_cache_fd_ < 0) {
            return MPV_ERROR_GENERIC;
        }
        while (stream->position >= player->streamed_bytes_ && !player->stream_download_done_) {
            player->stream_cv_.wait_for(lock, std::chrono::milliseconds(100));
        }

        const size_t available = player->streamed_bytes_ > stream->position
            ? player->streamed_bytes_ - stream->position
            : 0;
        const bool failed = player->stream_download_done_ && !player->stream_download_success_;
        if (available == 0) {
            return failed ? MPV_ERROR_GENERIC : 0;
        }

        const size_t to_read = std::min<size_t>(available, static_cast<size_t>(bytes));
        const size_t read = read_at_fd(player->stream_cache_fd_, buffer, to_read, stream->position);
        if (read == 0) {
            return MPV_ERROR_GENERIC;
        }
        stream->position += read;
        return static_cast<int64_t>(read);
    }

    static void stream_close(void* cookie) {
        delete static_cast<StreamSession*>(cookie);
    }

    static int on_curl_progress(
        void* clientp,
        curl_off_t dltotal,
        curl_off_t dlnow,
        curl_off_t /*ultotal*/,
        curl_off_t /*ulnow*/) {
        auto* player = static_cast<SwitchPlayer*>(clientp);
        if (player->stream_abort_.load()) {
            return 1;
        }
        player->update_download_progress(dltotal, dlnow);
        return 0;
    }

    static size_t on_stream_write(void* ptr, size_t size, size_t nmemb, void* userdata) {
        auto* player = static_cast<SwitchPlayer*>(userdata);
        const size_t total_size = size * nmemb;
        std::lock_guard<std::mutex> lock(player->stream_mutex_);
        if (player->stream_cache_fd_ < 0) {
            return 0;
        }

        const size_t write_offset = player->streamed_bytes_;
        const size_t written = write_at_fd(
            player->stream_cache_fd_, static_cast<const char*>(ptr), total_size, write_offset);
        player->streamed_bytes_ += written;
        player->stream_cv_.notify_all();
        return written;
    }

    static size_t on_audio_stream_write(void* ptr, size_t size, size_t nmemb, void* userdata) {
        auto* player = static_cast<SwitchPlayer*>(userdata);
        const size_t total_size = size * nmemb;
        std::lock_guard<std::mutex> lock(player->audio_mutex_);
        if (player->audio_cache_fd_ < 0) {
            return 0;
        }

        const size_t write_offset = player->audio_downloaded_bytes_.load();
        const size_t written = write_at_fd(
            player->audio_cache_fd_, static_cast<const char*>(ptr), total_size, write_offset);
        player->audio_downloaded_bytes_.store(write_offset + written);
        if (write_offset == 0 && written > 0) {
            logf("player: audio first bytes received=%zu", written);
        }
        player->audio_cv_.notify_all();
        return written;
    }

    static int on_audio_curl_progress(
        void* clientp,
        curl_off_t /*dltotal*/,
        curl_off_t /*dlnow*/,
        curl_off_t /*ultotal*/,
        curl_off_t /*ulnow*/) {
        auto* player = static_cast<SwitchPlayer*>(clientp);
        return player->audio_download_abort_.load() ? 1 : 0;
    }

    void set_loading_status(const std::string& title, const std::string& detail) {
        std::lock_guard<std::mutex> lock(status_mutex_);
        const std::string normalized_title = clamp_text(osd_text(title), 40);
        const std::string normalized_detail = clamp_text(osd_text(detail), 48);
        if (normalized_title == loading_title_ && normalized_detail == loading_detail_) {
            return;
        }
        loading_title_ = normalized_title;
        loading_detail_ = normalized_detail;
        logf("player: status title=%s detail=%s", loading_title_.c_str(), loading_detail_.c_str());
    }

    std::pair<std::string, std::string> get_loading_status() const {
        std::lock_guard<std::mutex> lock(status_mutex_);
        return {loading_title_, loading_detail_};
    }

    void show_osd_message(const std::string& message, int duration_ms = 2500) {
        osd_message_ = clamp_text(osd_text(message), 52);
        osd_visible_until_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(duration_ms);
    }

    bool is_temporary_osd_visible() const {
        return std::chrono::steady_clock::now() < osd_visible_until_;
    }

    bool should_draw_osd() const {
        return first_frame_rendered_ && (osd_pinned_ || last_pause_state_ || is_temporary_osd_visible());
    }

    void refresh_osd_snapshot(bool force = false) {
        if (!mpv_) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (!force && now - last_osd_refresh_ < std::chrono::milliseconds(200)) {
            return;
        }
        last_osd_refresh_ = now;

        double value = 0.0;
        if (mpv_get_property(mpv_, "time-pos", MPV_FORMAT_DOUBLE, &value) >= 0 && std::isfinite(value)
            && now >= osd_time_hold_until_) {
            last_time_pos_ = std::max(0.0, value);
        }
        if (mpv_get_property(mpv_, "duration", MPV_FORMAT_DOUBLE, &value) >= 0 && std::isfinite(value)) {
            last_duration_ = std::max(0.0, value);
        }
        if (mpv_get_property(mpv_, "volume", MPV_FORMAT_DOUBLE, &value) >= 0 && std::isfinite(value)) {
            last_volume_ = clamp_double(value, 0.0, 130.0);
        }

        int paused_flag = 0;
        if (mpv_get_property(mpv_, "pause", MPV_FORMAT_FLAG, &paused_flag) >= 0) {
            last_pause_state_ = paused_flag != 0;
        }

        char* media_title = mpv_get_property_string(mpv_, "media-title");
        if (media_title) {
            const std::string normalized = clamp_text(osd_text(media_title), 52);
            if (!normalized.empty()) {
                osd_title_ = normalized;
            }
            mpv_free(media_title);
        }
    }

    std::string get_mpv_property_text(const char* name) const {
        if (!mpv_ || !name) {
            return {};
        }

        char* value = mpv_get_property_string(mpv_, name);
        if (!value) {
            return {};
        }

        std::string result = value;
        mpv_free(value);
        return result;
    }

    void log_audio_state(const char* phase) const {
        const std::string aid = get_mpv_property_text("aid");
        const std::string codec = get_mpv_property_text("audio-codec-name");
        const std::string channels = get_mpv_property_text("audio-params/channel-count");
        const std::string rate = get_mpv_property_text("audio-params/samplerate");
        const std::string ao = get_mpv_property_text("current-ao");
        const std::string device = get_mpv_property_text("audio-device");
        logf("player: audio-state phase=%s aid=%s codec=%s channels=%s rate=%s ao=%s device=%s",
             phase ? phase : "unknown",
             aid.empty() ? "none" : aid.c_str(),
             codec.empty() ? "none" : codec.c_str(),
             channels.empty() ? "none" : channels.c_str(),
             rate.empty() ? "none" : rate.c_str(),
             ao.empty() ? "none" : ao.c_str(),
             device.empty() ? "none" : device.c_str());
    }

    void toggle_osd(bool& force_redraw) {
        osd_pinned_ = !osd_pinned_;
        show_osd_message(
            osd_pinned_ ? newpipe::tr("player/osd/locked")
                        : newpipe::tr("player/osd/auto"),
            1800);
        force_redraw = true;
    }

    // The info bar, drawn like YouTube's: the video darkens towards the top and the bottom
    // edge; the title and the status sit in the top shade; the red progress bar with its knob,
    // the clock with the chapter and the button hints in the bottom one.
    void render_playback_osd(int width, int height) {
        refresh_osd_snapshot();
        osd_top_rect_ = {};
        osd_bottom_rect_ = {};
        osd_bar_rect_ = {};
        round_button_rect_ = {};
        settings_button_rect_ = {};
        if (!should_draw_osd()) {
            return;
        }

        // Layout in 720p pixels, scaled to the screen (1.5x docked at 1080p).
        const float unit = height / 720.0f;
        auto px = [unit](float value) { return std::max(1, static_cast<int>(std::lround(value * unit))); };
        const int side = px(40);
        const int top_shade = px(170);
        const int bottom_shade = px(210);
        osd_top_rect_ = {0, 0, width, top_shade};
        osd_bottom_rect_ = {0, height - bottom_shade, width, bottom_shade};
        osd_font_.fade(0, 0, width, top_shade, width, height, 0.0f, 0.0f, 0.0f, 0.78f, true);
        osd_font_.fade(0, height - bottom_shade, width, bottom_shade, width, height, 0.0f, 0.0f, 0.0f, 0.86f, false);

        // The round play/pause button in the middle (touch); not over the countdown or a list.
        if (!autoplay_counting_down() && !menu_open()) {
            const int size = std::max(72, height / 7);
            if (const unsigned int texture = round_button_texture(!last_pause_state_)) {
                round_button_rect_ = {(width - size) / 2, (height - size) / 2, size, size};
                osd_font_.draw_image(texture, round_button_rect_.x, round_button_rect_.y, size, size, width, height);
            }
        }

        // Top: the title, and under it the state (or the latest message).
        const int title_scale = std::max(3, height / 240);
        const int meta_scale = std::max(2, height / 360);
        const std::string title = clamp_text(
            osd_title_.empty() ? clamp_text(osd_text(request_.title), 60) : osd_title_, 60);
        const std::string playback_state = last_pause_state_ ? newpipe::tr("player/status/paused")
            : active_is_live_ ? newpipe::tr("player/status/live")
                              : newpipe::tr("player/status/playing");
        const std::string status_line = is_temporary_osd_visible() && !osd_message_.empty()
            ? osd_message_
            : clamp_text(
                  playback_state
                      + (active_quality_label_.empty()
                             ? ""
                             : "  •  " + clamp_text(osd_text(active_quality_label_), 18)),
                  60);
        if (!title.empty()) {
            draw_text_line(side, px(24), title_scale, height, title, 1.0f, 1.0f, 1.0f);
        }
        // Top right: the settings list's button, for a finger ("+" on the pad).
        if (!menu_open()) {
            const std::string label = newpipe::tr("player/settings/button");
            const int button_w = measure_text_width(label, meta_scale) + px(32);
            const int button_h = meta_scale * 9 + px(18);
            settings_button_rect_ = {width - side - button_w, px(22), button_w, button_h};
            osd_font_.fill(settings_button_rect_.x, settings_button_rect_.y, button_w, button_h, width, height, 1.0f,
                           1.0f, 1.0f, 0.18f);
            draw_text_line(settings_button_rect_.x + px(16), settings_button_rect_.y + px(9), meta_scale, height, label,
                           1.0f, 1.0f, 1.0f);
        }
        draw_text_line(
            side,
            px(24) + title_scale * 9 + px(10),
            meta_scale,
            height,
            status_line,
            last_pause_state_ ? 1.0f : 0.80f,
            last_pause_state_ ? 0.78f : 0.80f,
            last_pause_state_ ? 0.32f : 0.80f);

        // Bottom: the progress bar. While seeking (a held button or a finger) it and the clock
        // preview where the seek lands.
        const bool scrubbing = scrub_target_ >= 0.0;
        const double shown_pos = scrubbing ? scrub_target_ : last_time_pos_;
        const int bar_x = side;
        const int bar_width = width - side * 2;
        const int bar_height = scrubbing ? px(8) : px(5);
        const int bar_center = height - px(92);
        const int bar_y = bar_center - bar_height / 2;
        osd_font_.fill(bar_x, bar_y, bar_width, bar_height, width, height, 1.0f, 1.0f, 1.0f, 0.28f);
        if (active_is_live_) {
            fill_rect(bar_x, bar_y, bar_width, bar_height, height, 1.0f, 0.0f, 0.0f);
        } else if (last_duration_ > 1.0) {
            osd_bar_rect_ = {bar_x, bar_y, bar_width, bar_height};
            // How far the cache reaches, i.e. how far a seek can currently land.
            if (const auto buffered = buffered_ratio()) {
                osd_font_.fill(bar_x, bar_y, static_cast<int>(std::round(bar_width * *buffered)), bar_height, width,
                               height, 1.0f, 1.0f, 1.0f, 0.35f);
            }
            const int played_x =
                bar_x + static_cast<int>(std::round(bar_width * clamp_double(shown_pos / last_duration_, 0.0, 1.0)));
            fill_rect(bar_x, bar_y, played_x - bar_x, bar_height, height, 1.0f, 0.0f, 0.0f);
            // Chapter starts split the bar with small gaps, as on YouTube.
            for (size_t i = 1; i < chapters_.size(); i++) {
                const double at = clamp_double(chapters_[i].start / last_duration_, 0.0, 1.0);
                const int x = bar_x + static_cast<int>(std::round(bar_width * at));
                osd_font_.fill(x - px(1), bar_y, px(3), bar_height, width, height, 0.0f, 0.0f, 0.0f, 0.8f);
            }
            // The knob on the played end, bigger while seeking.
            const int knob = scrubbing ? px(22) : px(15);
            if (const unsigned int disc = knob_texture()) {
                osd_font_.draw_mask(disc, played_x - knob / 2, bar_center - knob / 2, knob, knob, width, height, 1.0f,
                                    0.0f, 0.0f, 1.0f);
            }
        }

        // Under the bar: the clock and the chapter on the left, the volume on the right.
        const int row_y = bar_center + px(18);
        const std::string clock = active_is_live_
            ? newpipe::tr("player/status/live")
            : format_playback_time(shown_pos) + " / " + format_playback_time(last_duration_);
        draw_text_line(side, row_y, meta_scale, height, clock, 1.0f, 1.0f, 1.0f);
        const int chapter = chapter_at(shown_pos);
        if (chapter >= 0) {
            draw_text_line(side + measure_text_width(clock, meta_scale), row_y, meta_scale, height,
                           "  •  " + clamp_text(osd_text(chapters_[chapter].title), 44), 0.82f, 0.82f, 0.82f);
        }
        const std::string volume = osd_text(
            newpipe::tr("player/osd/volume", static_cast<int>(std::lround(last_volume_))));
        draw_text_line(width - side - measure_text_width(volume, meta_scale), row_y, meta_scale, height, volume,
                       0.82f, 0.82f, 0.82f);

        // The button hints, dim, along the bottom edge.
        std::string hints = newpipe::tr("player/osd_controls");
        if (!chapters_.empty()) {
            hints += "  " + newpipe::tr("player/chapters/hint");
        }
        if (shorts_mode_) {
            hints += "  " + newpipe::tr("player/shorts/hint");
        }
        hints = clamp_text(osd_text(hints), 110);
        draw_text_line(std::max(side, (width - measure_text_width(hints, meta_scale)) / 2),
                       row_y + meta_scale * 9 + px(14), meta_scale, height, hints, 0.60f, 0.60f, 0.60f);
    }

    bool prepare_stream(std::string& error) {
        use_stream_bridge_ = false;
        active_local_media_path_.clear();
        active_external_audio_local_path_.clear();
        audio_download_done_.store(false);
        audio_download_success_.store(false);
        audio_download_abort_.store(false);
        audio_downloaded_bytes_.store(0);
        audio_prefetch_min_bytes_ = 0;
        audio_attach_attempted_ = false;
        audio_wait_logged_ = false;
        audio_download_error_.clear();
        stream_total_bytes_ = 0;
        audio_total_bytes_ = 0;
        active_url_ = request_.url;
        active_referer_ = request_.referer;
        active_http_header_fields_ = request_.http_header_fields;
        active_quality_label_.clear();
        active_audio_language_.clear();
        active_hls_bitrate_ = 0;
        active_external_audio_url_.clear();
        fallback_url_.clear();
        fallback_referer_.clear();
        fallback_http_header_fields_.clear();
        fallback_quality_label_.clear();
        fallback_external_audio_url_.clear();
        fallback_attempted_ = false;
        active_use_ump_ = false;
        active_is_live_ = false;

        if (!YouTubeResolver::is_youtube_url(request_.url)) {
            set_loading_status(
                newpipe::tr("player/loading/opening_direct_media"),
                newpipe::tr("player/loading/non_youtube_url"));
            return true;
        }

        YouTubeResolver resolver;
        const auto resolved = resolver.resolve(
            request_.url,
            error,
            [this](const std::string& title, const std::string& detail) {
                set_loading_status(translate_loading_text(title), translate_loading_text(detail));
            });
        if (!resolved.has_value()) {
            return false;
        }

        active_url_ = resolved->stream_url;
        active_referer_ = resolved->referer;
        active_http_header_fields_ = resolved->http_header_fields;
        active_quality_label_ = resolved->quality_label;
        active_audio_language_ = resolved->audio_language;
        active_hls_bitrate_ = resolved->hls_bitrate;
        active_loudness_lufs_ = resolved->loudness_lufs;
        captions_ = resolved->captions;
        original_language_ = resolved->audio_language;
        active_external_audio_url_ = resolved->external_audio_url;
        fallback_url_ = resolved->fallback_stream_url;
        fallback_referer_ = resolved->fallback_referer;
        fallback_http_header_fields_ = resolved->fallback_http_header_fields;
        fallback_quality_label_ = resolved->fallback_quality_label;
        fallback_external_audio_url_ = resolved->fallback_external_audio_url;
        hls_master_url_ = resolved->hls_master_url;
        master_retry_done_ = false;
        active_use_ump_ = resolved->use_ump;
        active_is_live_ = resolved->is_live;
        if (!resolved->playlist_body.empty()) {
#ifdef __SWITCH__
            active_local_media_path_ = newpipe::app_file_path("selected.m3u8");
#else
            active_local_media_path_ = "/tmp/ytb_player_selected.m3u8";
#endif
            std::remove(active_local_media_path_.c_str());
            if (!write_text_file(active_local_media_path_, resolved->playlist_body)) {
                error = "local HLS playlist write failed";
                return false;
            }
            active_url_ = local_playlist_url(active_local_media_path_);
            logf("player: wrote local playlist path=%s bytes=%zu",
                 active_local_media_path_.c_str(),
                 resolved->playlist_body.size());
        }

        std::string detail = active_quality_label_.empty()
            ? newpipe::tr("player/loading/direct_stream_ready")
            : active_quality_label_;
        if (active_is_live_) {
            detail += " " + newpipe::tr("player/status/live");
        }
        set_loading_status(newpipe::tr("player/loading/opening_media_stream"), detail);
        logf("player: resolved youtube url=%s hls_bitrate=%d", active_url_.c_str(), active_hls_bitrate_);
        if (!start_stream_bridge_if_needed(error)) {
            return false;
        }
        start_audio_prefetch_if_needed();
        return true;
    }

    bool start_stream_bridge_if_needed(std::string& error) {
        if (active_is_live_) {
            set_loading_status(
                newpipe::tr("player/loading/opening_live_stream"),
                newpipe::tr("player/loading/direct_playback"));
            return true;
        }

        if (is_manifest_url(active_url_)) {
            set_loading_status(
                newpipe::tr("player/loading/opening_media_stream"),
                newpipe::tr("player/loading/direct_dash_playback"));
            return true;
        }

        if (active_url_.rfind("http://", 0) != 0 && active_url_.rfind("https://", 0) != 0) {
            return true;
        }

        return start_stream_bridge(error);
    }

    void stop_stream_bridge() {
        stream_abort_.store(true);
        stream_cv_.notify_all();

        if (stream_download_thread_.joinable()) {
            stream_download_thread_.join();
        }

        {
            std::lock_guard<std::mutex> lock(stream_mutex_);
            if (stream_cache_fd_ >= 0) {
                ::close(stream_cache_fd_);
                stream_cache_fd_ = -1;
            }
            streamed_bytes_ = 0;
            stream_download_done_ = false;
            stream_download_success_ = false;
            stream_download_error_.clear();
        }
        stream_total_bytes_ = 0;

        if (!stream_cache_path_.empty()) {
            std::remove(stream_cache_path_.c_str());
        }

        stream_abort_.store(false);
        use_stream_bridge_ = false;
    }

    void update_download_progress(curl_off_t dltotal, curl_off_t dlnow) {
        const auto now = std::chrono::steady_clock::now();
        if (dlnow > 0 && now - last_progress_update_ < std::chrono::milliseconds(200)
            && (dltotal <= 0 || dlnow < dltotal)) {
            return;
        }

        last_progress_update_ = now;
        std::string detail;
        if (dltotal > 0) {
            detail = format_megabytes(static_cast<double>(dlnow))
                + " / " + format_megabytes(static_cast<double>(dltotal));
        } else {
            detail = format_megabytes(static_cast<double>(dlnow))
                + " " + newpipe::tr("player/loading/received");
        }
        set_loading_status(newpipe::tr("player/loading/downloading_video_data"), detail);
    }

    static std::string build_ranged_url(const std::string& base_url, const std::string& range, int rn) {
        std::string url = base_url;
        url += (url.find('?') == std::string::npos) ? "?" : "&";
        // NB: no "alr=yes". With alr=yes YouTube may answer a range request with
        // an HTTP 200 whose body is a plaintext redirect URL instead of media;
        // curl writes that URL straight into the stream cache and corrupts
        // playback. A plain range request returns the bytes (or a normal HTTP
        // redirect that curl follows via CURLOPT_FOLLOWLOCATION).
        url += "range=" + range + "&rn=" + std::to_string(rn);
        return url;
    }

    static bool perform_ump_request(
        CURL* curl,
        const std::string& base_url,
        uint64_t range_start,
        uint64_t range_end,
        int request_number,
        std::vector<uint8_t>& media_data,
        long& status_code) {
        if (!curl) {
            return false;
        }

        constexpr std::array<uint8_t, 2> kUmpRequestBody = {120, 0};
        std::string request_url = build_ump_request_url(
            base_url, range_start, range_end, request_number);
        for (int redirect_count = 0; redirect_count < 5; ++redirect_count) {
            CurlByteBuffer response;
            curl_easy_setopt(curl, CURLOPT_URL, request_url.c_str());
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, kUmpRequestBody.data());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(kUmpRequestBody.size()));
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &append_curl_bytes);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

            const CURLcode result = curl_easy_perform(curl);
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
            if (result != CURLE_OK || status_code < 200 || status_code >= 300) {
                logf("player: UMP request failed curl=%d status=%ld",
                     static_cast<int>(result), status_code);
                return false;
            }

            const auto parsed = parse_ump_response(response.bytes.data(), response.bytes.size());
            if (!parsed.complete) {
                logf("player: incomplete UMP envelope bytes=%zu", response.bytes.size());
                return false;
            }
            for (int protection_status : parsed.stream_protection_statuses) {
                logf("player: UMP stream protection status=%d", protection_status);
                if (protection_status >= 3) {
                    return false;
                }
            }
            if (!parsed.redirect_url.empty()) {
                request_url = merge_ump_redirect_parameters(parsed.redirect_url, request_url);
                continue;
            }
            if (parsed.media_data.empty()) {
                log_line("player: UMP response contained no media data");
                return false;
            }
            media_data = parsed.media_data;
            return true;
        }

        log_line("player: too many UMP redirects");
        return false;
    }

    bool perform_chunked_ranged_download(CURL* parent_curl, long& status_code) {
        (void)parent_curl;
        // 1 MiB keeps every request comfortably inside YouTube's un-throttled
        // initial burst window while halving the request count vs 512 KiB.
        constexpr uint64_t kChunkBytes = 1024 * 1024;
        constexpr int kMaxRetries = 5;
        const auto total_size = find_query_u64(active_url_, "clen");
        if (!total_size.has_value() || *total_size == 0) {
            log_line("player: ranged download missing clen");
            return false;
        }

        // Reuse a single keep-alive connection for every range request. Forcing
        // a fresh TLS handshake per chunk was far too slow on real hardware and
        // let playback outrun the download; each ranged request already resets
        // YouTube's throttle counter, so one connection stays un-throttled.
        const auto extra_headers = split_header_fields(active_http_header_fields_);
        CURL* ch = curl_easy_init();
        if (!ch) {
            log_line("player: ranged download curl init failed");
            return false;
        }
        struct curl_slist* ch_headers = nullptr;
        for (const auto& h : extra_headers) {
            ch_headers = curl_slist_append(ch_headers, h.c_str());
        }
        curl_easy_setopt(ch, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(ch, CURLOPT_MAXREDIRS, 10L);
        curl_easy_setopt(
            ch,
            CURLOPT_USERAGENT,
            active_use_ump_ ? kUmpDownloadUserAgent : kDownloadUserAgent);
        curl_easy_setopt(ch, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(ch, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(ch, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(ch, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(ch, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(ch, CURLOPT_HTTPHEADER, ch_headers);
        if (!active_use_ump_) {
            curl_easy_setopt(ch, CURLOPT_WRITEFUNCTION, &SwitchPlayer::on_stream_write);
            curl_easy_setopt(ch, CURLOPT_WRITEDATA, this);
        }
        if (!active_referer_.empty()) {
            curl_easy_setopt(ch, CURLOPT_REFERER, active_referer_.c_str());
        }

        bool completed = false;
        int rn = 0;
        while (!stream_abort_.load()) {
            size_t chunk_start = 0;
            {
                std::lock_guard<std::mutex> lock(stream_mutex_);
                chunk_start = streamed_bytes_;
            }

            if (chunk_start >= *total_size) {
                status_code = 206;
                completed = true;
                break;
            }

            const uint64_t chunk_end = std::min<uint64_t>(
                *total_size - 1, static_cast<uint64_t>(chunk_start) + kChunkBytes - 1);
            const std::string range = std::to_string(chunk_start) + "-" + std::to_string(chunk_end);
            const std::string chunk_url = build_ranged_url(active_url_, range, rn);
            update_download_progress(static_cast<curl_off_t>(*total_size), static_cast<curl_off_t>(chunk_start));

            bool chunk_ok = false;
            for (int retry = 0; retry < kMaxRetries && !stream_abort_.load(); ++retry) {
                if (retry > 0) {
                    logf("player: ranged chunk retry %d range=%s", retry, range.c_str());
                    std::this_thread::sleep_for(std::chrono::milliseconds(1000 * retry));
                }

                CURLcode result = CURLE_OK;
                if (active_use_ump_) {
                    std::vector<uint8_t> media_data;
                    if (perform_ump_request(
                            ch,
                            active_url_,
                            chunk_start,
                            chunk_end,
                            rn,
                            media_data,
                            status_code)
                        && on_stream_write(media_data.data(), 1, media_data.size(), this)
                            == media_data.size()) {
                        chunk_ok = true;
                        break;
                    }
                    result = CURLE_HTTP_RETURNED_ERROR;
                } else {
                    curl_easy_setopt(ch, CURLOPT_URL, chunk_url.c_str());
                    result = curl_easy_perform(ch);
                    curl_easy_getinfo(ch, CURLINFO_RESPONSE_CODE, &status_code);
                    if (result == CURLE_OK && (status_code == 206 || status_code == 200)) {
                        chunk_ok = true;
                        break;
                    }
                }
                logf("player: ranged chunk failed range=%s curl=%d status=%ld attempt=%d",
                     range.c_str(),
                     static_cast<int>(result),
                     status_code,
                     retry + 1);
            }

            if (!chunk_ok) {
                break;
            }

            size_t chunk_written = 0;
            {
                std::lock_guard<std::mutex> lock(stream_mutex_);
                chunk_written = streamed_bytes_ - chunk_start;
            }
            if (chunk_written == 0) {
                logf("player: ranged chunk empty range=%s", range.c_str());
                break;
            }
            ++rn;
        }

        curl_slist_free_all(ch_headers);
        curl_easy_cleanup(ch);
        return completed;
    }

    bool start_stream_bridge(std::string& error) {
        set_loading_status(
            newpipe::tr("player/loading/downloading_video_data"),
            newpipe::tr("player/loading/starting_transfer"));

#ifdef __SWITCH__
        stream_cache_path_ = newpipe::app_file_path("stream.cache");
#else
        stream_cache_path_ = "/tmp/ytb_player_stream.cache";
#endif

        std::remove(stream_cache_path_.c_str());
        stream_cache_fd_ = ::open(stream_cache_path_.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0666);
        if (stream_cache_fd_ < 0) {
            error = "stream cache file open failed";
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(stream_mutex_);
            streamed_bytes_ = 0;
            stream_download_done_ = false;
            stream_download_success_ = false;
            stream_download_error_.clear();
        }

        stream_total_bytes_ = find_query_u64(active_url_, "clen").value_or(0);
        stream_abort_.store(false);
        last_progress_update_ = std::chrono::steady_clock::time_point{};
        stream_download_thread_ = std::thread([this]() {
            CURL* curl = curl_easy_init();
            if (!curl) {
                std::lock_guard<std::mutex> lock(stream_mutex_);
                stream_download_done_ = true;
                stream_download_success_ = false;
                stream_download_error_ = "curl init failed";
                stream_cv_.notify_all();
                return;
            }

            struct curl_slist* header_list = nullptr;
            const auto extra_headers = split_header_fields(active_http_header_fields_);
            for (const auto& header : extra_headers) {
                header_list = curl_slist_append(header_list, header.c_str());
            }

            const bool is_googlevideo = contains_case_insensitive(active_url_, "googlevideo.com/videoplayback");
            const char* ua = is_googlevideo
                ? (active_use_ump_ ? kUmpDownloadUserAgent : kDownloadUserAgent)
                : kUserAgent;

            curl_easy_setopt(curl, CURLOPT_URL, active_url_.c_str());
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
            curl_easy_setopt(curl, CURLOPT_USERAGENT, ua);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
            curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &SwitchPlayer::on_stream_write);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, this);
            curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &SwitchPlayer::on_curl_progress);
            curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
            if (!active_referer_.empty()) {
                curl_easy_setopt(curl, CURLOPT_REFERER, active_referer_.c_str());
            }
            long status_code = 0;
            CURLcode result = CURLE_OK;
            const bool has_ratebypass = contains_case_insensitive(active_url_, "ratebypass=yes");
            if (is_googlevideo && !has_ratebypass) {
                log_line(active_use_ump_
                    ? "player: using tokenless Android VR UMP ranged download"
                    : "player: using chunked ranged download");
                const bool ok = perform_chunked_ranged_download(curl, status_code);
                result = ok ? CURLE_OK : CURLE_HTTP_RETURNED_ERROR;
            } else {
                if (has_ratebypass) {
                    log_line("player: using direct download (ratebypass)");
                }
                result = curl_easy_perform(curl);
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
            }
            curl_slist_free_all(header_list);
            curl_easy_cleanup(curl);

            {
                std::lock_guard<std::mutex> lock(stream_mutex_);
                stream_download_done_ = true;
                stream_download_success_ =
                    !stream_abort_.load() && result == CURLE_OK
                    && ((status_code >= 200 && status_code < 300) || status_code == 206);
                if (!stream_download_success_) {
                    logf("player: stream download failed url=%s curl=%d status=%ld",
                         active_url_.c_str(),
                         static_cast<int>(result),
                         status_code);
                    stream_download_error_ = stream_abort_.load()
                        ? "stream aborted"
                        : "video stream download failed";
                } else {
                    logf("player: stream download complete bytes=%zu", streamed_bytes_);
                }
                stream_cv_.notify_all();
            }
        });

        std::unique_lock<std::mutex> lock(stream_mutex_);
        while (streamed_bytes_ < kInitialStreamBufferBytes && !stream_download_done_) {
            stream_cv_.wait_for(lock, std::chrono::milliseconds(50));
        }

        if (streamed_bytes_ == 0 && stream_download_done_ && !stream_download_success_) {
            error = stream_download_error_.empty() ? "video stream download failed" : stream_download_error_;
            return false;
        }

        use_stream_bridge_ = true;
        set_loading_status(
            newpipe::tr("player/loading/opening_media_stream"),
            streamed_bytes_ >= kInitialStreamBufferBytes
                ? newpipe::tr("player/loading/initial_buffer_ready")
                : newpipe::tr("player/loading/download_complete"));
        return true;
    }

    bool download_url_to_file(
        const std::string& url,
        const std::string& path,
        const std::string& referer,
        const std::string& header_fields,
        std::atomic<bool>& abort_flag,
        std::atomic<size_t>& downloaded_bytes,
        std::string& error) {
        std::remove(path.c_str());
        const int fd = ::open(path.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0666);
        if (fd < 0) {
            error = "audio cache file open failed";
            return false;
        }

        FileDownloadContext download_context;
        download_context.fd = fd;
        download_context.progress = &downloaded_bytes;

        CURL* curl = curl_easy_init();
        if (!curl) {
            ::close(fd);
            std::remove(path.c_str());
            error = "audio curl init failed";
            return false;
        }

        struct curl_slist* header_list = nullptr;
        const auto extra_headers = split_header_fields(header_fields);
        for (const auto& header : extra_headers) {
            header_list = curl_slist_append(header_list, header.c_str());
        }

        const bool is_gv = contains_case_insensitive(url, "googlevideo.com/videoplayback");
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, is_gv ? kDownloadUserAgent : kUserAgent);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &write_download_chunk);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &download_context);
        if (!referer.empty()) {
            curl_easy_setopt(curl, CURLOPT_REFERER, referer.c_str());
        }

        long status_code = 0;
        CURLcode result = CURLE_OK;
        if (contains_case_insensitive(url, "googlevideo.com/videoplayback")) {
            const auto total_size = find_query_u64(url, "clen");
            if (!total_size.has_value() || *total_size == 0) {
                result = CURLE_HTTP_RETURNED_ERROR;
                error = "audio ranged download missing clen";
            } else {
                constexpr uint64_t kChunkBytes = 1024 * 1024;
                constexpr int kMaxRetries = 3;
                int rn = 0;
                while (!abort_flag.load()) {
                    if (download_context.offset >= *total_size) {
                        status_code = 206;
                        break;
                    }

                    const uint64_t chunk_start = download_context.offset;
                    const uint64_t chunk_end = std::min<uint64_t>(
                        *total_size - 1, chunk_start + kChunkBytes - 1);
                    const std::string range = std::to_string(chunk_start) + "-" + std::to_string(chunk_end);
                    const std::string chunk_url = build_ranged_url(url, range, rn);
                    curl_easy_setopt(curl, CURLOPT_URL, chunk_url.c_str());
                    curl_easy_setopt(curl, CURLOPT_RANGE, nullptr);

                    bool chunk_ok = false;
                    for (int retry = 0; retry < kMaxRetries && !abort_flag.load(); ++retry) {
                        if (retry > 0) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(500 * retry));
                            curl_easy_setopt(curl, CURLOPT_URL, chunk_url.c_str());
                        }
                        result = curl_easy_perform(curl);
                        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
                        if (result == CURLE_OK && (status_code == 206 || status_code == 200)) {
                            chunk_ok = true;
                            break;
                        }
                        logf("player: audio ranged chunk failed range=%s curl=%d status=%ld attempt=%d",
                             range.c_str(),
                             static_cast<int>(result),
                             status_code,
                             retry + 1);
                    }

                    if (!chunk_ok) {
                        error = "audio ranged chunk failed";
                        break;
                    }
                    if (download_context.offset <= chunk_start) {
                        result = CURLE_WRITE_ERROR;
                        error = "audio ranged chunk empty";
                        break;
                    }
                    ++rn;
                }
            }
        } else {
            result = curl_easy_perform(curl);
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
        }

        curl_slist_free_all(header_list);
        curl_easy_cleanup(curl);
        ::close(fd);

        const bool ok = !abort_flag.load()
            && result == CURLE_OK
            && status_code >= 200
            && status_code < 300;
        if (!ok) {
            if (error.empty()) {
                error = abort_flag.load() ? "audio download aborted" : "audio download failed";
            }
            std::remove(path.c_str());
            return false;
        }

        return true;
    }

    void start_audio_prefetch_if_needed() {
        const std::string source_url = active_external_audio_url_;
        const std::string source_referer = active_referer_;
        const std::string source_headers = active_http_header_fields_;
        const bool source_use_ump = active_use_ump_;
        if (source_url.empty()) {
            pending_external_audio_attach_ = false;
            return;
        }

#ifdef __SWITCH__
        audio_cache_path_ = newpipe::app_file_path("audio.cache");
#else
        audio_cache_path_ = "/tmp/ytb_player_audio.cache";
#endif

        std::remove(audio_cache_path_.c_str());
        {
            std::lock_guard<std::mutex> lock(audio_mutex_);
            audio_cache_fd_ = ::open(audio_cache_path_.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0666);
        }
        if (audio_cache_fd_ < 0) {
            pending_external_audio_attach_ = false;
            audio_download_done_.store(true);
            audio_download_success_.store(false);
            audio_download_error_ = "audio cache file open failed";
            return;
        }

        audio_total_bytes_ = find_query_u64(source_url, "clen").value_or(0);
        audio_prefetch_min_bytes_ = contains_case_insensitive(source_url, "googlevideo.com/videoplayback")
            ? 1024 * 1024
            : 0;
        pending_external_audio_attach_ = true;
        logf("player: start audio prefetch kind=split-audio url=%s", source_url.c_str());
        audio_download_thread_ = std::thread([this, source_url, source_referer, source_headers, source_use_ump]() {
            CURL* curl = curl_easy_init();
            if (!curl) {
                audio_download_success_.store(false);
                audio_download_error_ = "audio curl init failed";
                audio_download_done_.store(true);
                audio_cv_.notify_all();
                return;
            }

            struct curl_slist* header_list = nullptr;
            const auto extra_headers = split_header_fields(source_headers);
            for (const auto& header : extra_headers) {
                header_list = curl_slist_append(header_list, header.c_str());
            }

            const bool is_googlevideo_audio = contains_case_insensitive(source_url, "googlevideo.com/videoplayback");
            const char* audio_ua = is_googlevideo_audio
                ? (source_use_ump ? kUmpDownloadUserAgent : kDownloadUserAgent)
                : kUserAgent;

            curl_easy_setopt(curl, CURLOPT_URL, source_url.c_str());
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
            curl_easy_setopt(curl, CURLOPT_USERAGENT, audio_ua);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
            curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &SwitchPlayer::on_audio_stream_write);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, this);
            curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &SwitchPlayer::on_audio_curl_progress);
            curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
            if (!source_referer.empty()) {
                curl_easy_setopt(curl, CURLOPT_REFERER, source_referer.c_str());
            }

            long status_code = 0;
            CURLcode result = CURLE_OK;
            if (contains_case_insensitive(source_url, "googlevideo.com/videoplayback") && source_use_ump) {
                const auto total_size = find_query_u64(source_url, "clen");
                if (!total_size.has_value() || *total_size == 0) {
                    result = CURLE_HTTP_RETURNED_ERROR;
                    audio_download_error_ = "audio UMP download missing clen";
                } else {
                    constexpr uint64_t kChunkBytes = 1024 * 1024;
                    constexpr int kMaxRetries = 5;
                    int rn = 0;
                    while (!audio_download_abort_.load()) {
                        const size_t chunk_start = audio_downloaded_bytes_.load();
                        if (chunk_start >= *total_size) {
                            status_code = 200;
                            break;
                        }
                        const uint64_t chunk_end = std::min<uint64_t>(
                            *total_size - 1,
                            static_cast<uint64_t>(chunk_start) + kChunkBytes - 1);
                        bool chunk_ok = false;
                        for (int retry = 0; retry < kMaxRetries && !audio_download_abort_.load(); ++retry) {
                            if (retry > 0) {
                                std::this_thread::sleep_for(std::chrono::milliseconds(750 * retry));
                            }
                            std::vector<uint8_t> media_data;
                            if (perform_ump_request(
                                    curl,
                                    source_url,
                                    chunk_start,
                                    chunk_end,
                                    rn,
                                    media_data,
                                    status_code)
                                && on_audio_stream_write(media_data.data(), 1, media_data.size(), this)
                                    == media_data.size()) {
                                chunk_ok = true;
                                result = CURLE_OK;
                                break;
                            }
                            result = CURLE_HTTP_RETURNED_ERROR;
                        }
                        if (!chunk_ok) {
                            audio_download_error_ = "audio UMP ranged chunk failed";
                            break;
                        }
                        ++rn;
                    }
                }
                curl_slist_free_all(header_list);
                curl_easy_cleanup(curl);
                curl = nullptr;
                header_list = nullptr;
            } else if (contains_case_insensitive(source_url, "googlevideo.com/videoplayback")) {
                // Use fresh curl handle per chunk to avoid connection reuse issues
                // with YouTube CDN concurrent download limits.
                curl_slist_free_all(header_list);
                curl_easy_cleanup(curl);
                curl = nullptr;
                header_list = nullptr;

                const auto total_size = find_query_u64(source_url, "clen");
                if (!total_size.has_value() || *total_size == 0) {
                    result = CURLE_HTTP_RETURNED_ERROR;
                    audio_download_error_ = "audio ranged download missing clen";
                } else {
                    constexpr uint64_t kChunkBytes = 512 * 1024;
                    constexpr int kMaxRetries = 5;
                    int rn = 0;
                    while (!audio_download_abort_.load()) {
                        const size_t chunk_start = audio_downloaded_bytes_.load();
                        if (chunk_start >= *total_size) {
                            status_code = 206;
                            break;
                        }

                        const uint64_t chunk_end = std::min<uint64_t>(
                            *total_size - 1, static_cast<uint64_t>(chunk_start) + kChunkBytes - 1);
                        const std::string range = std::to_string(chunk_start) + "-" + std::to_string(chunk_end);
                        const std::string chunk_url = build_ranged_url(source_url, range, rn);

                        bool chunk_ok = false;
                        for (int retry = 0; retry < kMaxRetries && !audio_download_abort_.load(); ++retry) {
                            if (retry > 0) {
                                std::this_thread::sleep_for(std::chrono::milliseconds(1000 * retry));
                            }

                            CURL* ch = curl_easy_init();
                            if (!ch) break;
                            struct curl_slist* ch_headers = nullptr;
                            for (const auto& h : extra_headers) {
                                ch_headers = curl_slist_append(ch_headers, h.c_str());
                            }
                            curl_easy_setopt(ch, CURLOPT_URL, chunk_url.c_str());
                            curl_easy_setopt(ch, CURLOPT_FOLLOWLOCATION, 1L);
                            curl_easy_setopt(ch, CURLOPT_MAXREDIRS, 10L);
                            curl_easy_setopt(ch, CURLOPT_USERAGENT, audio_ua);
                            curl_easy_setopt(ch, CURLOPT_SSL_VERIFYPEER, 0L);
                            curl_easy_setopt(ch, CURLOPT_SSL_VERIFYHOST, 0L);
                            curl_easy_setopt(ch, CURLOPT_CONNECTTIMEOUT, 15L);
                            curl_easy_setopt(ch, CURLOPT_TIMEOUT, 30L);
                            curl_easy_setopt(ch, CURLOPT_NOSIGNAL, 1L);
                            curl_easy_setopt(ch, CURLOPT_HTTPHEADER, ch_headers);
                            curl_easy_setopt(ch, CURLOPT_WRITEFUNCTION, &SwitchPlayer::on_audio_stream_write);
                            curl_easy_setopt(ch, CURLOPT_WRITEDATA, this);
                            curl_easy_setopt(ch, CURLOPT_FRESH_CONNECT, 1L);
                            curl_easy_setopt(ch, CURLOPT_FORBID_REUSE, 1L);
                            if (!source_referer.empty()) {
                                curl_easy_setopt(ch, CURLOPT_REFERER, source_referer.c_str());
                            }

                            result = curl_easy_perform(ch);
                            curl_easy_getinfo(ch, CURLINFO_RESPONSE_CODE, &status_code);
                            curl_slist_free_all(ch_headers);
                            curl_easy_cleanup(ch);

                            if (result == CURLE_OK && (status_code == 206 || status_code == 200)) {
                                chunk_ok = true;
                                break;
                            }
                            logf("player: audio ranged chunk failed range=%s curl=%d status=%ld attempt=%d",
                                 range.c_str(),
                                 static_cast<int>(result),
                                 status_code,
                                 retry + 1);
                        }

                        if (!chunk_ok) {
                            audio_download_error_ = "audio ranged chunk failed";
                            break;
                        }
                        if (audio_downloaded_bytes_.load() <= chunk_start) {
                            result = CURLE_WRITE_ERROR;
                            audio_download_error_ = "audio ranged chunk empty";
                            break;
                        }
                        ++rn;
                    }
                }
            } else {
                result = curl_easy_perform(curl);
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
                curl_slist_free_all(header_list);
                curl_easy_cleanup(curl);
            }
            // curl and header_list already cleaned up above

            const bool ok = !audio_download_abort_.load()
                && result == CURLE_OK
                && ((status_code >= 200 && status_code < 300) || status_code == 206);
            audio_download_success_.store(ok);
            audio_download_error_ = ok
                ? std::string()
                : (!audio_download_error_.empty()
                    ? audio_download_error_
                    : (audio_download_abort_.load() ? "audio download aborted" : "audio stream download failed"));
            audio_download_done_.store(true);
            audio_cv_.notify_all();
            if (ok) {
                logf("player: audio prefetch complete bytes=%zu", audio_downloaded_bytes_.load());
            } else {
                logf("player: audio prefetch failed error=%s curl=%d status=%ld",
                     audio_download_error_.c_str(),
                     static_cast<int>(result),
                     status_code);
            }
        });
    }

    void stop_audio_prefetch() {
        audio_download_abort_.store(true);
        if (audio_download_thread_.joinable()) {
            audio_download_thread_.join();
        }
        {
            std::lock_guard<std::mutex> lock(audio_mutex_);
            if (audio_cache_fd_ >= 0) {
                ::close(audio_cache_fd_);
                audio_cache_fd_ = -1;
            }
        }
        audio_download_abort_.store(false);
        audio_download_done_.store(false);
        audio_download_success_.store(false);
        audio_downloaded_bytes_.store(0);
        audio_total_bytes_ = 0;
        audio_prefetch_min_bytes_ = 0;
        audio_attach_attempted_ = false;
        audio_wait_logged_ = false;
        audio_download_error_.clear();
        if (!audio_cache_path_.empty()) {
            std::remove(audio_cache_path_.c_str());
            audio_cache_path_.clear();
        }
    }

    void start_prepare() {
        prepare_error_.clear();
        prepare_done_.store(false);
        prepare_success_.store(false);

        prepare_thread_ = std::thread([this]() {
            std::string error;
            const bool ok = prepare_stream(error);
            if (!ok) {
                prepare_error_ = std::move(error);
            }

            prepare_success_.store(ok);
            prepare_done_.store(true);
        });
    }

    bool wait_for_prepare(std::string& error) {
        int phase = 0;
        const Uint32 started_at = SDL_GetTicks();

        while (!prepare_done_.load() || SDL_GetTicks() - started_at < 250) {
            render_loading_screen(phase);
            phase = (phase + 1) % 12;
            SDL_PumpEvents();
            SDL_Delay(33);
        }

        if (prepare_thread_.joinable()) {
            prepare_thread_.join();
        }

        if (!prepare_success_.load()) {
            error = prepare_error_.empty() ? "stream preparation failed" : prepare_error_;
            return false;
        }

        return true;
    }

    bool init_sdl(std::string& error) {
        log_line("player: SDL_Init");
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0) {
            error = std::string("SDL_Init failed: ") + SDL_GetError();
            return false;
        }

        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

#ifdef __SWITCH__
        const AppletOperationMode mode = appletGetOperationMode();
        int width = mode == AppletOperationMode_Console ? 1920 : 1280;
        int height = mode == AppletOperationMode_Console ? 1080 : 720;
#else
        int width = 1280;
        int height = 720;
#endif

#ifdef __SWITCH__
        const Uint32 window_flags = SDL_WINDOW_SHOWN;
#else
        // Desktop SDL needs to be told the window gets a GL context (the Switch port does not).
        const Uint32 window_flags = SDL_WINDOW_SHOWN | SDL_WINDOW_OPENGL;
#endif
        window_ = SDL_CreateWindow("YTB Player", 0, 0, width, height, window_flags);
        if (!window_) {
            error = std::string("SDL_CreateWindow failed: ") + SDL_GetError();
            return false;
        }
        SDL_ShowWindow(window_);

        gl_context_ = SDL_GL_CreateContext(window_);
        if (!gl_context_) {
            error = std::string("SDL_GL_CreateContext failed: ") + SDL_GetError();
            return false;
        }

        SDL_GL_MakeCurrent(window_, gl_context_);
        SDL_GL_SetSwapInterval(1);
        SDL_GameControllerEventState(SDL_ENABLE);

        for (int i = 0; i < SDL_NumJoysticks(); i++) {
            if (SDL_IsGameController(i)) {
                auto* controller = SDL_GameControllerOpen(i);
                if (controller) {
                    game_controllers_.push_back(controller);
                    has_game_controller_ = true;
                }
            } else {
                auto* joystick = SDL_JoystickOpen(i);
                if (joystick) {
                    joysticks_.push_back(joystick);
                }
            }
        }

        return true;
    }

    bool init_mpv(std::string& error) {
        std::setlocale(LC_NUMERIC, "C");
        mpv_ = mpv_create();
        if (!mpv_) {
            error = "mpv_create failed";
            return false;
        }

        mpv_set_option_string(mpv_, "vo", "libmpv");
        // The portlibs FFmpeg is built with the nvtegra (Tegra X1 NVDEC) hwaccel. libmpv has
        // no GL interop for it, so frames are copied back ("-copy"); when the device cannot
        // be opened mpv falls back to software decoding by itself. The upstream build had
        // no hwaccel at all ("auto-safe" found nothing), so every video was decoded on the
        // 1 GHz CPU cores.
        const bool hardware_decoding = newpipe::SettingsStore::instance().settings().hardware_decoding;
        mpv_set_option_string(mpv_, "hwdec", hardware_decoding ? "nvtegra-copy" : "no");
        mpv_set_option_string(mpv_, "profile", "sw-fast");
        // Keyframe seeks: a precise seek decodes up to a whole HLS segment of frames before
        // showing anything, which left the picture frozen behind the audio after a skip.
        mpv_set_option_string(mpv_, "hr-seek", "no");
        mpv_set_option_string(mpv_, "framedrop", "decoder+vo");
        if (!hardware_decoding) {
            mpv_set_option_string(mpv_, "vd-lavc-skiploopfilter", "nonref");
        }
        mpv_set_option_string(mpv_, "osc", "no");
        mpv_set_option_string(mpv_, "terminal", "no");
        mpv_set_option_string(mpv_, "config", "no");
        mpv_set_option_string(mpv_, "keep-open", "yes");
        // Otherwise the info bar shows the file name of the trimmed local playlist.
        if (!request_.title.empty()) {
            mpv_set_option_string(mpv_, "force-media-title", request_.title.c_str());
        }
#ifndef __SWITCH__
        // The test container has no sound device.
        if (const char* ao = std::getenv("NEWPIPE_MPV_AO")) {
            mpv_set_option_string(mpv_, "ao", ao);
        }
#endif
        mpv_set_option_string(mpv_, "speed", format_speed(g_playback_speed).c_str());
        loop_on_ = !video_id_.empty() && video_id_ == g_loop_video_id;
        if (loop_on_) {
            mpv_set_option_string(mpv_, "loop-file", "inf");
        }
        if (active_loudness_lufs_.has_value() && *active_loudness_lufs_ > kTargetLoudnessLufs + 0.5) {
            char gain[32];
            std::snprintf(gain, sizeof(gain), "%.1f", kTargetLoudnessLufs - *active_loudness_lufs_);
            if (mpv_set_option_string(mpv_, "volume-gain", gain) < 0) {
                const std::string filter = std::string("lavfi=[volume=") + gain + "dB]";
                mpv_set_option_string(mpv_, "af", filter.c_str());
            }
            logf("player: loudness %.1f LUFS, gain %s dB", *active_loudness_lufs_, gain);
        } else if (active_loudness_lufs_.has_value()) {
            logf("player: loudness %.1f LUFS, no gain needed", *active_loudness_lufs_);
        }
        if (resume_from_ > 0.0) {
            char start[32];
            std::snprintf(start, sizeof(start), "%.1f", resume_from_);
            mpv_set_option_string(mpv_, "start", start);
            logf("player: resume from %.1fs", resume_from_);
        }
        mpv_set_option_string(mpv_, "force-seekable", "no");
        mpv_set_option_string(mpv_, "ytdl", "no");
        mpv_set_option_string(mpv_, "tls-verify", "no");
        mpv_set_option_string(mpv_, "access-references", "yes");
        if (!active_local_media_path_.empty()) {
            // Never let mpv's own playlist parser take it: it plays only the first URL of an
            // "#EXTM3U" file, i.e. the video playlist without its audio rendition.
            mpv_set_option_string(mpv_, "demuxer", "lavf");
            mpv_set_option_string(mpv_, "demuxer-lavf-format", "hls");
            mpv_set_option_string(mpv_, "load-unsafe-playlists", "yes");
            mpv_set_option_string(
                mpv_, "demuxer-lavf-o", "protocol_whitelist=[file,http,https,tcp,tls,crypto,data,subfile]");
        }
        // The whitelist is [bracketed]: mpv splits key/value lists on commas, so the plain
        // form was rejected as a whole and FFmpeg kept "file,crypto,data" for a local
        // playlist, refusing its https streams (verified with mpv on a PC).
        const bool is_hls = contains_case_insensitive(active_quality_label_, "hls");
        if (is_hls) {
            const std::string hls_bitrate = active_hls_bitrate_ > 0
                ? std::to_string(active_hls_bitrate_)
                : std::string("max");
            mpv_set_option_string(mpv_, "hls-bitrate", hls_bitrate.c_str());
            // mpv takes the local playlist for a local file and reads only 1 s ahead, too little
            // over Wi-Fi; the network default (150 MiB) is too much for the Switch. 30 s ahead,
            // and 16 MiB behind so that short skips back play from memory.
            mpv_set_option_string(mpv_, "cache", "yes");
            mpv_set_option_string(mpv_, "cache-secs", "30");
            mpv_set_option_string(mpv_, "demuxer-max-bytes", "32MiB");
            mpv_set_option_string(mpv_, "demuxer-max-back-bytes", "16MiB");
            mpv_set_option_string(mpv_, "load-unsafe-playlists", "yes");
            mpv_set_option_string(
                mpv_, "demuxer-lavf-o",
                "protocol_whitelist=[file,http,https,tcp,tls,crypto,data,subfile]");
            // The visionOS HLS master offers original + AI-dubbed audio with no
            // DEFAULT; prefer the original language so mpv doesn't pick the dub.
            if (!active_audio_language_.empty()) {
                mpv_set_option_string(mpv_, "alang", active_audio_language_.c_str());
                logf("player: prefer audio language=%s", active_audio_language_.c_str());
            }
        }
        if (use_stream_bridge_) {
            mpv_set_option_string(
                mpv_, "demuxer-lavf-o", "protocol_whitelist=file,http,https,tcp,tls,crypto,data,switchcache");
        }
        // Use Android UA for YouTube CDN to avoid rejection
        // The trimmed local playlist points at googlevideo too: same agent as the full master.
        const bool is_youtube_stream = !active_local_media_path_.empty()
            || contains_case_insensitive(active_url_, "googlevideo.com")
            || contains_case_insensitive(active_url_, "youtube.com");
        mpv_set_option_string(mpv_, "user-agent",
            is_youtube_stream ? kDownloadUserAgent : kUserAgent);
        if (!active_referer_.empty()) {
            mpv_set_option_string(mpv_, "referrer", active_referer_.c_str());
        }
        if (!active_http_header_fields_.empty()) {
            mpv_set_option_string(mpv_, "http-header-fields", active_http_header_fields_.c_str());
        }

        if (use_stream_bridge_ && mpv_stream_cb_add_ro(mpv_, "switchcache", this, &SwitchPlayer::stream_open) < 0) {
            error = "mpv stream callback setup failed";
            return false;
        }

        if (mpv_initialize(mpv_) < 0) {
            error = "mpv_initialize failed";
            return false;
        }

        // info adds the decoder choice ("Using hardware decoding ...") and track selection
        // without the per-segment chatter.
        mpv_request_log_messages(mpv_, "info");
        mpv_set_wakeup_callback(mpv_, &SwitchPlayer::on_mpv_wakeup, this);

        mpv_opengl_init_params gl_init{};
        gl_init.get_proc_address = &SwitchPlayer::get_proc_address;
        gl_init.get_proc_address_ctx = nullptr;

        int advanced_control = 1;
        mpv_render_param params[] = {
            {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL)},
            {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init},
            {MPV_RENDER_PARAM_ADVANCED_CONTROL, &advanced_control},
            {MPV_RENDER_PARAM_INVALID, nullptr},
        };

        if (mpv_render_context_create(&render_context_, mpv_, params) < 0) {
            error = "mpv_render_context_create failed";
            return false;
        }

        mpv_render_context_set_update_callback(render_context_, &SwitchPlayer::on_render_update, this);
        render_update_pending_.store(true);
        return true;
    }

    void destroy_mpv() {
        if (render_context_) {
            mpv_render_context_set_update_callback(render_context_, nullptr, nullptr);
            mpv_render_context_free(render_context_);
            render_context_ = nullptr;
        }

        if (mpv_) {
            mpv_set_wakeup_callback(mpv_, nullptr, nullptr);
            mpv_destroy(mpv_);
            mpv_ = nullptr;
        }
    }

    bool load_file(std::string& error) {
        const std::string playback_url = use_stream_bridge_ ? std::string(kProtocolPrefix) + "playback" : active_url_;
        logf("player: loadfile url=%s", playback_url.c_str());
        const char* command[] = {"loadfile", playback_url.c_str(), nullptr};
        if (mpv_command(mpv_, command) < 0) {
            error = "mpv loadfile failed";
            return false;
        }

        load_started_at_ = std::chrono::steady_clock::now();
        file_loaded_ = false;
        pending_external_audio_attach_ = !active_external_audio_url_.empty();
        return true;
    }

    void attach_external_audio_if_needed() {
        if (!pending_external_audio_attach_ || audio_attach_attempted_ || audio_cache_fd_ < 0 || !mpv_) {
            return;
        }

        if (!audio_download_done_.load() && audio_downloaded_bytes_.load() < audio_prefetch_min_bytes_) {
            if (!audio_wait_logged_) {
                audio_wait_logged_ = true;
                logf("player: waiting for audio prefetch bytes=%zu need=%zu",
                     audio_downloaded_bytes_.load(),
                     audio_prefetch_min_bytes_);
            }
            return;
        }

        if (audio_download_done_.load() && !audio_download_success_.load()) {
            pending_external_audio_attach_ = false;
            audio_attach_attempted_ = true;
            logf("player: audio prefetch unavailable error=%s", audio_download_error_.c_str());
            show_osd_message(newpipe::tr("player/osd/audio_download_failed"), 2500);
            return;
        }

        pending_external_audio_attach_ = false;
        audio_attach_attempted_ = true;
        if (!first_frame_rendered_) {
            set_loading_status(
                newpipe::tr("player/loading/opening_media_stream"),
                newpipe::tr("player/loading/attaching_audio_track"));
        }
        const char* audio_url = "switchcache://audio";
        logf("player: add external audio url=%s", audio_url);
        const char* audio_command[] = {"audio-add", audio_url, "select", nullptr};
        if (mpv_command_async(mpv_, 0, audio_command) < 0) {
            log_line("player: mpv audio-add failed, continuing video-only");
            show_osd_message(newpipe::tr("player/osd/audio_attach_failed"), 2500);
            return;
        }

        log_line("player: external audio attach queued");
    }

    void maybe_attach_external_audio() {
        if (!pending_external_audio_attach_ || audio_attach_attempted_) {
            return;
        }
        attach_external_audio_if_needed();
    }

    // True when the active video stream (progressive / tokenless UMP direct URL)
    // finished downloading unsuccessfully after already delivering some bytes.
    // A tokenless UMP URL 403s mid-stream once the initial CDN burst is spent
    // (no PoToken on-device); mpv then stalls forever instead of ending, so we
    // watch this to trigger a graceful fallback to the reliable progressive
    // stream (e.g. 360p) even after playback has started.
    bool video_stream_failed_midstream() {
        if (!use_stream_bridge_ || stream_abort_.load()) {
            return false;
        }
        std::lock_guard<std::mutex> lock(stream_mutex_);
        return stream_download_done_ && !stream_download_success_ && streamed_bytes_ > 0;
    }

    bool retry_with_fallback(std::string& error) {
        // The trimmed local playlist failed to open: retry the full master (same streams,
        // slower to open) before dropping to the slower fallback stream.
        if (!master_retry_done_ && !hls_master_url_.empty() && !active_local_media_path_.empty()) {
            master_retry_done_ = true;
            log_line("player: trimmed HLS playlist failed, retrying the full master");
            destroy_mpv();
            std::remove(active_local_media_path_.c_str());
            active_local_media_path_.clear();
            active_url_ = hls_master_url_;
            if (!init_mpv(error) || !load_file(error)) {
                return false;
            }
            first_frame_rendered_ = false;
            file_loaded_ = false;
            terminal_error_.clear();
            mpv_events_pending_.store(true);
            render_update_pending_.store(true);
            return true;
        }
        if (fallback_attempted_ || fallback_url_.empty()) {
            return false;
        }

        fallback_attempted_ = true;
        logf("player: retry with fallback quality=%s url=%s",
             fallback_quality_label_.c_str(),
             fallback_url_.c_str());
        set_loading_status(
            newpipe::tr("player/loading/stream_720_failed"),
            newpipe::tr("player/loading/fallback_safe_stream"));

        destroy_mpv();
        stop_stream_bridge();
        stop_audio_prefetch();
        if (!active_local_media_path_.empty()) {
            std::remove(active_local_media_path_.c_str());
            active_local_media_path_.clear();
        }
        if (!active_external_audio_local_path_.empty()) {
            std::remove(active_external_audio_local_path_.c_str());
            active_external_audio_local_path_.clear();
        }

        active_url_ = fallback_url_;
        active_referer_ = fallback_referer_;
        active_http_header_fields_ = fallback_http_header_fields_;
        active_quality_label_ = fallback_quality_label_;
        active_hls_bitrate_ = 0;
        active_external_audio_url_ = fallback_external_audio_url_;
        active_use_ump_ = false;
        pending_external_audio_attach_ = false;
        active_is_live_ = false;

        if (!start_stream_bridge_if_needed(error)) {
            return false;
        }
        start_audio_prefetch_if_needed();
        if (!init_mpv(error)) {
            return false;
        }
        if (!load_file(error)) {
            return false;
        }

        first_frame_rendered_ = false;
        file_loaded_ = false;
        terminal_error_.clear();
        mpv_events_pending_.store(true);
        render_update_pending_.store(true);
        return true;
    }

    void loop() {
        bool running = true;
        bool paused = false;
        bool force_redraw = true;
        int loading_phase = 0;

        while (running) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_QUIT) {
                    running = false;
#ifndef __SWITCH__
                } else if (event.type == SDL_KEYDOWN && !event.key.repeat) {
                    handle_key(event.key.keysym.sym, running, paused, force_redraw);
#endif
                } else if (event.type == SDL_CONTROLLERBUTTONDOWN) {
                    handle_controller_button(event.cbutton.button, running, paused, force_redraw);
                } else if (event.type == SDL_CONTROLLERAXISMOTION) {
                    handle_trigger(event.caxis.axis, event.caxis.value, force_redraw);
                } else if (event.type == SDL_FINGERDOWN || event.type == SDL_FINGERMOTION || event.type == SDL_FINGERUP) {
                    // The first finger down is followed until it lifts; others are ignored.
                    if (event.type == SDL_FINGERDOWN && !touch_down_) {
                        touch_finger_ = event.tfinger.fingerId;
                    } else if (event.tfinger.fingerId != touch_finger_) {
                        continue;
                    }
                    int width = 0;
                    int height = 0;
                    SDL_GL_GetDrawableSize(window_, &width, &height);
                    handle_touch(event.type == SDL_FINGERDOWN ? TouchPhase::down
                                 : event.type == SDL_FINGERUP ? TouchPhase::up
                                                              : TouchPhase::move,
                                 static_cast<int>(event.tfinger.x * width), static_cast<int>(event.tfinger.y * height),
                                 paused, force_redraw);
                } else if ((event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP)
                           && event.button.which != SDL_TOUCH_MOUSEID && event.button.button == SDL_BUTTON_LEFT) {
                    // A real mouse (desktop tests); SDL's copies of touches are left out.
                    const auto [x, y] = window_to_drawable(event.button.x, event.button.y);
                    handle_touch(event.type == SDL_MOUSEBUTTONDOWN ? TouchPhase::down : TouchPhase::up, x, y, paused,
                                 force_redraw);
                } else if (event.type == SDL_MOUSEMOTION && event.motion.which != SDL_TOUCH_MOUSEID
                           && (event.motion.state & SDL_BUTTON_LMASK)) {
                    const auto [x, y] = window_to_drawable(event.motion.x, event.motion.y);
                    handle_touch(TouchPhase::move, x, y, paused, force_redraw);
                } else if (!has_game_controller_ && event.type == SDL_JOYBUTTONDOWN) {
                    handle_joy_button(event.jbutton.button, running, paused, force_redraw);
                } else if (!has_game_controller_ && event.type == SDL_JOYHATMOTION) {
                    handle_hat(event.jhat.value, force_redraw);
                }
            }

            update_shorts(running, force_redraw);
            if (restart_requested_) {
                restart_requested_ = false;
                reopening_ = true;
                queue_playback(request_);
                running = false;
            }
            if (first_frame_rendered_) {
                update_held_seek(force_redraw);
                log_playback_health();
                update_autoplay(running, force_redraw);
                update_sponsor_skip(force_redraw);
                announce_subtitles(force_redraw);
                take_chapters(force_redraw);
            }

            if (mpv_events_pending_.exchange(false) && !drain_mpv_events()) {
                running = false;
            }

            maybe_attach_external_audio();

            // Once the first frame is up, mpv no longer triggers the initial
            // fallback path. If the video stream then dies mid-download (e.g. a
            // tokenless UMP URL 403ing after the burst), switch to the reliable
            // progressive fallback and restart instead of freezing forever.
            if (first_frame_rendered_ && !fallback_attempted_ && !fallback_url_.empty()
                && video_stream_failed_midstream()) {
                std::string fallback_error;
                log_line("player: mid-stream download failure, falling back to safe stream");
                if (retry_with_fallback(fallback_error)) {
                    loading_phase = 0;
                    force_redraw = true;
                    continue;
                }
                if (!fallback_error.empty()) {
                    terminal_error_ = fallback_error;
                    logf("player: mid-stream fallback failed error=%s", fallback_error.c_str());
                    running = false;
                    continue;
                }
            }

            bool frame_ready = false;
            if (render_context_ && render_update_pending_.exchange(false)) {
                frame_ready = (mpv_render_context_update(render_context_) & MPV_RENDER_UPDATE_FRAME) != 0;
            }

            if (!first_frame_rendered_) {
                if (!file_loaded_ && !fallback_attempted_ && !fallback_url_.empty()
                    && std::chrono::steady_clock::now() - load_started_at_ > std::chrono::seconds(30)) {
                    std::string fallback_error;
                    log_line("player: load timeout before file-loaded, attempting fallback");
                    if (retry_with_fallback(fallback_error)) {
                        loading_phase = 0;
                        continue;
                    }
                    if (!fallback_error.empty()) {
                        terminal_error_ = fallback_error;
                        logf("player: fallback after timeout failed error=%s", fallback_error.c_str());
                        running = false;
                        continue;
                    }
                }

                if (frame_ready) {
                    render_frame();
                    first_frame_rendered_ = true;
                    if (char* hwdec = mpv_get_property_string(mpv_, "hwdec-current")) {
                        logf("player: hwdec-current=%s", hwdec);
                        mpv_free(hwdec);
                    }
                    refresh_osd_snapshot(true);
                    if (resume_from_ > 0.0) {
                        show_osd_message(
                            newpipe::tr("player/osd/resumed", format_playback_time(resume_from_)), 5000);
                    } else {
                        show_osd_message(newpipe::tr("player/osd/playback_ready"), 3500);
                    }
                    force_redraw = paused;
                    set_loading_status(
                        newpipe::tr("player/loading/playback_started"),
                        active_quality_label_.empty()
                            ? newpipe::tr("player/loading/video_frame_ready")
                            : clamp_text(osd_text(active_quality_label_), 40));
                    continue;
                }

                render_loading_screen(loading_phase);
                loading_phase = (loading_phase + 1) % 12;
                SDL_Delay(33);
                continue;
            }

            bool should_render = force_redraw || frame_ready || paused || osd_pinned_ || is_temporary_osd_visible();
            if (should_render) {
                render_frame();
                force_redraw = paused;
                if (!frame_ready) {
                    SDL_Delay(16);
                }
            } else {
                SDL_Delay(10);
            }
        }
    }

    void handle_controller_button(Uint8 button, bool& running, bool& paused, bool& force_redraw) {
        if (std::chrono::steady_clock::now() < player_input_ready_at_) {
            logf("player: ignored early controller button=%u", static_cast<unsigned int>(button));
            return;
        }
        if (!first_frame_rendered_ && button == SDL_CONTROLLER_BUTTON_A) {
            log_line("player: ignored pause during loading");
            return;
        }
        logf("player: controller button=%u", static_cast<unsigned int>(button));
        if (menu_open()) {
            switch (button) {
                case SDL_CONTROLLER_BUTTON_DPAD_UP:
                    menu_key(MenuKey::up, force_redraw);
                    break;
                case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
                    menu_key(MenuKey::down, force_redraw);
                    break;
                case SDL_CONTROLLER_BUTTON_B:  // physical A
                    menu_key(MenuKey::pick, force_redraw);
                    break;
                case SDL_CONTROLLER_BUTTON_A:  // physical B
                case SDL_CONTROLLER_BUTTON_START:
                    menu_key(MenuKey::close, force_redraw);
                    break;
                default:
                    break;
            }
            return;
        }
        switch (button) {
            case SDL_CONTROLLER_BUTTON_A:
                if (!cancel_autoplay(force_redraw)) {
                    running = false;
                }
                break;
            case SDL_CONTROLLER_BUTTON_B:
                if (!autoplay_counting_down()) {
                    toggle_pause(paused, force_redraw);
                } else {
                    autoplay_now_ = true;
                }
                break;
            // SDL names the Switch buttons by position: SDL X is the physical Y button.
            case SDL_CONTROLLER_BUTTON_X:
                cycle_speed(force_redraw);
                break;
            case SDL_CONTROLLER_BUTTON_Y:
                toggle_osd(force_redraw);
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_UP:
                if (shorts_mode_) {
                    shorts_step_ = -1;
                } else {
                    change_volume(5, force_redraw);
                }
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
                if (shorts_mode_) {
                    shorts_step_ = 1;
                } else {
                    change_volume(-5, force_redraw);
                }
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
                seek_relative(-kShortSeekSeconds, force_redraw);
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
                seek_relative(kShortSeekSeconds, force_redraw);
                break;
            case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
                seek_relative(-kLongSeekSeconds, force_redraw);
                break;
            case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
                seek_relative(kLongSeekSeconds, force_redraw);
                break;
            case SDL_CONTROLLER_BUTTON_BACK:  // "-"
                restart_from_beginning(force_redraw);
                break;
            case SDL_CONTROLLER_BUTTON_START:  // "+"
                open_settings_menu(force_redraw);
                break;
            default:
                break;
        }
    }

    // ZR opens (and closes) the chapter list. SDL's game controller API reports ZL/ZR as
    // trigger axes; a press is the value going past the middle.
    void handle_trigger(Uint8 axis, Sint16 value, bool& force_redraw) {
        if (axis != SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
            return;
        }
        const bool pressed = value > 16000;
        if (pressed && !zr_held_ && std::chrono::steady_clock::now() >= player_input_ready_at_) {
            toggle_chapter_menu(force_redraw);
        }
        zr_held_ = pressed;
    }

    // ---- touch: one finger at a time. A press on the progress bar drags the seek target (the
    // bar and the clock preview it) and seeks on release; any other press acts on release, as
    // a tap, when the finger stayed put. The desktop build feeds the mouse in here.
    enum class TouchPhase { down, move, up };

    void handle_touch(TouchPhase phase, int x, int y, bool& paused, bool& force_redraw) {
        if (!first_frame_rendered_ || std::chrono::steady_clock::now() < player_input_ready_at_) {
            touch_down_ = false;
            return;
        }
        if (phase == TouchPhase::down) {
            touch_down_ = true;
            touch_start_x_ = x;
            touch_start_y_ = y;
            // A finger is thicker than the bar: some room above and below it counts.
            const ScreenRect bar{osd_bar_rect_.x, osd_bar_rect_.y - 24, osd_bar_rect_.w, osd_bar_rect_.h + 48};
            touch_on_bar_ = !menu_open() && !autoplay_counting_down() && bar.contains(x, y) && is_media_seekable();
            if (touch_on_bar_) {
                touch_scrub(x, force_redraw);
            }
            return;
        }
        if (!touch_down_) {
            return;
        }
        if (phase == TouchPhase::move) {
            if (touch_on_bar_) {
                touch_scrub(x, force_redraw);
            }
            return;
        }
        touch_down_ = false;
        if (touch_on_bar_) {
            touch_on_bar_ = false;
            const double target = scrub_target_;
            scrub_target_ = -1.0;
            if (target >= 0.0) {
                logf("player: touch seek to %.1f", target);
                seek_relative(target - last_time_pos_, force_redraw);
            }
            return;
        }
        const int moved_y = y - touch_start_y_;
        if (shorts_mode_ && !menu_open() && std::abs(moved_y) >= kShortsSwipePixels
            && std::abs(moved_y) > std::abs(x - touch_start_x_)) {
            shorts_step_ = moved_y < 0 ? 1 : -1;  // the finger pushes the next one up
            return;
        }
        if (std::abs(x - touch_start_x_) <= 30 && std::abs(y - touch_start_y_) <= 30) {
            handle_tap(x, y, paused, force_redraw);
        }
    }

    // Window coordinates (mouse events) in the drawable's pixels, which the layout uses.
    std::pair<int, int> window_to_drawable(int x, int y) const {
        int window_width = 0;
        int window_height = 0;
        int drawable_width = 0;
        int drawable_height = 0;
        SDL_GetWindowSize(window_, &window_width, &window_height);
        SDL_GL_GetDrawableSize(window_, &drawable_width, &drawable_height);
        if (window_width <= 0 || window_height <= 0) {
            return {x, y};
        }
        return {x * drawable_width / window_width, y * drawable_height / window_height};
    }

    void touch_scrub(int x, bool& force_redraw) {
        const double fraction =
            clamp_double((x - osd_bar_rect_.x) / static_cast<double>(std::max(1, osd_bar_rect_.w)), 0.0, 1.0);
        scrub_target_ = fraction * std::max(0.0, last_duration_ - 1.0);
        reveal_osd(3000);
        force_redraw = true;
    }

    // Shows the info bar for a while, with the usual status line.
    void reveal_osd(int duration_ms) {
        osd_message_.clear();
        osd_visible_until_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(duration_ms);
    }

    void handle_tap(int x, int y, bool& paused, bool& force_redraw) {
        force_redraw = true;
        if (menu_open()) {
            if (menu_rows_rect_.contains(x, y)) {
                menu_selected_ = menu_first_row_ + (y - menu_rows_rect_.y) / std::max(1, menu_row_height_);
                menu_key(MenuKey::pick, force_redraw);
            } else if (!menu_rect_.contains(x, y)) {
                menu_key(MenuKey::close, force_redraw);
            }
            return;
        }
        if (autoplay_counting_down()) {
            if (autoplay_rect_.contains(x, y)) {
                autoplay_now_ = true;
            }
            return;
        }
        if (round_button_rect_.contains(x, y)) {
            toggle_pause(paused, force_redraw);
            return;
        }
        if (settings_button_rect_.contains(x, y)) {
            open_settings_menu(force_redraw);
            return;
        }
        if (osd_top_rect_.contains(x, y) || osd_bottom_rect_.contains(x, y)) {
            reveal_osd(4000);  // keeps the bar up while it is used
            return;
        }
        // Elsewhere a tap shows the info bar, or hides it when it is up (while paused it stays).
        if (osd_bottom_rect_.w > 0) {
            osd_pinned_ = false;
            osd_visible_until_ = {};
        } else {
            reveal_osd(4000);
        }
    }

    // The progress bar's knob, made once per player.
    unsigned int knob_texture() {
        if (!knob_texture_) {
            constexpr int kSize = 64;
            const auto coverage = make_disc_mask(kSize);
            knob_texture_ = osd_font_.create_mask(coverage.data(), kSize, kSize);
        }
        return knob_texture_;
    }

    // The round button's two faces, made once per player.
    unsigned int round_button_texture(bool pause) {
        unsigned int& texture = pause ? pause_button_texture_ : play_button_texture_;
        if (!texture) {
            constexpr int kSize = 128;
            const auto pixels = make_round_button(kSize, pause);
            texture = osd_font_.create_image(pixels.data(), kSize, kSize);
        }
        return texture;
    }

#ifndef __SWITCH__
    // Desktop build (screenshot tests): Escape exits, Space pauses, arrows seek (10 s) and
    // change the volume, q/e seek 60 s, x = info bar, y = speed, m = back to the start,
    // s = subtitle list, c = chapter list, o = settings list ("+").
    void handle_key(SDL_Keycode key, bool& running, bool& paused, bool& force_redraw) {
        if (std::chrono::steady_clock::now() < player_input_ready_at_) {
            return;
        }
        logf("player: key=%d", static_cast<int>(key));
        if (menu_open()) {
            switch (key) {
                case SDLK_UP:
                    menu_key(MenuKey::up, force_redraw);
                    break;
                case SDLK_DOWN:
                    menu_key(MenuKey::down, force_redraw);
                    break;
                case SDLK_RETURN:
                case SDLK_SPACE:
                    menu_key(MenuKey::pick, force_redraw);
                    break;
                case SDLK_ESCAPE:
                case SDLK_s:
                case SDLK_c:
                case SDLK_o:
                    menu_key(MenuKey::close, force_redraw);
                    break;
                default:
                    break;
            }
            return;
        }
        switch (key) {
            case SDLK_ESCAPE:
                if (!cancel_autoplay(force_redraw)) {
                    running = false;
                }
                break;
            case SDLK_SPACE:
                if (autoplay_counting_down()) {
                    autoplay_now_ = true;
                } else if (first_frame_rendered_) {
                    toggle_pause(paused, force_redraw);
                }
                break;
            case SDLK_LEFT:
                seek_relative(-kShortSeekSeconds, force_redraw);
                break;
            case SDLK_RIGHT:
                seek_relative(kShortSeekSeconds, force_redraw);
                break;
            case SDLK_UP:
                if (shorts_mode_) {
                    shorts_step_ = -1;
                } else {
                    change_volume(5, force_redraw);
                }
                break;
            case SDLK_DOWN:
                if (shorts_mode_) {
                    shorts_step_ = 1;
                } else {
                    change_volume(-5, force_redraw);
                }
                break;
            case SDLK_q:
                seek_relative(-kLongSeekSeconds, force_redraw);
                break;
            case SDLK_e:
                seek_relative(kLongSeekSeconds, force_redraw);
                break;
            case SDLK_x:
                toggle_osd(force_redraw);
                break;
            case SDLK_y:
                cycle_speed(force_redraw);
                break;
            case SDLK_m:
                restart_from_beginning(force_redraw);
                break;
            case SDLK_o:
                open_settings_menu(force_redraw);
                break;
            case SDLK_s:
                open_subtitle_menu(force_redraw);
                break;
            case SDLK_c:
                toggle_chapter_menu(force_redraw);
                break;
            default:
                break;
        }
    }
#endif

    void handle_joy_button(Uint8 button, bool& running, bool& paused, bool& force_redraw) {
        if (std::chrono::steady_clock::now() < player_input_ready_at_) {
            logf("player: ignored early joystick button=%u", static_cast<unsigned int>(button));
            return;
        }
        if (!first_frame_rendered_ && button == 0) {
            log_line("player: ignored joystick pause during loading");
            return;
        }
        logf("player: joystick button=%u", static_cast<unsigned int>(button));
        if (menu_open()) {
            switch (button) {
                case 13:
                    menu_key(MenuKey::up, force_redraw);
                    break;
                case 15:
                    menu_key(MenuKey::down, force_redraw);
                    break;
                case 1:  // the pause button
                    menu_key(MenuKey::pick, force_redraw);
                    break;
                case 0:  // the back button
                case 9:  // ZR
                case 10:
                    menu_key(MenuKey::close, force_redraw);
                    break;
                default:
                    break;
            }
            return;
        }
        switch (button) {
            case 0:
                if (!cancel_autoplay(force_redraw)) {
                    running = false;
                }
                break;
            case 1:
                if (!autoplay_counting_down()) {
                    toggle_pause(paused, force_redraw);
                } else {
                    autoplay_now_ = true;
                }
                break;
            case 2:
                toggle_osd(force_redraw);
                break;
            case 3:
                cycle_speed(force_redraw);
                break;
            case 13:
                change_volume(5, force_redraw);
                break;
            case 15:
                change_volume(-5, force_redraw);
                break;
            // HidNpadButton bit order: 6 L, 7 R, 12 Left, 14 Right.
            case 12:
                seek_relative(-kShortSeekSeconds, force_redraw);
                break;
            case 14:
                seek_relative(kShortSeekSeconds, force_redraw);
                break;
            case 6:
                seek_relative(-kLongSeekSeconds, force_redraw);
                break;
            case 7:
                seek_relative(kLongSeekSeconds, force_redraw);
                break;
            case 11:  // "-"
                restart_from_beginning(force_redraw);
                break;
            case 10:  // "+"
                open_subtitle_menu(force_redraw);
                break;
            case 9:  // ZR
                toggle_chapter_menu(force_redraw);
                break;
            default:
                break;
        }
    }

    // SDL reports one button-down per press. Holding left/right (or L/R) keeps seeking, with
    // bigger steps the longer it is held: 10 s steps, then 30 s, then 60 s (L/R: 60/180/360 s),
    // so the end of a long video is a few seconds away instead of dozens of presses.
    void update_held_seek(bool& force_redraw) {
        if (touch_on_bar_) {
            return;  // a finger drags the seek target; it seeks when lifted
        }
        int direction = 0;
        bool shoulder = false;
        for (auto* controller : game_controllers_) {
            if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) {
                direction = 1;
            } else if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) {
                direction = -1;
            } else if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) {
                direction = 1;
                shoulder = true;
            } else if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) {
                direction = -1;
                shoulder = true;
            }
            if (direction != 0) {
                break;
            }
        }
        if (direction == 0 && !has_game_controller_) {
            for (auto* joystick : joysticks_) {
                // HidNpadButton bit order: 6 L, 7 R, 12 Left, 14 Right.
                if (SDL_JoystickGetButton(joystick, 14)) {
                    direction = 1;
                } else if (SDL_JoystickGetButton(joystick, 12)) {
                    direction = -1;
                } else if (SDL_JoystickGetButton(joystick, 7)) {
                    direction = 1;
                    shoulder = true;
                } else if (SDL_JoystickGetButton(joystick, 6)) {
                    direction = -1;
                    shoulder = true;
                }
                if (direction != 0) {
                    break;
                }
            }
        }

        if (menu_open()) {
            direction = 0;  // the list takes the direction buttons
        }
        const auto now = std::chrono::steady_clock::now();
        if (direction == 0 || direction != held_seek_direction_ || shoulder != held_seek_shoulder_) {
            // Released (or changed direction) after scrubbing: seek once to the target.
            if (scrub_target_ >= 0.0) {
                const double target = scrub_target_;
                scrub_target_ = -1.0;
                seek_relative(target - last_time_pos_, force_redraw);
            }
            // A new press already seeked once through its button-down event.
            held_seek_direction_ = direction;
            held_seek_shoulder_ = shoulder;
            held_seek_since_ = now;
            held_seek_last_ = now;
            return;
        }
        const auto held = now - held_seek_since_;
        if (held < std::chrono::milliseconds(450) || now - held_seek_last_ < std::chrono::milliseconds(200)) {
            return;
        }
        held_seek_last_ = now;
        double step = shoulder ? kLongSeekSeconds : kShortSeekSeconds;
        if (held > std::chrono::milliseconds(1500)) {
            step *= 3.0;
        }
        if (held > std::chrono::milliseconds(3500)) {
            step *= 2.0;
        }
        // Scrub: only the target moves while the button is held. Seeking on every tick
        // queued one network seek per 200 ms, which stalled the picture for a long time.
        const double start = scrub_target_ >= 0.0 ? scrub_target_ : last_time_pos_;
        scrub_target_ = clamp_double(start + direction * step, 0.0, std::max(0.0, last_duration_ - 1.0));
        show_osd_message(
            newpipe::tr(
                "player/osd/seek",
                format_playback_time(scrub_target_),
                format_playback_time(last_duration_)),
            1500);
        force_redraw = true;
    }

    // Whether the decoder keeps up (drops, fps, avsync) and whether playback waits for the
    // network (wait, buf). Once a second for a while after a seek, otherwise every 30 s.
    void log_playback_health() {
        const auto now = std::chrono::steady_clock::now();
        const bool after_seek = now < seek_diag_until_;
        if (now - last_health_log_ < (after_seek ? std::chrono::seconds(1) : std::chrono::seconds(30))) {
            return;
        }
        last_health_log_ = now;
        auto text = [this](const char* name) {
            std::string value = get_mpv_property_text(name);
            return value.empty() ? std::string("-") : value;
        };
        logf("player: health pos=%s avsync=%s drop=%s/%s fps=%s wait=%s buf=%s codec=%s pause=%s eof=%s seeking=%s",
             text("time-pos").c_str(),
             text("avsync").c_str(),
             text("decoder-frame-drop-count").c_str(),
             text("frame-drop-count").c_str(),
             text("estimated-vf-fps").c_str(),
             text("paused-for-cache").c_str(),
             text("demuxer-cache-duration").c_str(),
             text("video-format").c_str(),
             text("pause").c_str(),
             text("eof-reached").c_str(),
             text("seeking").c_str());
    }

    // "-": back to the start, e.g. after a resume the viewer did not want.
    void restart_from_beginning(bool& force_redraw) {
        seek_relative(-(last_duration_ + 60.0), force_redraw);
    }

    // Where the viewer stopped, for the next open and the card bar (WatchProgressStore).
    void save_watch_progress() {
        if (video_id_.empty() || active_is_live_ || !mpv_ || !first_frame_rendered_) {
            return;
        }
        double position = last_time_pos_;
        double duration = last_duration_;
        double value = 0.0;
        if (mpv_get_property(mpv_, "time-pos", MPV_FORMAT_DOUBLE, &value) >= 0 && std::isfinite(value)) {
            position = value;
        }
        if (mpv_get_property(mpv_, "duration", MPV_FORMAT_DOUBLE, &value) >= 0 && std::isfinite(value)) {
            duration = value;
        }
        int eof = 0;
        if (mpv_get_property(mpv_, "eof-reached", MPV_FORMAT_FLAG, &eof) >= 0 && eof) {
            position = duration;
        }
        WatchProgressStore::instance().save(video_id_, position, duration);
        logf("player: progress video=%s pos=%.1f dur=%.1f", video_id_.c_str(), position, duration);
    }

    // ---- autoplay: in the last minute a worker picks the next video (the next item of the
    // playlist, else the first related video not watched to the end); at the end of the file a
    // countdown runs, then the player queues that video and closes, and main starts it at once.
    bool autoplay_counting_down() const {
        return autoplay_countdown_start_ != std::chrono::steady_clock::time_point{};
    }

    // Returns true when a countdown was running (and is now cancelled).
    bool cancel_autoplay(bool& force_redraw) {
        if (!autoplay_counting_down()) {
            return false;
        }
        autoplay_countdown_start_ = {};
        autoplay_cancelled_ = true;
        show_osd_message(newpipe::tr("player/autoplay/cancelled"), 2500);
        log_line("player: autoplay cancelled");
        force_redraw = true;
        return true;
    }

    // ---- Shorts (see ShortsQueue).
    void begin_shorts() {
        shorts_mode_ = true;
        ShortsQueue& queue = shorts_queue();
        const bool continuing = queue.index < queue.items.size() && queue.items[queue.index].id == video_id_;
        if (!continuing) {
            queue = {};
            StreamItem current;
            current.id = video_id_;
            current.url = request_.url;
            current.title = request_.title;
            queue.items.push_back(current);
            queue.more = request_.reel_sequence;
        }
        logf("player: shorts %zu/%zu more=%d", queue.index + 1, queue.items.size(), queue.more.empty() ? 0 : 1);
        // The Shorts from YouTube's sequence come without titles: "Shorts" until it is known
        // (mpv would name the stream's file).
        if (request_.title.empty()) {
            request_.title = "Shorts";
            osd_title_ = request_.title;
            start_shorts_title_fetch();
        }
        start_shorts_fetch();
    }

    // The next Shorts once the list is down to its last three.
    void start_shorts_fetch() {
        const ShortsQueue& queue = shorts_queue();
        if (shorts_fetch_ || queue.more.empty() || queue.index + 3 < queue.items.size()) {
            return;
        }
        shorts_fetch_ = std::make_shared<ShortsFetchState>();
        std::shared_ptr<ShortsFetchState> state = shorts_fetch_;
        const std::string params = queue.more;
        const std::atomic<bool>* abort = &background_abort_;
        start_background("shorts", [state, params, abort]() {
            HttpsHttpClient client;
            client.set_abort_flag(abort);
            YouTubeCatalogService service(&client);
            const auto page = service.get_shorts_sequence(params);
            std::lock_guard<std::mutex> lock(state->mutex);
            if (page.has_value()) {
                state->items = page->items;
                state->more = page->next_page_token;
            }
            state->done = true;
        });
    }

    void start_shorts_title_fetch() {
        shorts_title_ = std::make_shared<ShortsFetchState>();
        std::shared_ptr<ShortsFetchState> state = shorts_title_;
        // The watch page's details; a Short's own page is laid out differently.
        const std::string url = "https://www.youtube.com/watch?v=" + video_id_;
        const std::atomic<bool>* abort = &background_abort_;
        start_background("shorts title", [state, url, abort]() {
            HttpsHttpClient client;
            client.set_abort_flag(abort);
            YouTubeCatalogService service(&client);
            const auto detail = service.get_stream_detail(url);
            std::lock_guard<std::mutex> lock(state->mutex);
            if (detail.has_value()) {
                state->title = detail->item.title;
            }
            state->done = true;
        });
    }

    void update_shorts(bool& running, bool& force_redraw) {
        if (!shorts_mode_) {
            return;
        }
        ShortsQueue& queue = shorts_queue();
        // Shorts that came in join the list, without repeats; a failed request ends it.
        if (const std::shared_ptr<ShortsFetchState> state = shorts_fetch_) {
            std::unique_lock<std::mutex> lock(state->mutex);
            if (state->done) {
                size_t added = 0;
                for (auto& item : state->items) {
                    const bool known = std::any_of(queue.items.begin(), queue.items.end(),
                                                   [&item](const StreamItem& other) { return other.id == item.id; });
                    if (!known) {
                        queue.items.push_back(std::move(item));
                        added++;
                    }
                }
                queue.more = added > 0 ? state->more : std::string();
                logf("player: shorts added=%zu total=%zu more=%d", added, queue.items.size(),
                     queue.more.empty() ? 0 : 1);
                lock.unlock();
                shorts_fetch_.reset();
            }
        }
        if (const std::shared_ptr<ShortsFetchState> state = shorts_title_) {
            std::unique_lock<std::mutex> lock(state->mutex);
            if (state->done) {
                if (!state->title.empty()) {
                    request_.title = state->title;
                    osd_title_ = clamp_text(osd_text(state->title), 52);
                    if (mpv_) {
                        mpv_set_property_string(mpv_, "force-media-title", state->title.c_str());
                    }
                    if (queue.index < queue.items.size()) {
                        queue.items[queue.index].title = state->title;
                    }
                    force_redraw = true;
                }
                lock.unlock();
                shorts_title_.reset();
            }
        }
        // The end of a Short starts the next one.
        int eof = 0;
        if (first_frame_rendered_ && mpv_ && !shorts_ended_
            && mpv_get_property(mpv_, "eof-reached", MPV_FORMAT_FLAG, &eof) >= 0 && eof) {
            shorts_ended_ = true;
            shorts_step_ = 1;
        }
        if (shorts_waiting_ && !shorts_fetch_) {
            shorts_waiting_ = false;
            shorts_step_ = 1;
        }
        const int step = shorts_step_;
        shorts_step_ = 0;
        if (step == 0) {
            return;
        }
        if (step < 0) {
            if (queue.index == 0) {
                return;
            }
            queue.index--;
        } else if (queue.index + 1 < queue.items.size()) {
            queue.index++;
        } else {
            // The list ran out: go on once the next ones are here, if YouTube has more.
            start_shorts_fetch();
            if (shorts_fetch_) {
                shorts_waiting_ = true;
                show_osd_message(newpipe::tr("player/shorts/next_loading"));
                force_redraw = true;
            }
            return;
        }
        const StreamItem next = queue.items[queue.index];
        const auto request = build_playback_request(next, std::nullopt);
        if (!request.has_value()) {
            return;
        }
        std::string ignored_error;
        LibraryStore::instance().add_history(next, &ignored_error);
        queue_playback(*request);
        logf("player: shorts step %d to %zu/%zu id=%s", step, queue.index + 1, queue.items.size(), next.id.c_str());
        running = false;
    }

    // The thumbnail behind the loading screen: hq720 (1280x720) when the video has one, else
    // hqdefault (480x360) without its black bars.
    void start_loading_picture_fetch() {
        const auto id = YouTubeResolver::extract_video_id(request_.url);
        if (!id) {
            return;
        }
        loading_picture_ = std::make_shared<LoadingPictureState>();
        std::shared_ptr<LoadingPictureState> state = loading_picture_;
        const std::string video_id = *id;
        const std::atomic<bool>* abort = &background_abort_;
        start_background("loading picture", [state, video_id, abort]() {
            HttpsHttpClient client;
            client.set_abort_flag(abort);
            std::vector<unsigned char> pixels;
            int width = 0;
            int height = 0;
            for (const char* name : {"hq720.jpg", "hqdefault.jpg"}) {
                if (const auto body = client.get("https://i.ytimg.com/vi/" + video_id + "/" + name)) {
                    pixels = decode_picture(*body, width, height);
                }
                if (!pixels.empty()) {
                    break;
                }
            }
            if (!pixels.empty() && width * 3 == height * 4) {
                const int shown = width * 9 / 16;
                const size_t top = static_cast<size_t>((height - shown) / 2) * width * 4;
                pixels.erase(pixels.begin(), pixels.begin() + static_cast<std::ptrdiff_t>(top));
                pixels.resize(static_cast<size_t>(shown) * width * 4);
                height = shown;
            }
            logf("player: loading picture %dx%d", width, height);
            std::lock_guard<std::mutex> lock(state->mutex);
            state->pixels = std::move(pixels);
            state->width = width;
            state->height = height;
        });
    }

    void start_next_fetch() {
        if (next_state_ || video_id_.empty()) {
            return;
        }
        next_state_ = std::make_shared<NextVideoState>();
        std::shared_ptr<NextVideoState> state = next_state_;
        StreamItem current;
        current.id = video_id_;
        current.url = request_.url;
        current.title = request_.title;
        log_line("player: autoplay looking for the next video");
        const std::atomic<bool>* abort = &background_abort_;
        start_background("next video", [state, current, abort]() {
            HttpsHttpClient client;
            client.set_abort_flag(abort);
            YouTubeCatalogService service(&client);
            std::optional<StreamItem> pick;
            if (current.url.find("list=") != std::string::npos) {
                if (const auto feed = service.get_playlist_feed(current)) {
                    for (size_t i = 0; i + 1 < feed->items.size(); i++) {
                        if (YouTubeResolver::extract_video_id(feed->items[i].url) == current.id) {
                            pick = feed->items[i + 1];
                            break;
                        }
                    }
                }
            }
            if (!pick.has_value()) {
                if (const auto feed = service.get_related_feed(current)) {
                    for (const auto& item : feed->items) {
                        const auto id = YouTubeResolver::extract_video_id(item.url);
                        if (!id || *id == current.id || item.is_live
                            || WatchProgressStore::instance().watched_fraction(item) >= 1.0f) {
                            continue;
                        }
                        pick = item;
                        break;
                    }
                }
            }
            std::vector<unsigned char> thumbnail;
            int thumbnail_width = 0;
            int thumbnail_height = 0;
            if (pick.has_value()) {
                // mqdefault is 320x180, without the black bars of hqdefault.
                const auto id = YouTubeResolver::extract_video_id(pick->url);
                const std::string url = id ? "https://i.ytimg.com/vi/" + *id + "/mqdefault.jpg" : pick->thumbnail_url;
                if (!url.empty()) {
                    if (const auto body = client.get(url)) {
                        thumbnail = decode_picture(*body, thumbnail_width, thumbnail_height);
                    }
                }
            }
            logf("player: autoplay next=%s thumbnail=%dx%d", pick ? pick->url.c_str() : "none", thumbnail_width,
                 thumbnail_height);
            std::lock_guard<std::mutex> lock(state->mutex);
            state->item = pick;
            state->thumbnail = std::move(thumbnail);
            state->thumbnail_width = thumbnail_width;
            state->thumbnail_height = thumbnail_height;
            state->done = true;
        });
    }

    void update_autoplay(bool& running, bool& force_redraw) {
        if (!autoplay_enabled_ || shorts_mode_ || active_is_live_ || video_id_.empty() || !mpv_) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (!autoplay_now_ && now - autoplay_last_check_ < std::chrono::milliseconds(250)) {
            return;
        }
        autoplay_last_check_ = now;
        if (last_duration_ > 0.0 && last_time_pos_ > last_duration_ - 60.0) {
            start_next_fetch();
        }

        int eof = 0;
        if (mpv_get_property(mpv_, "eof-reached", MPV_FORMAT_FLAG, &eof) < 0 || !eof) {
            // Seeking back from the end stops the countdown and allows a new one later.
            if (autoplay_counting_down()) {
                autoplay_countdown_start_ = {};
                force_redraw = true;
            }
            autoplay_cancelled_ = false;
            return;
        }
        if (autoplay_cancelled_) {
            return;
        }
        start_next_fetch();
        std::optional<StreamItem> next;
        {
            std::lock_guard<std::mutex> lock(next_state_->mutex);
            if (!next_state_->done) {
                return;
            }
            next = next_state_->item;
        }
        if (!next.has_value()) {
            return;
        }
        if (!autoplay_counting_down()) {
            autoplay_countdown_start_ = now;
            autoplay_next_title_ = clamp_text(osd_text(next->title), 52);
        }
        force_redraw = true;
        if (!autoplay_now_ && now - autoplay_countdown_start_ < std::chrono::seconds(kAutoplayCountdownSeconds)) {
            return;
        }

        const auto request = build_playback_request(*next, std::nullopt);
        if (!request.has_value()) {
            autoplay_countdown_start_ = {};
            autoplay_cancelled_ = true;
            return;
        }
        std::string ignored_error;
        LibraryStore::instance().add_history(*next, &ignored_error);
        queue_playback(*request);
        logf("player: autoplay start url=%s", request->url.c_str());
        running = false;
    }

    void render_autoplay_panel(int width, int height) {
        autoplay_rect_ = {};
        if (!autoplay_counting_down()) {
            return;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - autoplay_countdown_start_).count();
        const int remaining = std::max(1, kAutoplayCountdownSeconds - static_cast<int>(elapsed));
        const int scale = std::max(2, height / 300);
        const std::string heading = newpipe::tr("player/autoplay/countdown", std::to_string(remaining));
        const std::string hint = newpipe::tr("player/autoplay/hint");
        const int line = scale * 9;

        if (!autoplay_thumbnail_ && next_state_) {
            std::lock_guard<std::mutex> lock(next_state_->mutex);
            if (!next_state_->thumbnail.empty()) {
                autoplay_thumbnail_ = osd_font_.create_image(
                    next_state_->thumbnail.data(), next_state_->thumbnail_width, next_state_->thumbnail_height);
                next_state_->thumbnail.clear();
            }
        }
        // The thumbnail (16:9) on the left, the three lines beside it.
        const int thumb_h = autoplay_thumbnail_ ? line * 6 : 0;
        const int thumb_w = thumb_h * 16 / 9;
        const int gap = autoplay_thumbnail_ ? 24 : 0;
        const int text_w = std::max({measure_text_width(autoplay_next_title_, scale), measure_text_width(heading, scale),
                                     measure_text_width(hint, scale)});
        const int text_h = line * 3 + 24;
        const int content_h = std::max(thumb_h, text_h);
        const int panel_w = std::min(width - 80, thumb_w + gap + text_w + 64);
        const int panel_h = content_h + 48;
        const int panel_x = (width - panel_w) / 2;
        const int panel_y = (height - panel_h) / 2;
        const int text_x = panel_x + 32 + thumb_w + gap;
        const int text_y = panel_y + 24 + (content_h - text_h) / 2;
        autoplay_rect_ = {panel_x, panel_y, panel_w, panel_h};
        osd_font_.fill(panel_x, panel_y, panel_w, panel_h, width, height, 0.05f, 0.05f, 0.05f, 0.92f);
        // The top edge fills up as the countdown runs.
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - autoplay_countdown_start_).count();
        const double progress = std::min(1.0, elapsed_ms / (kAutoplayCountdownSeconds * 1000.0));
        osd_font_.fill(panel_x, panel_y, panel_w, 4, width, height, 1.0f, 1.0f, 1.0f, 0.25f);
        fill_rect(panel_x, panel_y, static_cast<int>(panel_w * progress), 4, height, 0.96f, 0.26f, 0.21f);
        if (autoplay_thumbnail_) {
            GLint viewport[4] = {0, 0, 0, 0};
            glGetIntegerv(GL_VIEWPORT, viewport);
            osd_font_.draw_image(autoplay_thumbnail_, panel_x + 32, panel_y + 24 + (content_h - thumb_h) / 2, thumb_w,
                                 thumb_h, viewport[2], height);
        }
        draw_text_line(text_x, text_y, scale, height, heading, 0.96f, 0.96f, 0.96f);
        draw_text_line(text_x, text_y + line + 8, scale, height, autoplay_next_title_, 1.0f, 0.78f, 0.32f);
        draw_text_line(text_x, text_y + line * 2 + 24, scale, height, hint, 0.70f, 0.70f, 0.70f);
    }

    // Network lookups beside playback (sponsor segments, subtitles, the next video) run on
    // helper threads kept here. libnx cannot detach a thread: std::thread::detach throws
    // there and ended the app at every video. cleanup() cuts their requests short and joins.
    template <typename Work>
    void start_background(const char* what, Work work) {
        try {
            background_threads_.emplace_back(std::move(work));
        } catch (const std::exception& ex) {
            logf("player: %s thread not started: %s", what, ex.what());
        }
    }

    // ---- SponsorBlock: the segments come from a worker; a segment is skipped once, when
    // playback enters it, so seeking back into it plays it.
    void start_sponsor_fetch() {
        sponsor_state_ = std::make_shared<SponsorState>();
        std::shared_ptr<SponsorState> state = sponsor_state_;
        const std::string video_id = video_id_;
        const std::atomic<bool>* abort = &background_abort_;
        start_background("sponsor", [state, video_id, abort]() {
            HttpsHttpClient client;
            client.set_abort_flag(abort);
            auto segments = fetch_sponsor_segments(client, video_id);
            std::lock_guard<std::mutex> lock(state->mutex);
            state->segments = std::move(segments);
            state->done = true;
        });
    }

    void update_sponsor_skip(bool& force_redraw) {
        if (!sponsor_state_ || !mpv_ || scrub_target_ >= 0.0) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - sponsor_last_check_ < std::chrono::milliseconds(250)) {
            return;
        }
        sponsor_last_check_ = now;
        std::vector<SkipSegment> segments;
        {
            std::lock_guard<std::mutex> lock(sponsor_state_->mutex);
            if (!sponsor_state_->done || sponsor_state_->segments.empty()) {
                return;
            }
            segments = sponsor_state_->segments;
        }
        if (sponsor_skipped_.size() != segments.size()) {
            sponsor_skipped_.assign(segments.size(), false);
        }
        double position = 0.0;
        if (mpv_get_property(mpv_, "time-pos", MPV_FORMAT_DOUBLE, &position) < 0 || !std::isfinite(position)) {
            return;
        }
        for (size_t i = 0; i < segments.size(); i++) {
            const SkipSegment& segment = segments[i];
            if (sponsor_skipped_[i] || position < segment.start - 0.3 || position >= segment.end - 1.0) {
                continue;
            }
            sponsor_skipped_[i] = true;
            last_time_pos_ = position;
            seek_relative(segment.end - position, force_redraw);
            show_osd_message(
                newpipe::tr(
                    "player/osd/sponsor_skipped",
                    std::to_string(static_cast<int>(std::lround(segment.end - segment.start)))),
                3000);
            logf("player: sponsor skipped %s %.1f-%.1f at %.1f",
                 segment.category.c_str(), segment.start, segment.end, position);
            break;
        }
    }

    // ---- subtitles: YouTube's own tracks (json3), drawn with the overlay font. "+" opens a
    // list of the video's tracks; the pick (on/off and its language) is kept for the next videos.
    // The interface language; "auto" is the console's (see content_locale.hpp).
    std::string interface_language() const {
        const std::string language = SettingsStore::instance().settings().language;
        if (!language.empty() && language != "auto") {
            return language;
        }
        return content_hl();
    }

    // The language looked for first: the one last picked in the list, else the interface's.
    std::string subtitle_language() const {
        const std::string picked = SettingsStore::instance().settings().subtitle_language;
        return picked.empty() ? interface_language() : picked;
    }

    // A track's name in the list and messages: Turkish names in the Turkish interface,
    // YouTube's (English) names otherwise.
    std::string subtitle_track_label(const CaptionTrack& track) const {
        if (interface_language() == "tr") {
            const std::string name = turkish_language_name(track.language_code, std::string());
            if (name != track.language_code) {
                return track.auto_generated ? newpipe::tr("player/subtitles/auto_track", name) : name;
            }
        }
        return track.name.empty() ? track.language_code : track.name;
    }

    // Without a track the one pick_subtitle_track() prefers is fetched.
    void start_subtitle_fetch(std::optional<CaptionTrack> track = std::nullopt) {
        if (subtitle_state_ || video_id_.empty()) {
            return;
        }
        subtitle_state_ = std::make_shared<SubtitleState>();
        std::shared_ptr<SubtitleState> state = subtitle_state_;
        if (!track.has_value()) {
            track = pick_subtitle_track(captions_, subtitle_language(), original_language_);
        }
        if (!track.has_value()) {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->done = true;
            return;
        }
        const CaptionTrack chosen = *track;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->track = chosen;
        }
        const std::atomic<bool>* abort = &background_abort_;
        start_background("subtitles", [state, chosen, abort]() {
            HttpsHttpClient client;
            client.set_abort_flag(abort);
            auto cues = fetch_subtitle_cues(client, chosen);
            std::lock_guard<std::mutex> lock(state->mutex);
            state->cues = std::move(cues);
            state->done = true;
        });
    }

    void open_subtitle_menu(bool& force_redraw) {
        if (!first_frame_rendered_ || active_is_live_) {
            return;
        }
        force_redraw = true;
        if (captions_.empty()) {
            show_osd_message(newpipe::tr("player/subtitles/none"), 2500);
            return;
        }
        std::optional<CaptionTrack> current;
        if (subtitles_on_ && subtitle_state_) {
            std::lock_guard<std::mutex> lock(subtitle_state_->mutex);
            current = subtitle_state_->track;
        }
        std::vector<std::string> items{newpipe::tr("player/subtitles/menu_off")};
        int checked = 0;
        for (size_t i = 0; i < captions_.size(); i++) {
            items.push_back(subtitle_track_label(captions_[i]));
            if (current.has_value() && current->base_url == captions_[i].base_url) {
                checked = static_cast<int>(i) + 1;
            }
        }
        open_menu(MenuKind::subtitles, newpipe::tr("player/subtitles/menu_title"), std::move(items), checked);
    }

    // index into captions_, -1 for off.
    void choose_subtitles(int index, bool& force_redraw) {
        const bool on = index >= 0 && index < static_cast<int>(captions_.size());
        const std::string language =
            on ? captions_[index].language_code : SettingsStore::instance().settings().subtitle_language;
        std::string ignored_error;
        SettingsStore::instance().update_subtitles(on, language, &ignored_error);
        subtitles_on_ = on;
        subtitle_state_.reset();
        if (on) {
            logf("player: subtitles picked %s (%s)", captions_[index].language_code.c_str(),
                 captions_[index].auto_generated ? "auto" : "manual");
            subtitles_announced_ = false;
            start_subtitle_fetch(captions_[index]);
            show_osd_message(newpipe::tr("player/subtitles/loading"), 2000);
        } else {
            show_osd_message(newpipe::tr("player/subtitles/off"), 2000);
        }
        force_redraw = true;
    }

    // ---- chapters: YouTube's player-bar chapters, marked on the progress bar; ZR lists them.
    void start_chapter_fetch() {
        chapter_state_ = std::make_shared<ChapterState>();
        std::shared_ptr<ChapterState> state = chapter_state_;
        const std::string video_id = video_id_;
        const std::atomic<bool>* abort = &background_abort_;
        start_background("chapters", [state, video_id, abort]() {
            HttpsHttpClient client;
            client.set_abort_flag(abort);
            YouTubeCatalogService service(&client);
            auto chapters = service.get_chapters(video_id);
            std::lock_guard<std::mutex> lock(state->mutex);
            state->chapters = std::move(chapters);
            state->done = true;
        });
    }

    // Moves the chapters over once the helper has them.
    void take_chapters(bool& force_redraw) {
        if (!chapter_state_) {
            return;
        }
        std::vector<Chapter> chapters;
        {
            std::lock_guard<std::mutex> lock(chapter_state_->mutex);
            if (!chapter_state_->done) {
                return;
            }
            chapters = std::move(chapter_state_->chapters);
        }
        chapter_state_.reset();
        chapters_ = std::move(chapters);
        force_redraw = force_redraw || !chapters_.empty();
    }

    // The chapter playing at pos; -1 without chapters.
    int chapter_at(double pos) const {
        int index = -1;
        // A little slack: a jump lands on or just before the chapter's first second.
        for (size_t i = 0; i < chapters_.size() && chapters_[i].start <= pos + 1.0; i++) {
            index = static_cast<int>(i);
        }
        return index;
    }

    void toggle_chapter_menu(bool& force_redraw) {
        if (!first_frame_rendered_ || active_is_live_) {
            return;
        }
        force_redraw = true;
        if (menu_kind_ == MenuKind::chapters) {
            menu_key(MenuKey::close, force_redraw);
            return;
        }
        if (chapters_.empty()) {
            show_osd_message(
                newpipe::tr(chapter_state_ ? "player/chapters/loading" : "player/chapters/none"), 2500);
            return;
        }
        std::vector<std::string> items;
        for (const Chapter& chapter : chapters_) {
            items.push_back(format_playback_time(chapter.start) + "  " + chapter.title);
        }
        open_menu(MenuKind::chapters, newpipe::tr("player/chapters/menu_title"), std::move(items),
                  chapter_at(last_time_pos_));
    }

    void jump_to_chapter(int index, bool& force_redraw) {
        if (index < 0 || index >= static_cast<int>(chapters_.size())) {
            return;
        }
        const Chapter& chapter = chapters_[index];
        logf("player: chapter %d at %.1f", index, chapter.start);
        seek_relative(chapter.start - last_time_pos_, force_redraw);
        show_osd_message(newpipe::tr("player/chapters/jumped", clamp_text(osd_text(chapter.title), 40)), 2500);
    }

    // ---- the list shown over the video (subtitle tracks, chapters). Up/down move the highlight,
    // A picks, B or the button that opened it closes it; while it is up, the other buttons do nothing.
    enum class MenuKind { none, subtitles, chapters, settings, speed, quality };
    enum class MenuKey { up, down, pick, close };

    bool menu_open() const {
        return menu_kind_ != MenuKind::none;
    }

    void open_menu(MenuKind kind, std::string title, std::vector<std::string> items, int checked) {
        menu_kind_ = kind;
        menu_title_ = std::move(title);
        menu_items_ = std::move(items);
        menu_checked_ = checked;
        menu_selected_ = std::max(0, checked);
    }

    void menu_key(MenuKey key, bool& force_redraw) {
        force_redraw = true;
        const int count = static_cast<int>(menu_items_.size());
        if (key == MenuKey::up || key == MenuKey::down) {
            if (count > 0) {
                menu_selected_ = (menu_selected_ + (key == MenuKey::up ? count - 1 : 1)) % count;
            }
            return;
        }
        const MenuKind kind = menu_kind_;
        const int index = menu_selected_;
        menu_kind_ = MenuKind::none;
        menu_items_.clear();
        if (key == MenuKey::pick && kind == MenuKind::subtitles) {
            choose_subtitles(index - 1, force_redraw);  // row 0 is "off"
        } else if (key == MenuKey::pick && kind == MenuKind::chapters) {
            jump_to_chapter(index, force_redraw);
        } else if (key == MenuKey::pick && kind == MenuKind::settings) {
            pick_setting(index, force_redraw);
        } else if (key == MenuKey::pick && kind == MenuKind::speed) {
            set_speed(kMenuSpeeds[index], force_redraw);
        } else if (key == MenuKey::pick && kind == MenuKind::quality) {
            choose_quality(index, force_redraw);
        }
    }

    // ---- the settings list ("+"), as YouTube's player has it: each row names its current
    // choice and opens its own list; the loop row just switches.
    enum class SettingsRow { quality, speed, loop, subtitles, chapters };
    static constexpr double kMenuSpeeds[] = {0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0};
    static constexpr PlaybackQualityMode kMenuQualities[] = {
        PlaybackQualityMode::BEST, PlaybackQualityMode::HD_1080, PlaybackQualityMode::HD_720,
        PlaybackQualityMode::LOW_320};

    static std::string quality_name(PlaybackQualityMode mode) {
        switch (mode) {
            case PlaybackQualityMode::HD_1080:
                return newpipe::tr("settings/playback_quality/options/hd_1080");
            case PlaybackQualityMode::HD_720:
                return newpipe::tr("settings/playback_quality/options/hd_720");
            case PlaybackQualityMode::LOW_320:
                return newpipe::tr("settings/playback_quality/options/low_320");
            default:
                return newpipe::tr("player/settings/auto");  // docked 1080p, handheld 720p
        }
    }

    static std::string speed_name(double speed) {
        return std::fabs(speed - 1.0) < 0.01 ? newpipe::tr("player/settings/normal") : format_speed(speed) + "x";
    }

    void open_settings_menu(bool& force_redraw) {
        if (!first_frame_rendered_) {
            return;
        }
        force_redraw = true;
        settings_rows_.clear();
        std::vector<std::string> items;
        const auto add = [this, &items](SettingsRow row, const char* name, const std::string& value) {
            settings_rows_.push_back(row);
            items.push_back(newpipe::tr(name) + ":  " + value);
        };
        std::string quality = quality_name(SettingsStore::instance().settings().playback_quality);
        if (!active_quality_label_.empty()) {
            quality += "  (" + active_quality_label_ + ")";
        }
        add(SettingsRow::quality, "player/settings/quality", quality);
        add(SettingsRow::speed, "player/settings/speed", speed_name(g_playback_speed));
        if (!active_is_live_) {
            add(SettingsRow::loop, "player/settings/loop",
                newpipe::tr(loop_on_ ? "player/settings/on" : "player/settings/off"));
            std::string subtitles = newpipe::tr(captions_.empty() ? "player/settings/none" : "player/settings/off");
            if (subtitles_on_ && subtitle_state_) {
                std::lock_guard<std::mutex> lock(subtitle_state_->mutex);
                if (subtitle_state_->track.has_value()) {
                    subtitles = subtitle_track_label(*subtitle_state_->track);
                }
            }
            add(SettingsRow::subtitles, "player/settings/subtitles", subtitles);
        }
        if (!chapters_.empty()) {
            add(SettingsRow::chapters, "player/settings/chapters", std::to_string(chapters_.size()));
        }
        open_menu(MenuKind::settings, newpipe::tr("player/settings/title"), std::move(items), -1);
    }

    void pick_setting(int index, bool& force_redraw) {
        if (index < 0 || index >= static_cast<int>(settings_rows_.size())) {
            return;
        }
        switch (settings_rows_[index]) {
            case SettingsRow::quality: {
                const PlaybackQualityMode current = SettingsStore::instance().settings().playback_quality;
                std::vector<std::string> items;
                int checked = 0;
                for (size_t i = 0; i < std::size(kMenuQualities); i++) {
                    items.push_back(quality_name(kMenuQualities[i]));
                    if (kMenuQualities[i] == current) {
                        checked = static_cast<int>(i);
                    }
                }
                open_menu(MenuKind::quality, newpipe::tr("player/settings/quality"), std::move(items), checked);
                break;
            }
            case SettingsRow::speed: {
                std::vector<std::string> items;
                int checked = -1;
                for (size_t i = 0; i < std::size(kMenuSpeeds); i++) {
                    items.push_back(speed_name(kMenuSpeeds[i]));
                    if (std::fabs(kMenuSpeeds[i] - g_playback_speed) < 0.01) {
                        checked = static_cast<int>(i);
                    }
                }
                open_menu(MenuKind::speed, newpipe::tr("player/settings/speed"), std::move(items), checked);
                break;
            }
            case SettingsRow::loop:
                loop_on_ = !loop_on_;
                g_loop_video_id = loop_on_ ? video_id_ : std::string();
                if (mpv_) {
                    mpv_set_property_string(mpv_, "loop-file", loop_on_ ? "inf" : "no");
                }
                show_osd_message(newpipe::tr(loop_on_ ? "player/settings/loop_on" : "player/settings/loop_off"), 2000);
                logf("player: loop=%d", loop_on_ ? 1 : 0);
                break;
            case SettingsRow::subtitles:
                open_subtitle_menu(force_redraw);
                break;
            case SettingsRow::chapters:
                toggle_chapter_menu(force_redraw);
                break;
        }
        force_redraw = true;
    }

    void set_speed(double speed, bool& force_redraw) {
        g_playback_speed = speed;
        if (mpv_) {
            mpv_set_property(mpv_, "speed", MPV_FORMAT_DOUBLE, &g_playback_speed);
        }
        show_osd_message(newpipe::tr("player/osd/speed", format_speed(g_playback_speed)), 2000);
        logf("player: speed=%s", format_speed(g_playback_speed).c_str());
        force_redraw = true;
    }

    // Another quality means resolving the stream again: the player saves the place, closes
    // and opens the same video, which resumes there (the watch progress).
    void choose_quality(int index, bool& force_redraw) {
        if (index < 0 || index >= static_cast<int>(std::size(kMenuQualities))) {
            return;
        }
        const PlaybackQualityMode mode = kMenuQualities[index];
        if (mode == SettingsStore::instance().settings().playback_quality) {
            return;
        }
        std::string ignored_error;
        SettingsStore::instance().update_playback_quality(mode, &ignored_error);
        show_osd_message(newpipe::tr("player/settings/quality_changed", quality_name(mode)), 3000);
        logf("player: quality changed to %d, reopening", static_cast<int>(mode));
        restart_requested_ = true;
        force_redraw = true;
    }

    void render_menu(int width, int height) {
        menu_rect_ = {};
        menu_rows_rect_ = {};
        if (!menu_open() || menu_items_.empty()) {
            return;
        }
        const int scale = std::max(2, height / 300);
        const int line = scale * 9;
        const int row_h = line + 16;
        const int count = static_cast<int>(menu_items_.size());
        const int rows = std::min(count, std::max(3, (height - 240) / row_h));
        // The rows shown: a window that keeps the highlight in view.
        const int first = std::clamp(menu_selected_ - rows / 2, 0, count - rows);
        const std::string hint = osd_text(newpipe::tr("player/menu/hint"));
        const std::string title = osd_text(menu_title_);
        int text_w = std::max(measure_text_width(title, scale), measure_text_width(hint, scale));
        std::vector<std::string> labels;
        for (int i = first; i < first + rows; i++) {
            labels.push_back(clamp_text(osd_text(menu_items_[i]), 40));
            text_w = std::max(text_w, measure_text_width(labels.back(), scale) + 28);
        }
        const int panel_w = std::min(width - 80, std::max(width / 3, text_w + 64));
        const int panel_h = 20 + line + 16 + rows * row_h + 14 + line + 20;
        const int panel_x = width - panel_w - 40;
        const int panel_y = std::max(20, (height - panel_h) / 2);
        osd_font_.fill(panel_x, panel_y, panel_w, panel_h, width, height, 0.06f, 0.06f, 0.06f, 0.92f);
        fill_rect(panel_x, panel_y, panel_w, 4, height, 0.96f, 0.26f, 0.21f);
        draw_text_line(panel_x + 24, panel_y + 20, scale, height, title, 0.96f, 0.96f, 0.96f);
        int y = panel_y + 20 + line + 16;
        menu_rect_ = {panel_x, panel_y, panel_w, panel_h};
        menu_rows_rect_ = {panel_x, y, panel_w, rows * row_h};
        menu_first_row_ = first;
        menu_row_height_ = row_h;
        for (int i = first; i < first + rows; i++) {
            if (i == menu_selected_) {
                osd_font_.fill(panel_x + 12, y, panel_w - 24, row_h, width, height, 1.0f, 1.0f, 1.0f, 0.16f);
            }
            if (i == menu_checked_) {
                fill_rect(panel_x + 24, y + row_h / 2 - 4, 8, 8, height, 0.96f, 0.26f, 0.21f);
            }
            const float shade = i == menu_selected_ ? 1.0f : 0.80f;
            draw_text_line(panel_x + 24 + 28, y + 8, scale, height, labels[i - first], shade, shade, shade);
            y += row_h;
        }
        if (count > rows) {
            // Scroll bar: where the shown rows are in the whole list.
            const int track_y = panel_y + 20 + line + 16;
            const int track_h = rows * row_h;
            fill_rect(panel_x + panel_w - 10, track_y, 4, track_h, height, 0.20f, 0.20f, 0.20f);
            fill_rect(panel_x + panel_w - 10, track_y + track_h * first / count, 4,
                      std::max(8, track_h * rows / count), height, 0.70f, 0.70f, 0.70f);
        }
        draw_text_line(panel_x + 24, y + 14, scale, height, hint, 0.62f, 0.62f, 0.62f);
    }

    // Once the track is in: which language is shown, or that there is none.
    void announce_subtitles(bool& force_redraw) {
        if (!subtitles_on_ || subtitles_announced_ || !subtitle_state_) {
            return;
        }
        std::optional<CaptionTrack> track;
        size_t cue_count = 0;
        {
            std::lock_guard<std::mutex> lock(subtitle_state_->mutex);
            if (!subtitle_state_->done) {
                return;
            }
            track = subtitle_state_->track;
            cue_count = subtitle_state_->cues.size();
        }
        subtitles_announced_ = true;
        const std::string wanted = subtitle_language();
        if (!track.has_value() || cue_count == 0) {
            show_osd_message(newpipe::tr("player/subtitles/none"), 3000);
        } else {
            const std::string name = subtitle_track_label(*track);
            show_osd_message(
                same_language(track->language_code, wanted)
                    ? newpipe::tr("player/subtitles/on", name)
                    : newpipe::tr("player/subtitles/other", turkish_language_name(wanted, wanted), name),
                4000);
        }
        force_redraw = true;
    }

    void render_subtitles(int width, int height) {
        if (!subtitles_on_ || !subtitle_state_) {
            return;
        }
        std::vector<std::string> lines;
        {
            std::lock_guard<std::mutex> lock(subtitle_state_->mutex);
            if (!subtitle_state_->done) {
                return;
            }
            // Cues are sorted by start. Automatic tracks roll up: overlapping cues stack, and
            // only the newest two lines stay.
            const double now = last_time_pos_;
            for (const auto& cue : subtitle_state_->cues) {
                if (cue.start > now) {
                    break;
                }
                if (now >= cue.end) {
                    continue;
                }
                size_t begin = 0;
                while (begin <= cue.text.size()) {
                    const size_t end = cue.text.find('\n', begin);
                    const std::string line = cue.text.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
                    if (!line.empty()) {
                        lines.push_back(line);
                    }
                    if (end == std::string::npos) {
                        break;
                    }
                    begin = end + 1;
                }
            }
        }
        if (lines.empty()) {
            return;
        }
        if (lines.size() > 2) {
            lines.erase(lines.begin(), lines.end() - 2);
        }

        const int scale = std::max(3, height / 240);
        const int line_height = scale * 12;
        const int margin = std::max(20, width / 40);
        // Above the progress bar when the info bar is up, else near the bottom edge.
        int bottom = height - margin;
        if (should_draw_osd()) {
            bottom = std::min(bottom, height - static_cast<int>(std::lround(height * 122.0 / 720.0)));
        }
        int y = bottom - static_cast<int>(lines.size()) * line_height;
        for (const std::string& line : lines) {
            const std::string text = osd_text(line);
            const int text_width = std::min(measure_text_width(text, scale), width - 2 * margin);
            const int x = (width - text_width) / 2;
            osd_font_.fill(x - 12, y - 2, text_width + 24, line_height, width, height, 0.0f, 0.0f, 0.0f, 0.75f);
            draw_text_line(x, y + scale, scale, height, text, 1.0f, 1.0f, 1.0f);
            y += line_height;
        }
    }

    void cycle_speed(bool& force_redraw) {
        const size_t count = sizeof(kPlaybackSpeeds) / sizeof(kPlaybackSpeeds[0]);
        size_t next = 0;
        for (size_t i = 0; i < count; i++) {
            if (std::fabs(kPlaybackSpeeds[i] - g_playback_speed) < 0.01) {
                next = (i + 1) % count;
                break;
            }
        }
        set_speed(kPlaybackSpeeds[next], force_redraw);
    }

    void handle_hat(Uint8 hat_value, bool& force_redraw) {
        if (hat_value & SDL_HAT_UP) {
            change_volume(5, force_redraw);
        } else if (hat_value & SDL_HAT_DOWN) {
            change_volume(-5, force_redraw);
        } else if (hat_value & SDL_HAT_LEFT) {
            seek_relative(-kShortSeekSeconds, force_redraw);
        } else if (hat_value & SDL_HAT_RIGHT) {
            seek_relative(kShortSeekSeconds, force_redraw);
        }
    }

    void toggle_pause(bool& paused, bool& force_redraw) {
        if (!mpv_) {
            return;
        }

        const char* command[] = {"cycle", "pause", nullptr};
        mpv_command(mpv_, command);
        paused = !paused;
        last_pause_state_ = paused;
        show_osd_message(
            paused ? newpipe::tr("player/status/paused")
                   : newpipe::tr("player/status/playing"),
            3000);
        refresh_osd_snapshot(true);
        logf("player: pause toggled paused=%d", paused ? 1 : 0);
        force_redraw = true;
    }

    void change_volume(int delta, bool& force_redraw) {
        if (!mpv_) {
            return;
        }

        const std::string delta_text = std::to_string(delta);
        const char* command[] = {"add", "volume", delta_text.c_str(), nullptr};
        mpv_command(mpv_, command);
        last_volume_ = clamp_double(last_volume_ + static_cast<double>(delta), 0.0, 130.0);
        show_osd_message(
            newpipe::tr("player/osd/volume", static_cast<int>(std::lround(last_volume_))),
            2200);
        refresh_osd_snapshot(true);
        logf("player: volume delta=%d", delta);
        force_redraw = true;
    }

    // Fraction of the stream sitting in the cache files. Returns nothing when the
    // amount cannot be established, which keeps callers from guessing.
    std::optional<double> buffered_ratio() const {
        if (!use_stream_bridge_) {
            return std::nullopt;
        }

        size_t downloaded = 0;
        bool done = false;
        bool success = false;
        {
            std::lock_guard<std::mutex> lock(stream_mutex_);
            downloaded = streamed_bytes_;
            done = stream_download_done_;
            success = stream_download_success_;
        }

        double ratio = 1.0;
        if (!(done && success)) {
            if (stream_total_bytes_ == 0) {
                return std::nullopt;
            }
            ratio = static_cast<double>(downloaded) / static_cast<double>(stream_total_bytes_);
        }

        // A split audio track is downloaded on its own thread and can lag behind the
        // video, and a seek has to land inside both caches.
        if (!active_external_audio_url_.empty()) {
            if (audio_download_done_.load() && audio_download_success_.load()) {
                // fully cached, video ratio decides
            } else if (audio_total_bytes_ == 0) {
                return std::nullopt;
            } else {
                ratio = std::min(
                    ratio,
                    static_cast<double>(audio_downloaded_bytes_.load())
                        / static_cast<double>(audio_total_bytes_));
            }
        }

        return clamp_double(ratio, 0.0, 1.0);
    }

    // Last position a seek can currently reach, in seconds. Bytes are mapped onto
    // time linearly, which is only an approximation for a variable bitrate track but
    // errs on the conservative side often enough to be a useful guard. Returns
    // nothing when the whole stream is cached or the limit cannot be computed, in
    // which case seeking is left entirely to mpv.
    std::optional<double> buffered_seek_limit_seconds() const {
        if (last_duration_ <= 1.0) {
            return std::nullopt;
        }

        const auto ratio = buffered_ratio();
        if (!ratio.has_value() || *ratio >= 0.999) {
            return std::nullopt;
        }

        return last_duration_ * *ratio;
    }

    bool is_media_seekable() const {
        if (!mpv_) {
            return false;
        }

        int flag = 0;
        if (mpv_get_property(mpv_, "seekable", MPV_FORMAT_FLAG, &flag) < 0) {
            // Property not readable yet: let mpv reject the command itself instead of
            // refusing the input here.
            return true;
        }
        return flag != 0;
    }

    void seek_relative(double delta_seconds, bool& force_redraw) {
        if (!mpv_ || !first_frame_rendered_.load()) {
            return;
        }

        force_redraw = true;
        if (active_is_live_) {
            show_osd_message(newpipe::tr("player/osd/seek_live"), 2000);
            return;
        }

        refresh_osd_snapshot(true);
        if (last_duration_ <= 1.0 || !is_media_seekable()) {
            show_osd_message(newpipe::tr("player/osd/seek_unavailable"), 2000);
            return;
        }

        // Stop just short of the end so a forward seek cannot run into EOF.
        double target = clamp_double(
            last_time_pos_ + delta_seconds, 0.0, std::max(0.0, last_duration_ - 1.0));

        bool limited = false;
        if (const auto limit = buffered_seek_limit_seconds()) {
            // Stay a couple of seconds inside the buffered edge: the demuxer reads
            // ahead of the position it seeks to.
            const double safe_limit = std::max(0.0, *limit - 2.0);
            if (target > safe_limit) {
                target = safe_limit;
                limited = true;
            }

            if (delta_seconds > 0.0 && target <= last_time_pos_ + 0.5) {
                show_osd_message(
                    newpipe::tr("player/osd/seek_buffer_limit", format_playback_time(*limit)), 2500);
                logf("player: seek refused, buffered up to %.1fs pos=%.1fs", *limit, last_time_pos_);
                return;
            }
        }

        // A seek that would not move (pressing on at the end) is dropped: repeated ones there
        // left mpv stalled in the desktop tests.
        if (!limited && std::fabs(target - last_time_pos_) < 0.5) {
            show_osd_message(
                newpipe::tr("player/osd/seek", format_playback_time(target), format_playback_time(last_duration_)),
                1500);
            return;
        }
        // Relative, so that mpv seeks in that direction: with keyframe seeks (hr-seek=no) an
        // absolute target lands on the keyframe before it, and YouTube keyframes are about
        // 10 s apart, so +10 s often did not move at all (seen in the desktop tests). In the
        // last seconds there is no keyframe ahead to land on: absolute, and the end plays out.
        const bool absolute = limited || target > last_duration_ - 15.0;
        char target_text[32];
        std::snprintf(target_text, sizeof(target_text), "%.3f", absolute ? target : target - last_time_pos_);
        const char* command[] = {"seek", target_text, absolute ? "absolute" : "relative", nullptr};
        // Async: a seek on the cache bridge can block on file IO, and this runs on
        // the render thread.
        if (mpv_command_async(mpv_, 0, command) < 0) {
            show_osd_message(newpipe::tr("player/osd/seek_unavailable"), 2000);
            return;
        }

        last_time_pos_ = target;
        osd_time_hold_until_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(600);
        show_osd_message(
            newpipe::tr(
                "player/osd/seek",
                format_playback_time(target),
                format_playback_time(last_duration_)),
            2500);
        logf("player: seek delta=%.1f target=%.3f limited=%d",
             delta_seconds,
             target,
             limited ? 1 : 0);
        seek_diag_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(12);
        last_health_log_ = {};
    }

    void render_loading_screen(int phase) {
        int width = 1280;
        int height = 720;
        SDL_GL_GetDrawableSize(window_, &width, &height);

        glViewport(0, 0, width, height);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        if (osd_font_ready()) {
            render_loading_overlay(width, height);
        } else {
            render_loading_fallback(width, height, phase);
        }
        SDL_GL_SwapWindow(window_);
        glFinish();
    }

    // As YouTube opens a video: its thumbnail, darkened, behind a spinning ring; the title and
    // the channel under the ring, what is being done at the bottom.
    void render_loading_overlay(int width, int height) {
        OsdFont& font = osd_font_;
        if (!loading_texture_ && loading_picture_) {
            std::lock_guard<std::mutex> lock(loading_picture_->mutex);
            if (!loading_picture_->pixels.empty()) {
                loading_texture_ = font.create_image(loading_picture_->pixels.data(), loading_picture_->width,
                                                     loading_picture_->height);
                loading_picture_->pixels.clear();
                loading_texture_since_ = SDL_GetTicks();
            }
        }
        if (loading_texture_) {
            font.draw_image(loading_texture_, 0, 0, width, height, width, height);
            // Fades in over 300 ms; then dark enough for white text, darker still at the bottom.
            const float shown = std::min(1.0f, (SDL_GetTicks() - loading_texture_since_) / 300.0f);
            font.fill(0, 0, width, height, width, height, 0.0f, 0.0f, 0.0f, 1.0f - 0.34f * shown);
            font.fade(0, height / 2, width, height - height / 2, width, height, 0.0f, 0.0f, 0.0f, 0.8f, false);
        }

        const int size = std::max(40, height * 9 / 100);
        if (spinner_size_ != size) {
            spinner_masks_.clear();
            const float thickness = std::max(3.0f, size / 13.0f);
            for (int i = 0; i < kSpinnerSteps; i++) {
                const auto mask = make_arc_mask(size, thickness, 2.0f * kPi * i / kSpinnerSteps);
                spinner_masks_.push_back(font.create_mask(mask.data(), size, size));
            }
            spinner_size_ = size;
        }
        // One turn a second, clockwise.
        const size_t step = static_cast<size_t>(static_cast<uint64_t>(SDL_GetTicks()) * kSpinnerSteps / 1000)
                            % kSpinnerSteps;
        const int cx = width / 2;
        const int cy = height * 40 / 100;
        font.draw_mask(spinner_masks_[step], cx - size / 2, cy - size / 2, size, size, width, height, 1.0f, 1.0f,
                       1.0f, 0.95f);

        const int title_px = std::max(20, height * 44 / 1000);
        const int channel_px = std::max(16, height * 31 / 1000);
        const int status_px = std::max(14, height * 27 / 1000);
        const int max_width = width * 3 / 4;
        if (loading_lines_title_ != request_.title || loading_lines_width_ != max_width) {
            loading_lines_ = wrap_osd_title(font, request_.title, title_px, max_width);
            loading_lines_title_ = request_.title;
            loading_lines_width_ = max_width;
        }
        int y = cy + size / 2 + height * 7 / 100;
        for (const auto& line : loading_lines_) {
            font.draw(line, (width - font.measure(line, title_px)) / 2, y, title_px, width, height, 1.0f, 1.0f, 1.0f);
            y += title_px * 13 / 10;
        }
        if (!request_.channel.empty()) {
            const std::string channel = fit_osd_text(font, request_.channel, channel_px, max_width);
            font.draw(channel, (width - font.measure(channel, channel_px)) / 2, y + channel_px / 3, channel_px, width,
                      height, 0.72f, 0.72f, 0.72f);
        }

        // What is being done, and under it the step, dimmer (not when it only repeats the title).
        const auto [status, detail] = get_loading_status();
        const int status_y = height - height * 13 / 100;
        if (!status.empty()) {
            const std::string line = fit_osd_text(font, status, status_px, max_width);
            font.draw(line, (width - font.measure(line, status_px)) / 2, status_y, status_px, width, height, 0.8f, 0.8f,
                      0.8f);
        }
        if (!detail.empty() && detail != clamp_text(osd_text(request_.title), 40)) {
            const int detail_px = status_px * 85 / 100;
            const std::string line = fit_osd_text(font, detail, detail_px, max_width);
            font.draw(line, (width - font.measure(line, detail_px)) / 2, status_y + status_px * 3 / 2, detail_px, width,
                      height, 0.55f, 0.55f, 0.55f);
        }
    }

    // The bitmap font's screen (tests with NEWPIPE_OSD_BITMAP): a panel, dots, two lines.
    void render_loading_fallback(int width, int height, int phase) {
        const int panel_w = width * 2 / 3;
        // Tall enough for the TrueType status lines under the spinner.
        const int panel_h = height * 2 / 5;
        const int panel_x = (width - panel_w) / 2;
        const int panel_y = (height - panel_h) / 2;
        fill_rect(panel_x, panel_y, panel_w, panel_h, height, 0.10f, 0.10f, 0.10f);

        const int cx = width / 2;
        const int cy = height / 2 - height / 20;
        const int radius = std::min(width, height) / 12;
        const int dot_size = std::max(14, std::min(width, height) / 32);

        for (int i = 0; i < 12; i++) {
            const float angle = (static_cast<float>(i) / 12.0f) * 2.0f * kPi;
            const int x = static_cast<int>(std::round(cx + std::cos(angle) * radius)) - dot_size / 2;
            const int y = static_cast<int>(std::round(cy + std::sin(angle) * radius)) - dot_size / 2;
            const int active = (phase + 12 - i) % 12;
            const float shade = active == 0 ? 1.0f : std::max(0.25f, 0.88f - static_cast<float>(active) * 0.08f);
            fill_rect(x, y, dot_size, dot_size, height, shade, shade, shade);
        }

        const auto [title, detail] = get_loading_status();
        const int title_scale = std::max(3, height / 240);
        const int detail_scale = std::max(2, height / 320);
        const int title_y = cy + radius + dot_size + height / 30;
        const int detail_y = title_y + title_scale * 12;

        if (!title.empty()) {
            draw_text_line(
                (width - measure_text_width(title, title_scale)) / 2,
                title_y,
                title_scale,
                height,
                title,
                0.92f,
                0.92f,
                0.92f);
        }

        if (!detail.empty()) {
            draw_text_line(
                (width - measure_text_width(detail, detail_scale)) / 2,
                detail_y,
                detail_scale,
                height,
                detail,
                0.65f,
                0.65f,
                0.65f);
        }

    }

    bool drain_mpv_events() {
        while (true) {
            mpv_event* event = mpv_wait_event(mpv_, 0);
            if (!event || event->event_id == MPV_EVENT_NONE) {
                return true;
            }

            if (event->event_id == MPV_EVENT_LOG_MESSAGE) {
                const auto* message = static_cast<mpv_event_log_message*>(event->data);
                if (message && message->prefix && message->level && message->text) {
                    std::string text = message->text;
                    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
                        text.pop_back();
                    }
                    logf("player: mpv [%s] %s: %s",
                         message->level,
                         message->prefix,
                         text.c_str());
                }
                continue;
            }

            if (event->event_id == MPV_EVENT_START_FILE) {
                set_loading_status(
                    newpipe::tr("player/loading/opening_media_stream"),
                    use_stream_bridge_ ? newpipe::tr("player/loading/reading_stream_buffer")
                                       : newpipe::tr("player/loading/contacting_video_cdn"));
                continue;
            }

            if (event->event_id == MPV_EVENT_FILE_LOADED) {
                file_loaded_ = true;
                maybe_attach_external_audio();
                log_line("player: mpv file-loaded event");
                std::string detail = active_quality_label_.empty()
                    ? newpipe::tr("player/loading/buffering_first_frame")
                    : active_is_live_ ? active_quality_label_ + " " + newpipe::tr("player/status/live")
                                      : active_quality_label_;
                set_loading_status(newpipe::tr("player/loading/buffering_first_frame"), detail);
                continue;
            }

            if (event->event_id == MPV_EVENT_AUDIO_RECONFIG) {
                log_line("player: mpv audio-reconfig event");
                continue;
            }

            if (event->event_id == MPV_EVENT_END_FILE) {
                const auto* end_file = static_cast<mpv_event_end_file*>(event->data);
                std::string reason = end_file ? end_reason_to_string(end_file->reason) : "unknown";
                logf("player: mpv end-file reason=%s", reason.c_str());
                if (end_file && end_file->reason == MPV_END_FILE_REASON_REDIRECT) {
                    set_loading_status(
                        newpipe::tr("player/loading/opening_media_stream"),
                        newpipe::tr("player/loading/following_playlist_redirect"));
                    continue;
                }
                if (end_file && end_file->reason == MPV_END_FILE_REASON_ERROR) {
                    if (!first_frame_rendered_) {
                        std::string fallback_error;
                        if (retry_with_fallback(fallback_error)) {
                            continue;
                        }
                        if (!fallback_error.empty()) {
                            logf("player: fallback failed error=%s", fallback_error.c_str());
                        }
                    }
                    terminal_error_ = end_file->error != 0
                        ? std::string("mpv load failed: ") + mpv_error_string(end_file->error)
                        : "mpv load failed";
                }
                return false;
            }

            if (event->event_id == MPV_EVENT_SHUTDOWN) {
                if (terminal_error_.empty()) {
                    terminal_error_ = "mpv shutdown";
                }
                log_line("player: mpv shutdown");
                return false;
            }
        }
    }

    void render_frame() {
        int width = 1280;
        int height = 720;
        SDL_GL_GetDrawableSize(window_, &width, &height);

        glViewport(0, 0, width, height);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        mpv_opengl_fbo fbo{};
        fbo.fbo = 0;
        fbo.w = width;
        fbo.h = height;
        fbo.internal_format = 0;

        int flip_y = 1;
        mpv_render_param params[] = {
            {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
            {MPV_RENDER_PARAM_FLIP_Y, &flip_y},
            {MPV_RENDER_PARAM_INVALID, nullptr},
        };

        mpv_render_context_render(render_context_, params);
        render_subtitles(width, height);
        render_playback_osd(width, height);
        render_autoplay_panel(width, height);
        render_menu(width, height);
        SDL_GL_SwapWindow(window_);
        mpv_render_context_report_swap(render_context_);
    }

    void cleanup() {
        background_abort_.store(true);
        for (auto& thread : background_threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        background_threads_.clear();
        stop_stream_bridge();
        stop_audio_prefetch();
        destroy_mpv();

        if (gl_context_) {
            osd_font_.release();
            SDL_GL_MakeCurrent(window_, nullptr);
            SDL_GL_DeleteContext(gl_context_);
            gl_context_ = nullptr;
        }

        if (window_) {
            SDL_DestroyWindow(window_);
            window_ = nullptr;
        }

        if (prepare_thread_.joinable()) {
            prepare_thread_.join();
        }
        if (!active_local_media_path_.empty()) {
            std::remove(active_local_media_path_.c_str());
            active_local_media_path_.clear();
        }
        if (!active_external_audio_local_path_.empty()) {
            std::remove(active_external_audio_local_path_.c_str());
            active_external_audio_local_path_.clear();
        }

        for (auto* controller : game_controllers_) {
            SDL_GameControllerClose(controller);
        }
        game_controllers_.clear();

        for (auto* joystick : joysticks_) {
            SDL_JoystickClose(joystick);
        }
        joysticks_.clear();

        SDL_Quit();
#ifdef __SWITCH__
        appletSetMediaPlaybackState(false);
#endif
    }

    PlaybackRequest request_;
    SDL_Window* window_ = nullptr;
    SDL_GLContext gl_context_ = nullptr;
    mpv_handle* mpv_ = nullptr;
    mpv_render_context* render_context_ = nullptr;
    std::atomic<bool> mpv_events_pending_{false};
    std::atomic<bool> render_update_pending_{false};
    std::string terminal_error_;
    std::thread prepare_thread_;
    std::atomic<bool> prepare_done_{false};
    std::atomic<bool> prepare_success_{false};
    std::string prepare_error_;
    std::string active_url_;
    std::string active_referer_;
    std::string active_http_header_fields_;
    std::string active_quality_label_;
    std::string active_audio_language_;
    int active_hls_bitrate_ = 0;
    std::string active_external_audio_url_;
    std::string active_external_audio_local_path_;
    bool pending_external_audio_attach_ = false;
    std::thread audio_download_thread_;
    std::atomic<bool> audio_download_done_{false};
    std::atomic<bool> audio_download_success_{false};
    std::atomic<bool> audio_download_abort_{false};
    std::atomic<size_t> audio_downloaded_bytes_{0};
    std::string audio_download_error_;
    size_t audio_prefetch_min_bytes_ = 0;
    bool audio_attach_attempted_ = false;
    bool audio_wait_logged_ = false;
    std::string fallback_url_;
    std::string hls_master_url_;
    bool master_retry_done_ = false;
    std::string fallback_referer_;
    std::string fallback_http_header_fields_;
    std::string fallback_quality_label_;
    std::string fallback_external_audio_url_;
    bool active_use_ump_ = false;
    bool active_is_live_ = false;
    bool fallback_attempted_ = false;
    bool use_stream_bridge_ = false;
    bool file_loaded_ = false;
    // Read from the mpv render/event thread and the SDL input path, so keep it atomic.
    std::atomic<bool> first_frame_rendered_{false};
    bool osd_pinned_ = false;
    std::thread stream_download_thread_;
    std::string stream_cache_path_;
    std::string audio_cache_path_;
    std::string active_local_media_path_;
    int stream_cache_fd_ = -1;
    int audio_cache_fd_ = -1;
    mutable std::mutex stream_mutex_;
    mutable std::mutex audio_mutex_;
    std::condition_variable stream_cv_;
    std::condition_variable audio_cv_;
    size_t streamed_bytes_ = 0;
    // Total byte size of each cached track when the URL advertises it, used to map
    // downloaded bytes onto playable seconds for the seek guard and the OSD buffer
    // bar. Never handed to mpv, see stream_open.
    uint64_t stream_total_bytes_ = 0;
    uint64_t audio_total_bytes_ = 0;
    bool stream_download_done_ = false;
    bool stream_download_success_ = false;
    std::string stream_download_error_;
    std::atomic<bool> stream_abort_{false};
    std::vector<SDL_GameController*> game_controllers_;
    std::vector<SDL_Joystick*> joysticks_;
    bool has_game_controller_ = false;
    int held_seek_direction_ = 0;
    double scrub_target_ = -1.0;  // seek target while a seek button is held, -1 when idle
    OsdFont osd_font_;
    std::atomic<bool> background_abort_{false};
    std::vector<std::thread> background_threads_;
    std::string video_id_;        // YouTube id, for the watch progress
    std::optional<double> active_loudness_lufs_;
    std::vector<CaptionTrack> captions_;
    std::string original_language_;
    bool subtitles_on_ = false;
    bool subtitles_announced_ = false;
    std::shared_ptr<SubtitleState> subtitle_state_;
    // Touch (see handle_touch) and where the last frame drew things, for its hit tests.
    bool touch_down_ = false;
    bool touch_on_bar_ = false;
    SDL_FingerID touch_finger_ = 0;
    int touch_start_x_ = 0;
    int touch_start_y_ = 0;
    ScreenRect osd_top_rect_;
    ScreenRect osd_bottom_rect_;
    ScreenRect osd_bar_rect_;
    ScreenRect round_button_rect_;
    ScreenRect settings_button_rect_;
    ScreenRect menu_rect_;
    ScreenRect menu_rows_rect_;
    int menu_first_row_ = 0;
    int menu_row_height_ = 1;
    ScreenRect autoplay_rect_;
    unsigned int play_button_texture_ = 0;  // textures owned by osd_font_
    unsigned int pause_button_texture_ = 0;
    unsigned int knob_texture_ = 0;
    std::shared_ptr<ChapterState> chapter_state_;  // until take_chapters() moves them over
    std::vector<Chapter> chapters_;
    bool zr_held_ = false;
    MenuKind menu_kind_ = MenuKind::none;
    std::string menu_title_;
    std::vector<std::string> menu_items_;
    int menu_selected_ = 0;
    int menu_checked_ = -1;  // the current choice, marked in the list
    std::shared_ptr<SponsorState> sponsor_state_;
    std::vector<bool> sponsor_skipped_;
    std::chrono::steady_clock::time_point sponsor_last_check_{};
    bool autoplay_enabled_ = false;
    bool autoplay_cancelled_ = false;  // B during the countdown; reset by seeking away from the end
    bool autoplay_now_ = false;        // A during the countdown
    std::shared_ptr<NextVideoState> next_state_;
    std::vector<SettingsRow> settings_rows_;  // what each row of the settings list opens
    bool loop_on_ = false;
    bool restart_requested_ = false;  // another quality: reopen the video where it is
    bool reopening_ = false;          // this run ends for that reopen
    // Shorts (see ShortsQueue): -1/+1 asked by a button or a swipe, done by update_shorts().
    bool shorts_mode_ = false;
    int shorts_step_ = 0;
    bool shorts_waiting_ = false;  // the next Short was asked for before it came
    bool shorts_ended_ = false;
    std::shared_ptr<ShortsFetchState> shorts_fetch_;
    std::shared_ptr<ShortsFetchState> shorts_title_;
    std::string autoplay_next_title_;
    unsigned int autoplay_thumbnail_ = 0;  // texture owned by osd_font_
    std::chrono::steady_clock::time_point autoplay_countdown_start_{};
    std::chrono::steady_clock::time_point autoplay_last_check_{};
    double resume_from_ = 0.0;    // start position taken from the watch progress, 0 = start
    std::chrono::steady_clock::time_point last_health_log_{};
    std::chrono::steady_clock::time_point seek_diag_until_{};
    bool held_seek_shoulder_ = false;
    std::chrono::steady_clock::time_point held_seek_since_{};
    std::chrono::steady_clock::time_point held_seek_last_{};
    mutable std::mutex status_mutex_;
    std::string loading_title_;
    std::string loading_detail_;
    // The loading screen: the thumbnail (fetched, then a texture), the spinner's masks and
    // the title's lines as last laid out.
    std::shared_ptr<LoadingPictureState> loading_picture_;
    unsigned int loading_texture_ = 0;
    Uint32 loading_texture_since_ = 0;
    std::vector<unsigned int> spinner_masks_;
    int spinner_size_ = 0;
    std::vector<std::string> loading_lines_;
    std::string loading_lines_title_;
    int loading_lines_width_ = 0;
    std::chrono::steady_clock::time_point last_progress_update_{};
    std::chrono::steady_clock::time_point load_started_at_{};
    std::chrono::steady_clock::time_point osd_visible_until_{};
    std::chrono::steady_clock::time_point last_osd_refresh_{};
    // While a seek is in flight mpv keeps reporting the old time-pos for a moment,
    // so hold the OSD on the requested position instead of letting it snap back.
    std::chrono::steady_clock::time_point osd_time_hold_until_{};
    std::chrono::steady_clock::time_point player_input_ready_at_{};
    std::string osd_title_ = clamp_text(osd_text(request_.title), 52);
    std::string osd_message_;
    double last_time_pos_ = 0.0;
    double last_duration_ = 0.0;
    double last_volume_ = 100.0;
    bool last_pause_state_ = false;
};

}  // namespace

bool run_switch_player(const PlaybackRequest& request, std::string& error) {
    SwitchPlayer player(request);
    return player.run(error);
}

}  // namespace newpipe
