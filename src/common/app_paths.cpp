#include "newpipe/app_paths.hpp"

#include <cstdio>
#include <fstream>
#include <sys/stat.h>

namespace newpipe {
namespace {

#ifdef __SWITCH__
constexpr const char* kAppFolder = "sdmc:/switch/ytb-player";
constexpr const char* kLegacyPrefix = "sdmc:/switch/switch_newpipe";
#else
constexpr const char* kAppFolder = "ytb-player";
constexpr const char* kLegacyPrefix = "switch_newpipe";
#endif

bool file_exists(const std::string& path) {
    struct stat info;
    return stat(path.c_str(), &info) == 0;
}

}  // namespace

std::string app_file_path(const char* name) {
    return std::string(kAppFolder) + "/" + name;
}

int prepare_app_folder() {
    mkdir(kAppFolder, 0777);

    // Copied, not moved: Switch-NewPipe itself (whose files these are) may still be in use.
    // The logs and download caches stay behind.
    static const struct {
        const char* legacy;
        const char* name;
    } kCopies[] = {
        {"_settings.json", "settings.json"},
        {"_library.json", "library.json"},
        {"_progress.json", "progress.json"},
        {"_session.json", "session.json"},
        {"_auth.txt", "auth.txt"},
    };
    int copied = 0;
    for (const auto& copy : kCopies) {
        const std::string from = std::string(kLegacyPrefix) + copy.legacy;
        const std::string to = app_file_path(copy.name);
        if (!file_exists(from) || file_exists(to)) {
            continue;
        }
        std::ifstream input(from, std::ios::binary);
        std::ofstream output(to, std::ios::binary | std::ios::trunc);
        output << input.rdbuf();
        if (input && output.good()) {
            copied++;
        } else {
            output.close();
            std::remove(to.c_str());
        }
    }
    return copied;
}

}  // namespace newpipe
