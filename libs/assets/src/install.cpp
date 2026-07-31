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

// The one shape both nodename.ini ends go through: keep the printable ASCII the
// FON can actually draw, drop everything else (a stray '\r', the '\n' sub_40C08C
// strips, any control byte a hand-edit could smuggle in), and truncate to the
// buffer the original reads into. Applied on BOTH read and write so a value
// cannot round-trip into something the roster then has to defend against.
std::string sanitize_node_name(const std::string& raw) {
    std::string out;
    for (const char c : raw) {
        if (out.size() >= kNodeNameMax) break;
        const unsigned char u = static_cast<unsigned char>(c);
        if (u >= 32 && u < 127) out += c;
    }
    // Leading/trailing blanks would render as an invisible name.
    return trim(out);
}

bool iequals(const std::string& a, const char* b) {
    std::size_t i = 0;
    for (; i < a.size() && b[i]; ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return i == a.size() && b[i] == '\0';
}

// One keyboard binding slot, so the keydef writer's helpers stay at three
// arguments instead of carrying (set, action) around separately.
struct KeySlot {
    int set = 0;
    int action = 0;
};

// "keydef=<set>,<action>,<scancode>" — three comma-separated ints. An
// out-of-range set/action drops the WHOLE line, matching the original's clamp
// (docs/re/results-and-options.md §3).
void apply_keydef(const std::string& val, Options& opts) {
    int set = 0, action = 0, scancode = 0;
    if (std::sscanf(val.c_str(), "%d,%d,%d", &set, &action, &scancode) != 3) return;
    if (set < 0 || set >= KeyDef::kSets || action < 0 || action >= KeyDef::kActionsPerSet) return;
    if (!opts.keydef) opts.keydef = KeyDef{};
    opts.keydef->scancode[set][action] = scancode;
}

// Find the line owning `key` (same "first '=' splits key/value" rule as the
// reader) and rewrite its value; append when the key is absent.
void set_key(std::vector<std::string>& lines, const char* key, const std::string& value) {
    for (std::string& line : lines) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        if (!iequals(trim(line.substr(0, eq)), key)) continue;
        line = std::string(key) + "=" + value;
        return;
    }
    lines.push_back(std::string(key) + "=" + value);
}

void set_bool(std::vector<std::string>& lines, const char* key, bool v) {
    set_key(lines, key, v ? "1" : "0");
}

// keydef= is the one key with MANY lines (the file's real shape: 20 distinct
// lines sharing a key), so it needs its own find/replace-or-append pass per
// (set,action) triple rather than the single-line set_key above.
void set_keydef_line(std::vector<std::string>& lines, KeySlot slot, int scancode) {
    const std::string prefix = std::to_string(slot.set) + "," + std::to_string(slot.action) + ",";
    const std::string value = prefix + std::to_string(scancode);
    for (std::string& line : lines) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        if (!iequals(trim(line.substr(0, eq)), "keydef")) continue;
        if (trim(line.substr(eq + 1)).rfind(prefix, 0) != 0) continue;  // different (set,action)
        line = "keydef=" + value;
        return;
    }
    lines.push_back("keydef=" + value);
}

// The writer (sub_405DE3) always emits all 20 triples in a fixed (set, action)
// order; we do the same but skip a triple whose scancode is still -1 (never
// bound), so a partially-populated KeyDef (e.g. only the 6 UI-exposed actions)
// does not fabricate slots 6-9.
void write_keydefs(std::vector<std::string>& lines, const KeyDef& keydef) {
    for (int set = 0; set < KeyDef::kSets; ++set) {
        for (int action = 0; action < KeyDef::kActionsPerSet; ++action) {
            const int sc = keydef.scancode[set][action];
            if (sc >= 0) set_keydef_line(lines, KeySlot{set, action}, sc);
        }
    }
}

// One gamedir.txt: first line, trimmed, accepted only if it names a directory.
// A UTF-8 BOM is stripped — PowerShell's `-Encoding utf8` writes one, and it
// otherwise becomes part of the path, which fails in a way that looks like the
// file was ignored entirely.
fs::path read_gamedir_file(const fs::path& file) {
    std::ifstream f(file);
    if (!f) return {};
    std::string line;
    if (!std::getline(f, line)) return {};
    if (line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);
    line = trim(line);
    return (!line.empty() && fs::is_directory(line)) ? fs::path(line) : fs::path{};
}

}  // namespace

