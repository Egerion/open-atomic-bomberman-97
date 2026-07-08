#include "bomber/assets/install.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

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
        else if (iequals(key, "team_play"))
            opts.team_play = std::atoi(val.c_str()) != 0;
        // Other keys are intentionally ignored (see Options doc).
    }
    return opts;
}

void save_options(const fs::path& path, const Options& opts) {
    // Read every existing line verbatim (read-modify-write), so a hand-edited
    // file keeps its comments/ordering/unknown keys. Missing file -> start
    // from an empty line set (still yields a valid options.ini).
    std::vector<std::string> lines;
    if (std::ifstream in(path); in) {
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }
    }

    // For each key we own, find its line (same "first '=' splits key/value"
    // rule as the reader) and rewrite the value; otherwise remember to append.
    auto set_key = [&](const char* key, const std::string& value) {
        for (std::string& line : lines) {
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            if (!iequals(trim(line.substr(0, eq)), key)) continue;
            line = std::string(key) + "=" + value;
            return;
        }
        lines.push_back(std::string(key) + "=" + value);
    };

    if (opts.conveyor_speed) set_key("conveyor_speed", std::to_string(*opts.conveyor_speed));
    if (opts.team_play) set_key("team_play", *opts.team_play ? "1" : "0");

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("save_options: cannot write " + path.string());
    for (const std::string& line : lines) out << line << "\n";
}

}  // namespace bomber::assets
