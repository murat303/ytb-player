#include "newpipe/log.hpp"

#include "newpipe/app_paths.hpp"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

namespace newpipe {
namespace {

std::mutex g_log_mutex;
FILE* g_log_file = nullptr;
std::chrono::steady_clock::time_point g_log_start;

std::string log_path() {
    return app_file_path("ytb-player.log");
}

// A hang usually ends with HOME -> close and a relaunch, which used to truncate the only log
// of the failing session. Keep the previous run next to the new one.
std::string previous_log_path() {
    return app_file_path("ytb-player.prev.log");
}

// The log lives on the SD card and a long session (thumbnail reloads, mpv warnings) used to
// grow it without bound. Past this size logging stops for the rest of the run.
constexpr long kMaxLogBytes = 4 * 1024 * 1024;

// Seconds since start on every line, so slow steps (opening a video, a seek) show as gaps.
void write_stamp_locked() {
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - g_log_start).count();
    std::fprintf(g_log_file, "%8.2f ", seconds);
}

void enforce_log_limit_locked() {
    if (g_log_file && std::ftell(g_log_file) > kMaxLogBytes) {
        std::fprintf(g_log_file, "=== log truncated (size limit) ===\n");
        std::fclose(g_log_file);
        g_log_file = nullptr;
    }
}

}  // namespace

void init_log() {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (g_log_file) {
        return;
    }

    std::remove(previous_log_path().c_str());
    std::rename(log_path().c_str(), previous_log_path().c_str());
    g_log_start = std::chrono::steady_clock::now();
    g_log_file = std::fopen(log_path().c_str(), "w");
    if (!g_log_file) {
        return;
    }

    std::fprintf(g_log_file, "=== YTB Player log start ===\n");
    std::fflush(g_log_file);
}

void shutdown_log() {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (!g_log_file) {
        return;
    }

    std::fprintf(g_log_file, "=== switch_newpipe log end ===\n");
    std::fflush(g_log_file);
    std::fclose(g_log_file);
    g_log_file = nullptr;
}

void log_line(const std::string& message) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (!g_log_file) {
        return;
    }

    write_stamp_locked();
    std::fprintf(g_log_file, "%s\n", message.c_str());
    std::fflush(g_log_file);
    enforce_log_limit_locked();
}

void logf(const char* format, ...) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (!g_log_file) {
        return;
    }

    write_stamp_locked();
    va_list args;
    va_start(args, format);
    std::vfprintf(g_log_file, format, args);
    va_end(args);
    std::fputc('\n', g_log_file);
    std::fflush(g_log_file);
    enforce_log_limit_locked();
}

}  // namespace newpipe