fs::path default_game_dir(const fs::path& exe_dir) {
    if (const char* env = std::getenv("BOMBER_GAME_DIR"); env && *env && fs::is_directory(env))
        return env;

    // The probe order below runs from MOST to LEAST deliberate, and that
    // ordering is the contract, not an accident: the hardcoded absolute paths
    // used to be tried before anything exe-relative, so a machine that happened
    // to have an install at one of them worked while an identical copy of the
    // game beside its own data did not. "Works on the developer's machine
    // only" was a probe-order bug, so keep the machine-wide guesses last.

    // 1. gamedir.txt — an explicit answer. Working directory (a dev shell,
    //    `make run`) first, then the exe's own folder, which is what makes
    //    "unzip anywhere, drop a one-line gamedir.txt beside the exe" work
    //    regardless of what a shortcut set as the working directory.
    if (fs::path p = read_gamedir_file("gamedir.txt"); !p.empty()) return p;
    if (!exe_dir.empty())
        if (fs::path p = read_gamedir_file(exe_dir / "gamedir.txt"); !p.empty()) return p;

    if (!exe_dir.empty()) {
        // 2. The exe sitting INSIDE the install — the thing people actually do
        //    with a single self-contained binary: copy it into the game folder
        //    and double-click. The folder's NAME is irrelevant; DATA/ is what
        //    identifies an install (it is the directory every asset load is
        //    rooted at), so a renamed or hand-copied install is found too.
        if (fs::is_directory(exe_dir / "DATA")) return exe_dir;
        // 3. The install as a subfolder beside the exe.
        if (fs::is_directory(exe_dir / "BOMBRMAN")) return exe_dir / "BOMBRMAN";
    }

    // 4. Same two shapes relative to the working directory.
    if (fs::is_directory("DATA")) return ".";
    if (fs::is_directory("./BOMBRMAN")) return "./BOMBRMAN";

    // 5. Where the 1997 installer puts the game. A machine-wide guess: right
    //    often enough to be worth trying, never allowed to override any of the
    //    deliberate placements above.
    for (const char* p : {"D:/Program Files (x86)/INTRPLAY/BOMBRMAN",
                          "C:/Program Files (x86)/INTRPLAY/BOMBRMAN"}) {
        if (fs::is_directory(p)) return p;
    }
    return {};
}

