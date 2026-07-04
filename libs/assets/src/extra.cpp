#include "bomber/assets/extra.hpp"

#include <cctype>
#include <fstream>
#include <string>

namespace bomber::assets::extra {
namespace {

std::string trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\x1a");
    auto e = s.find_last_not_of(" \t\r\x1a");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

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

// Coordinate normalization from sub_404E99: wrap negatives up from the far
// edge, clamp over-large ones to the last tile.
int norm(int v, int extent) {
    while (v < 0) v += extent;
    if (v >= extent) v = extent - 1;
    return v;
}

}  // namespace

std::vector<Actor> parse(const std::filesystem::path& path, int board_w, int board_h) {
    std::vector<Actor> actors;
    std::ifstream f(path);
    if (!f) return actors;  // no EXTRA<N>.RES for this board => no actors

    std::string raw;
    while (std::getline(f, raw)) {
        std::string line = trim(raw);
        if (line.empty() || line[0] == ';') continue;
        if (line[0] != '-' || line.size() < 2) continue;

        const char cmd = static_cast<char>(std::toupper(static_cast<unsigned char>(line[1])));
        // Fields after the leading dash, split on commas and individually
        // trimmed (the files pad numbers with spaces: "-A,S, 2, 2").
        auto parts = split(line.substr(1), ',');  // parts[0] == type letter
        Actor a;

        if (cmd == 'A' || cmd == 'C') {
            if (parts.size() < 4) continue;  // needs type,dir,x,y
            int d = dir_letter(parts[1]);
            if (d < 0) continue;
            a.kind = (cmd == 'C') ? Kind::Conveyor : Kind::DirArrow;
            a.dir = d;
            a.x = norm(to_int(parts[2]), board_w);
            a.y = norm(to_int(parts[3]), board_h);
            actors.push_back(a);
        } else if (cmd == 'T') {
            if (parts.size() < 3) continue;  // needs type,x,y
            a.kind = Kind::Trampoline;
            // '-T,H,H' => random odd-parity placement, resolved by the caller.
            if (!parts[1].empty() &&
                std::toupper(static_cast<unsigned char>(parts[1][0])) == 'H') {
                a.random = true;
            } else {
                a.x = norm(to_int(parts[1]), board_w);
                a.y = norm(to_int(parts[2]), board_h);
            }
            actors.push_back(a);
        } else if (cmd == 'W') {
            if (parts.size() < 6) continue;  // type,type,idno,x,y,linkto
            a.kind = Kind::Warphole;
            a.idno = to_int(parts[2]);
            a.x = norm(to_int(parts[3]), board_w);
            a.y = norm(to_int(parts[4]), board_h);
            a.linkto = to_int(parts[5]);
            actors.push_back(a);
        }
        // Unknown type letters are skipped (the original aborts; we keep going).
    }
    return actors;
}

std::vector<Actor> load_for_board(const std::filesystem::path& game_dir, int board, int board_w,
                                  int board_h) {
    auto path = game_dir / "DATA" / "RES" / ("EXTRA" + std::to_string(board) + ".RES");
    return parse(path, board_w, board_h);
}

}  // namespace bomber::assets::extra
