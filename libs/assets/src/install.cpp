#include "bomber/assets/install.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <string>

namespace bomber::assets {

namespace fs = std::filesystem;

namespace {

// Trim leading/trailing ASCII whitespace (incl. the '\r' on CRLF files).
std::string trim(std::string s) {
    auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    std::size_t b = 0, e = s.size();
    while (b < e && ws(s[b])) ++b;
    while (e > b && ws(s[e - 1])) --e;
    return s.substr(b, e - b);
}

bool iequals(const std::string& a, const char* b) {
    std::size_t i = 0;
    for (; i < a.size() && b[i]; ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return i == a.size() && b[i] == '\0';
}

}  // namespace

fs::path default_game_dir() {
    if (const char* env = std::getenv("BOMBER_GAME_DIR"); env && *env && fs::is_directory(env))
        return env;
    if (std::ifstream f("gamedir.txt"); f) {
        std::string line;
        if (std::getline(f, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (!line.empty() && fs::is_directory(line)) return line;
        }
    }
    for (const char* p : {"D:/Program Files (x86)/INTRPLAY/BOMBRMAN",
                          "C:/Program Files (x86)/INTRPLAY/BOMBRMAN", "./BOMBRMAN"}) {
        if (fs::is_directory(p)) return p;
    }
    return {};
}

Options load_options(const fs::path& path) {
    Options opts;
    std::ifstream f(path);
    if (!f) return opts;  // no file: every field stays empty (caller keeps defaults)

    // Mirror sub_406238: for each line, split on the FIRST '=' into key/value,
    // trim both, and match the key case-insensitively. ';'-comment and blank
    // lines have no '=' in the key position we care about and are skipped.
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));
        if (key.empty() || val.empty()) continue;
        if (iequals(key, "conveyor_speed"))
            opts.conveyor_speed = std::atoi(val.c_str());
        // Other keys are intentionally ignored (see Options doc).
    }
    return opts;
}

}  // namespace bomber::assets