namespace {

// The three post-read clamps from docs/re/results-and-options.md §3's table,
// named so the dispatch chain below is one statement per key. Keeping the
// arithmetic here rather than inline is what lets the chain stay a flat,
// checkable list.
int clamp_low(int v, int lo) {
    return v < lo ? lo : v;
}

// 1001 is the "unlimited" sentinel and is never clamped.
int clamp_playtime(int v) {
    return (v != 1001 && v < 60) ? 60 : v;
}

int clamp_netprotocol(int v) {
    return v < 0 ? 0 : (v > 3 ? 3 : v);
}

// The 22 keys sub_406238's stricmp chain actually has, in ITS order.
// DELIBERATELY a long if/else chain: the RE workflow requires a ported
// mechanic to mirror the binary rather than paraphrase it, and this chain IS
// the binary's shape (docs/coding-standards.md §8 — replacing a faithful-port
// dispatch with a table destroys the property that makes the port checkable
// against the exe). Answers whether the key was one of them.
bool apply_original_option(const std::string& key, const std::string& val, Options& opts) {
    const auto as_int = [&val] { return std::atoi(val.c_str()); };
    const auto as_bool = [&as_int] { return as_int() != 0; };
    // One line per key is the whole point: this block is meant to be read next
    // to sub_406238's stricmp chain and checked against it arm by arm, which is
    // CLAUDE.md's stated reason for holding a mirrored block off the formatter.
    // The upper clamp on levelno needs getvalue(35) and is left to the consumer.
    // clang-format off
    if (iequals(key, "levelno")) opts.levelno = clamp_low(as_int(), -1);
    else if (iequals(key, "num_to_win_match")) opts.num_to_win_match = clamp_low(as_int(), 1);
    else if (iequals(key, "enclosement_depth")) opts.enclosement_depth = clamp_low(as_int(), 0);
    else if (iequals(key, "conveyor_speed")) opts.conveyor_speed = as_int();
    else if (iequals(key, "team_play")) opts.team_play = as_bool();
    else if (iequals(key, "random_start")) opts.random_start = as_bool();
    else if (iequals(key, "stomped_bombs_detonate")) opts.stomped_bombs_detonate = as_bool();
    else if (iequals(key, "win_by_kills")) opts.win_by_kills = as_bool();
    else if (iequals(key, "goldman")) opts.goldman = as_bool();
    else if (iequals(key, "schemefilename")) opts.schemefilename = val;
    else if (iequals(key, "playtime")) opts.playtime = clamp_playtime(as_int());
    else if (iequals(key, "assign_keyboards")) opts.assign_keyboards = as_bool();
    else if (iequals(key, "diseases_destroyable")) opts.diseases_destroyable = as_bool();
    else if (iequals(key, "lost_net_revert_ai")) opts.lost_net_revert_ai = as_bool();
    else if (iequals(key, "disable_game_music")) opts.disable_game_music = as_bool();
    else if (iequals(key, "modemport")) opts.modemport = as_int();
    else if (iequals(key, "modembaud")) opts.modembaud = as_int();
    else if (iequals(key, "modemirq")) opts.modemirq = as_int();
    else if (iequals(key, "modemdial")) opts.modemdial = val;
    else if (iequals(key, "netprotocol")) opts.netprotocol = clamp_netprotocol(as_int());
    else if (iequals(key, "smallmemory")) opts.smallmemory = as_bool();
    else if (iequals(key, "keydef")) apply_keydef(val, opts);
    else return false;
    // clang-format on
    return true;
}

// The port's OWN keys — none of these exist in the 1997 binary, so none of them
// is in the chain above and none ever reached the original's stricmp list
// (install.hpp documents each). They are parsed the same normalized-bool way.
// This is the seam the chain is split on: it separates our additions from the
// faithful port rather than cutting the ported dispatch in an arbitrary place.
void apply_port_option(const std::string& key, const std::string& val, Options& opts) {
    const auto as_bool = [&val] { return std::atoi(val.c_str()) != 0; };
    // clang-format off
    if (iequals(key, "fullscreen")) opts.fullscreen = as_bool();
    else if (iequals(key, "vsync")) opts.vsync = as_bool();
    else if (iequals(key, "native_cadence")) opts.native_cadence = as_bool();
    else if (iequals(key, "show_fps")) opts.show_fps = as_bool();
    else if (iequals(key, "soft_scaling")) opts.soft_scaling = as_bool();
    // clang-format on
    // Anything else hits the original's final `else` (a debug log line, not a
    // user-facing effect) and is intentionally ignored — still preserved
    // verbatim by save_options' read-modify-write.
}

void apply_option(const std::string& key, const std::string& val, Options& opts) {
    if (apply_original_option(key, val, opts)) return;
    apply_port_option(key, val, opts);
}

}  // namespace

