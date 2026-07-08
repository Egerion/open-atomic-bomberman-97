#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace bomber::assets::res {

// The install's MESSAGES.TXT string table (the original's getstring /
// sub_4124A4): one entry per line, "<id>,<text>", with ';' full-line comments
// and blank lines skipped. Everything after the FIRST comma is the message text
// and is kept verbatim (it may contain commas and printf-style % specifiers,
// which sub_4518D0 == sprintf formats). The file is the user's own game data,
// loaded at runtime — never committed.
struct Messages {
    std::map<int, std::string> strings;   // id -> format string
    std::vector<std::string> warnings;    // lines that looked like data but didn't parse

    // getstring(id): the format string for id, or `fallback` if absent.
    std::string get_or(int id, const std::string& fallback = std::string()) const {
        auto it = strings.find(id);
        return it != strings.end() ? it->second : fallback;
    }
    const std::string* find(int id) const {
        auto it = strings.find(id);
        return it != strings.end() ? &it->second : nullptr;
    }
};

// Parse MESSAGES.TXT text (in-memory; hermetic for tests).
Messages parse_messages(std::string_view text);
// Load + parse MESSAGES.TXT from a path (throws std::runtime_error if unreadable).
Messages load_messages(const std::filesystem::path& path);

}  // namespace bomber::assets::res
