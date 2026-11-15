// The diagnostic log, in memory and shareable, like every harmony client
// (CLAUDE.md). The last lines are kept in a ring; each line also goes to stderr
// and, when HARMONY_LOG_FILE is set, to that file (the bench reads it).
// Never a password, a cookie, an e-mail address or message text.
#pragma once
#include <string>
#include <vector>

void logf(const char* fmt, ...);
// Defense in depth: discard sensitive lines before any diagnostic sink.
std::string FilterLog(const std::string& text);
// The ring, oldest first, for the "copy the log" button and for crash reports.
std::vector<std::string> LogLines();
