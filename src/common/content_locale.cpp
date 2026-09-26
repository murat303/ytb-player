#include "newpipe/content_locale.hpp"

#include <atomic>
#include <ctime>

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace newpipe {
namespace {

// Written once on the UI thread, read by the loaders' threads.
std::atomic<int> g_language{static_cast<int>(ContentLanguage::english)};
std::atomic<int> g_utc_offset_minutes{0};

int device_utc_offset_minutes() {
#ifdef __SWITCH__
    u64 now = 0;
    TimeCalendarTime calendar{};
    TimeCalendarAdditionalInfo info{};
    if (R_SUCCEEDED(timeGetCurrentTime(TimeType_Default, &now))
        && R_SUCCEEDED(timeToCalendarTimeWithMyRule(now, &calendar, &info))) {
        return info.offset / 60;
    }
    return 0;
#else
    const std::time_t now = std::time(nullptr);
    std::tm utc = *std::gmtime(&now);
    utc.tm_isdst = -1;
    // UTC's fields read as local time: the offset is how far that lands from now.
    return static_cast<int>(std::difftime(now, std::mktime(&utc)) / 60);
#endif
}

}  // namespace

void set_content_language(const std::string& locale) {
    ContentLanguage language = ContentLanguage::english;
    if (locale.rfind("tr", 0) == 0) {
        language = ContentLanguage::turkish;
    } else if (locale.rfind("ko", 0) == 0) {
        language = ContentLanguage::korean;
    }
    g_language.store(static_cast<int>(language));
    g_utc_offset_minutes.store(device_utc_offset_minutes());
}

ContentLanguage content_language() {
    return static_cast<ContentLanguage>(g_language.load());
}

const char* content_hl() {
    switch (content_language()) {
        case ContentLanguage::turkish:
            return "tr";
        case ContentLanguage::korean:
            return "ko";
        default:
            return "en";
    }
}

const char* content_gl() {
    switch (content_language()) {
        case ContentLanguage::turkish:
            return "TR";
        case ContentLanguage::korean:
            return "KR";
        default:
            return "US";
    }
}

const char* accept_language() {
    switch (content_language()) {
        case ContentLanguage::turkish:
            return "tr-TR,tr;q=0.9,en-US;q=0.8";
        case ContentLanguage::korean:
            return "ko-KR,ko;q=0.9,en-US;q=0.8";
        default:
            return "en-US,en;q=0.9";
    }
}

std::string pref_cookie() {
    return std::string("PREF=hl=") + content_hl() + "&gl=" + content_gl();
}

int utc_offset_minutes() {
    return g_utc_offset_minutes.load();
}

const char* localized(const char* turkish, const char* english) {
    return content_language() == ContentLanguage::turkish ? turkish : english;
}

}  // namespace newpipe