Options load_options(const fs::path& path) {
    Options opts;
    std::ifstream f(path);
    if (!f) return opts;  // no file: every field stays empty (caller keeps defaults)

    // Mirror sub_406238: for each line, split on the FIRST '=' into key/value,
    // trim both, and match the key case-insensitively. ';'-comment and blank
    // lines have no '=' in the key position we care about and are skipped.
    std::string line;
    while (std::getline(f, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(line.substr(0, eq));
        const std::string val = trim(line.substr(eq + 1));
        if (key.empty() || val.empty()) continue;
        apply_option(key, val, opts);
    }
    return opts;
}

namespace {

// Read every existing line verbatim, so a hand-edited file keeps its
// comments/ordering/unknown keys. A missing file yields an empty line set,
// which still writes back as a valid options.ini.
std::vector<std::string> read_lines(const fs::path& path) {
    std::vector<std::string> lines;
    std::ifstream in(path);
    if (!in) return lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

// The 21 single-line keys sub_405DE3 emits, in ITS fixed fprintf order — the
// mirror of apply_original_option's read chain, and split from the port's own
// keys on the same seam. Deliberately a flat list rather than a table: the
// fields are of three different types, so a variant-driven table would add a
// layer a reader has to decode before reaching logic that is already one line
// per key (docs/coding-standards.md §8 — a pattern nobody needs is a defect).
void write_original_keys(std::vector<std::string>& lines, const Options& opts) {
    if (opts.levelno) set_key(lines, "levelno", std::to_string(*opts.levelno));
    if (opts.num_to_win_match)
        set_key(lines, "num_to_win_match", std::to_string(*opts.num_to_win_match));
    if (opts.enclosement_depth)
        set_key(lines, "enclosement_depth", std::to_string(*opts.enclosement_depth));
    if (opts.conveyor_speed) set_key(lines, "conveyor_speed", std::to_string(*opts.conveyor_speed));
    if (opts.team_play) set_bool(lines, "team_play", *opts.team_play);
    if (opts.random_start) set_bool(lines, "random_start", *opts.random_start);
    if (opts.stomped_bombs_detonate)
        set_bool(lines, "stomped_bombs_detonate", *opts.stomped_bombs_detonate);
    if (opts.win_by_kills) set_bool(lines, "win_by_kills", *opts.win_by_kills);
    if (opts.goldman) set_bool(lines, "goldman", *opts.goldman);
    if (opts.schemefilename) set_key(lines, "schemefilename", *opts.schemefilename);
    if (opts.playtime) set_key(lines, "playtime", std::to_string(*opts.playtime));
    if (opts.assign_keyboards) set_bool(lines, "assign_keyboards", *opts.assign_keyboards);
    if (opts.diseases_destroyable)
        set_bool(lines, "diseases_destroyable", *opts.diseases_destroyable);
    if (opts.lost_net_revert_ai) set_bool(lines, "lost_net_revert_ai", *opts.lost_net_revert_ai);
    if (opts.disable_game_music) set_bool(lines, "disable_game_music", *opts.disable_game_music);
    if (opts.modemport) set_key(lines, "modemport", std::to_string(*opts.modemport));
    if (opts.modembaud) set_key(lines, "modembaud", std::to_string(*opts.modembaud));
    if (opts.modemirq) set_key(lines, "modemirq", std::to_string(*opts.modemirq));
    if (opts.modemdial) set_key(lines, "modemdial", *opts.modemdial);
    if (opts.netprotocol) set_key(lines, "netprotocol", std::to_string(*opts.netprotocol));
    if (opts.smallmemory) set_bool(lines, "smallmemory", *opts.smallmemory);
}

// PORT-ONLY keys — none of these is one of the original's 22 (install.hpp
// documents each), persisted through the SAME read-modify-write file so they
// round-trip like every RE'd toggle above.
void write_port_keys(std::vector<std::string>& lines, const Options& opts) {
    if (opts.fullscreen) set_bool(lines, "fullscreen", *opts.fullscreen);
    if (opts.vsync) set_bool(lines, "vsync", *opts.vsync);
    if (opts.native_cadence) set_bool(lines, "native_cadence", *opts.native_cadence);
    if (opts.show_fps) set_bool(lines, "show_fps", *opts.show_fps);
    if (opts.soft_scaling) set_bool(lines, "soft_scaling", *opts.soft_scaling);
}

}  // namespace

void save_options(const fs::path& path, const Options& opts) {
    // Read-modify-write: only the keys named in `opts` change, every other line
    // survives verbatim. The three calls below are in sub_405DE3's emit order,
    // which decides where a key ABSENT from the file gets appended.
    std::vector<std::string> lines = read_lines(path);
    write_original_keys(lines, opts);
    write_port_keys(lines, opts);
    if (opts.keydef) write_keydefs(lines, *opts.keydef);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("save_options: cannot write " + path.string());
    for (const std::string& line : lines) out << line << "\n";
}

std::string load_node_name(const fs::path& path) {
    // sub_40C08C: the FIRST line only, fgets(buf, 40), '\n' stripped.
    std::ifstream f(path);
    if (!f) return {};
    std::string line;
    if (!std::getline(f, line)) return {};
    return sanitize_node_name(line);
}

void save_node_name(const fs::path& path, const std::string& name) {
    // sub_40C140: fopen("nodename.ini", "wt") + fputs — one line, nothing else.
    // The shipped file has no trailing newline; a text-mode fputs of a buffer
    // that never held one wouldn't add it, so neither do we (and load_node_name
    // reads it back either way).
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("save_node_name: cannot write " + path.string());
    out << sanitize_node_name(name);
}

}  // namespace bomber::assets
