#pragma once

#include <string>

namespace newpipe {

// The language of what YouTube sends (section titles, dates, counts, the trending region) and
// of the few texts the service writes itself. It follows the app's language: Turkish, Korean,
// or English for any other.
enum class ContentLanguage { english, turkish, korean };

// From the app's locale ("tr", "ko", "en-US", "en-GB", "de", ...), once the UI knows it.
void set_content_language(const std::string& locale);
ContentLanguage content_language();

const char* content_hl();       // "en", "tr", "ko"
const char* content_gl();       // "US", "TR", "KR"
const char* accept_language();  // the Accept-Language header
std::string pref_cookie();      // "PREF=hl=..&gl=.."
int utc_offset_minutes();       // the console's time zone

// A text the service writes itself, in the content language; Korean gets the English one.
const char* localized(const char* turkish, const char* english);

}  // namespace newpipe
