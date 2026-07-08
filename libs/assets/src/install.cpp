#include "bomber/assets/install.hpp"

#include <cctype>
#include <cstdio>
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
    // Clamps mirror docs/re/results-and-options.md §3's table exactly.
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));
        if (key.empty() || val.empty()) continue;
        auto as_int = [&] { return std::atoi(val.c_str()); };
        auto as_bool = [&] { return as_int() != 0; };
        if (iequals(key, "levelno")) {
            int v = as_int();
            opts.levelno = v < -1 ? -1 : v;  // upper clamp needs getvalue(35), left to the consumer
        } else if (iequals(key, "num_to_win_match")) {
            int v = as_int();
            opts.num_to_win_match = v < 1 ? 1 : v;
        } else if (iequals(key, "enclosement_depth")) {
            int v = as_int();
            opts.enclosement_depth = v < 0 ? 0 : v;
        } else if (iequals(key, "conveyor_speed")) {
            opts.conveyor_speed = as_int();
        } else if (iequals(key, "team_play")) {
            opts.team_play = as_bool();
        } else if (iequals(key, "random_start")) {
            opts.random_start = as_bool();
        } else if (iequals(key, "stomped_bombs_detonate")) {
            opts.stomped_bombs_detonate = as_bool();
        } else if (iequals(key, "win_by_kills")) {
            opts.win_by_kills = as_bool();
        } else if (iequals(key, "goldman")) {
            opts.goldman = as_bool();
        } else if (iequals(key, "schemefilename")) {
            opts.schemefilename = val;
        } else if (iequals(key, "playtime")) {
            int v = as_int();
            if (v != 1001 && v < 60) v = 60;  // 1001 = the "unlimited" sentinel, never clamped
            opts.playtime = v;
        } else if (iequals(key, "assign_keyboards")) {
            opts.assign_keyboards = as_bool();
        } else if (iequals(key, "diseases_destroyable")) {
            opts.diseases_destroyable = as_bool();
        } else if (iequals(key, "lost_net_revert_ai")) {
            opts.lost_net_revert_ai = as_bool();
        } else if (iequals(key, "disable_game_music")) {
            opts.disable_game_music = as_bool();
        } else if (iequals(key, "modemport")) {
            opts.modemport = as_int();
        } else if (iequals(key, "modembaud")) {
            opts.modembaud = as_int();
        } else if (iequals(key, "modemirq")) {
            opts.modemirq = as_int();
        } else if (iequals(key, "modemdial")) {
            opts.modemdial = val;
        } else if (iequals(key, "netprotocol")) {
            int v = as_int();
            if (v < 0) v = 0;
            if (v > 3) v = 3;
            opts.netprotocol = v;
        } else if (iequals(key, "smallmemory")) {
            opts.smallmemory = as_bool();
        } else if (iequals(key, "keydef")) {
            // "keydef=<set>,<action>,<scancode>" — three comma-separated ints.
            // Out-of-range set/action drops the WHOLE line (§3's clamp note).
            int set = 0, action = 0, scancode = 0;
            if (std::sscanf(val.c_str(), "%d,%d,%d", &set, &action, &scancode) == 3 &&
                set >= 0 && set < KeyDef::kSets && action >= 0 && action < KeyDef::kActionsPerSet) {
                if (!opts.keydef) opts.keydef = KeyDef{};
                opts.keydef->scancode[set][action] = scancode;
            }
        }
        // Any other key hits the original's final `else` (a debug log line,
        // not a user-facing effect) and is intentionally ignored here — still
        // preserved verbatim by save_options' read-modify-write.
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
    auto set_bool = [&](const char* key, bool v) { set_key(key, v ? "1" : "0"); };

    if (opts.levelno) set_key("levelno", std::to_string(*opts.levelno));
    if (opts.num_to_win_match) set_key("num_to_win_match", std::to_string(*opts.num_to_win_match));
    if (opts.enclosement_depth) set_key("enclosement_depth", std::to_string(*opts.enclosement_depth));
    if (opts.conveyor_speed) set_key("conveyor_speed", std::to_string(*opts.conveyor_speed));
    if (opts.team_play) set_bool("team_play", *opts.team_play);
    if (opts.random_start) set_bool("random_start", *opts.random_start);
    if (opts.stomped_bombs_detonate) set_bool("stomped_bombs_detonate", *opts.stomped_bombs_detonate);
    if (opts.win_by_kills) set_bool("win_by_kills", *opts.win_by_kills);
    if (opts.goldman) set_bool("goldman", *opts.goldman);
    if (opts.schemefilename) set_key("schemefilename", *opts.schemefilename);
    if (opts.playtime) set_key("playtime", std::to_string(*opts.playtime));
    if (opts.assign_keyboards) set_bool("assign_keyboards", *opts.assign_keyboards);
    if (opts.diseases_destroyable) set_bool("diseases_destroyable", *opts.diseases_destroyable);
    if (opts.lost_net_revert_ai) set_bool("lost_net_revert_ai", *opts.lost_net_revert_ai);
    if (opts.disable_game_music) set_bool("disable_game_music", *opts.disable_game_music);
    if (opts.modemport) set_key("modemport", std::to_string(*opts.modemport));
    if (opts.modembaud) set_key("modembaud", std::to_string(*opts.modembaud));
    if (opts.modemirq) set_key("modemirq", std::to_string(*opts.modemirq));
    if (opts.modemdial) set_key("modemdial", *opts.modemdial);
    if (opts.netprotocol) set_key("netprotocol", std::to_string(*opts.netprotocol));
    if (opts.smallmemory) set_bool("smallmemory", *opts.smallmemory);
    if (opts.keydef) {
        // The writer (sub_405DE3) always emits all 20 triples in a fixed
        // (set, action) order; we do the same but skip a triple whose
        // scancode is still -1 (never bound), so a partially-populated KeyDef
        // (e.g. only the 6 UI-exposed actions) does not fabricate slots 6-9.
        // Multiple keydef= lines are ADDED (matching the file's real shape —
        // 20 distinct lines with the same key), so this key needs its own
        // find/replace-or-append pass per (set,action) triple rather than the
        // single-line set_key above.
        for (int set = 0; set < KeyDef::kSets; ++set) {
            for (int action = 0; action < KeyDef::kActionsPerSet; ++action) {
                int sc = opts.keydef->scancode[set][action];
                if (sc < 0) continue;
                std::string value = std::to_string(set) + "," + std::to_string(action) + "," +
                                     std::to_string(sc);
                std::string prefix = std::to_string(set) + "," + std::to_string(action) + ",";
                bool replaced = false;
                for (std::string& line : lines) {
                    auto eq = line.find('=');
                    if (eq == std::string::npos) continue;
                    if (!iequals(trim(line.substr(0, eq)), "keydef")) continue;
                    std::string existing = trim(line.substr(eq + 1));
                    if (existing.rfind(prefix, 0) != 0) continue;  // different (set,action)
                    line = "keydef=" + value;
                    replaced = true;
                    break;
                }
                if (!replaced) lines.push_back("keydef=" + value);
            }
        }
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("save_options: cannot write " + path.string());
    for (const std::string& line : lines) out << line << "\n";
}

}  // namespace bomber::assets
