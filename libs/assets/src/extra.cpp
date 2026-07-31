#include "bomber/assets/extra.hpp"

#include <cctype>
#include <cstddef>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "text_util.hpp"

namespace bomber::assets::extra {
namespace {

using text::trim;

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (true) {
        auto p = s.find(sep, start);
        if (p == std::string::npos) {
            out.push_back(trim(s.substr(start)));
            break;
        }
        out.push_back(trim(s.substr(start, p - start)));
        start = p + 1;
    }
    return out;
}

// Direction letter -> godir (sub_404DB8): n=0, e=1, s=2, w=3; else -1.
int dir_letter(const std::string& tok) {
    if (tok.empty()) return -1;
    switch (std::tolower(static_cast<unsigned char>(tok[0]))) {
        case 'n': return 0;
        case 'e': return 1;
        case 's': return 2;
        case 'w': return 3;
        default: return -1;
    }
}

// atoi that tolerates a leading sign and spaces; returns 0 on garbage (matches
// the original's sub_4516C1 behaviour closely enough for placement).
int to_int(const std::string& tok) {
    try {
        return std::stoi(tok);
    } catch (...) {
        return 0;
    }
}

// The board a line's coordinates are normalized against, so the per-kind
// parsers below take one argument for it instead of a (w, h) pair.
struct Board {
    int w = 0;
    int h = 0;
};

// Coordinate normalization from sub_404E99: wrap negatives up from the far
// edge, clamp over-large ones to the last tile.
//
// The original writes the wrap as `while (v < 0) v += extent`, and transcribing
// that literally made the iteration count a FUNCTION OF THE FILE: `-A,n,
// -2147483648,0` spins ~143 million times on a 15-wide board, and a file of
// such rows hangs the loader before it ever returns. The closed form below is
// the same function on every input — repeated addition of a positive `extent`
// to a negative `v` lands exactly on the Euclidean remainder in [0, extent),
// which the clamp then cannot trip — in constant time. `extent <= 0` had no
// terminating case at all; a board with no tiles has no position to normalise
// to, so it collapses to 0.
int norm(int v, int extent) {
    if (extent <= 0) return 0;
    if (v < 0) return (v % extent + extent) % extent;
    return v >= extent ? extent - 1 : v;
}

// '-A' / '-C': type,dir,x,y. An unrecognised direction letter drops the line,
// matching the original's godir lookup returning -1.
std::optional<Actor> parse_arrow(const std::vector<std::string>& parts, char cmd, Board board) {
    if (parts.size() < 4) return std::nullopt;
    const int d = dir_letter(parts[1]);
    if (d < 0) return std::nullopt;
    Actor a;
    a.kind = (cmd == 'C') ? Kind::Conveyor : Kind::DirArrow;
    a.dir = d;
    a.x = norm(to_int(parts[2]), board.w);
    a.y = norm(to_int(parts[3]), board.h);
    return a;
}

// '-T': type,x,y — or '-T,H,H', which asks for a random odd-parity placement
// the caller resolves with a setup-only RNG (stage-actors.md §8).
std::optional<Actor> parse_trampoline(const std::vector<std::string>& parts, Board board) {
    if (parts.size() < 3) return std::nullopt;
    Actor a;
    a.kind = Kind::Trampoline;
    if (!parts[1].empty() && std::toupper(static_cast<unsigned char>(parts[1][0])) == 'H') {
        a.random = true;
        return a;
    }
    a.x = norm(to_int(parts[1]), board.w);
    a.y = norm(to_int(parts[2]), board.h);
    return a;
}

// '-W': type,type,idno,x,y,linkto.
std::optional<Actor> parse_warphole(const std::vector<std::string>& parts, Board board) {
    if (parts.size() < 6) return std::nullopt;
    Actor a;
    a.kind = Kind::Warphole;
    a.idno = to_int(parts[2]);
    a.x = norm(to_int(parts[3]), board.w);
    a.y = norm(to_int(parts[4]), board.h);
    a.linkto = to_int(parts[5]);
    return a;
}

std::optional<Actor> parse_line(const std::string& line, Board board) {
    if (line.empty() || line[0] == ';') return std::nullopt;
    if (line[0] != '-' || line.size() < 2) return std::nullopt;

    const char cmd = static_cast<char>(std::toupper(static_cast<unsigned char>(line[1])));
    // Fields after the leading dash, split on commas and individually trimmed
    // (the files pad numbers with spaces: "-A,S, 2, 2"). parts[0] == the letter.
    const std::vector<std::string> parts = split(line.substr(1), ',');
    if (cmd == 'A' || cmd == 'C') return parse_arrow(parts, cmd, board);
    if (cmd == 'T') return parse_trampoline(parts, board);
    if (cmd == 'W') return parse_warphole(parts, board);
    // Unknown type letters are skipped (the original aborts; we keep going).
    return std::nullopt;
}

}  // namespace

std::vector<Actor> parse(const std::filesystem::path& path, int board_w, int board_h) {
    std::vector<Actor> actors;
    std::ifstream f(path);
    if (!f) return actors;  // no EXTRA<N>.RES for this board => no actors

    const Board board{board_w, board_h};
    std::string raw;
    while (std::getline(f, raw)) {
        if (std::optional<Actor> a = parse_line(trim(raw), board)) actors.push_back(*a);
    }
    return actors;
}

std::vector<Actor> load_for_board(const std::filesystem::path& game_dir, int board, int board_w,
                                  int board_h) {
    auto path = game_dir / "DATA" / "RES" / ("EXTRA" + std::to_string(board) + ".RES");
    return parse(path, board_w, board_h);
}

}  // namespace bomber::assets::extra
