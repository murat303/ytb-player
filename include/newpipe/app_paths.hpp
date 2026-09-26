#pragma once

#include <string>

namespace newpipe {

// The app keeps all of its files in one folder of its own: sdmc:/switch/ytb-player/ on the
// Switch (next to ytb-player.nro), ytb-player/ under the working folder elsewhere.
std::string app_file_path(const char* name);

// Creates that folder and copies in what Switch-NewPipe (and YTB Player before 1.1) kept loose
// in sdmc:/switch/ as switch_newpipe_*: settings, history, session. Runs before the log is
// opened; returns how many files it copied.
int prepare_app_folder();

}  // namespace newpipe
