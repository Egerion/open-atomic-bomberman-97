#include "bomber/assets/sch.hpp"

#include <fstream>
#include <stdexcept>

namespace bomber::assets::sch {
namespace {

std::string trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\x1a");
    auto e = s.find_last_not_of(" \t\r\x1a");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

std::vector<std::string> split(const std::string& s, char sep, int max_parts = -1) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (true) {
        if (max_parts > 0 && static_cast<int>(out.size()) == max_parts - 1) {
            out.push_back(s.substr(start));
            break;
        }
        auto p = s.find(sep, start);
        if (p == std::string::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, p - start));
        start = p + 1;
    }
    return out;
}

}  // namespace

Scheme load(const std::filesystem::path& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open scheme: " + path.string());

    Scheme sch;
    std::string raw;
    while (std::getline(f, raw)) {
        std::string line = trim(raw);
        if (line.empty() || line[0] == ';') continue;
        if (line[0] != '-' || line.size() < 2) continue;

        char cmd = line[1];
        std::string rest = line.size() > 3 ? line.substr(3) : std::string();  // after "-X,"
        switch (cmd) {
            case 'V': sch.version = std::stoi(rest); break;
            case 'N': sch.name = trim(rest); break;
            case 'B': sch.brick_density = std::stoi(rest); break;
            case 'R': {
                auto parts = split(rest, ',', 2);
                if (parts.size() != 2) throw std::runtime_error("bad -R line: " + path.string());
                sch.rows.push_back(trim(parts[1]));
                break;
            }
            case 'S': {
                auto parts = split(rest, ',');
                if (parts.size() < 3) throw std::runtime_error("bad -S line: " + path.string());
                Spawn sp;
                sp.player = std::stoi(parts[0]);
                sp.x = std::stoi(parts[1]);
                sp.y = std::stoi(parts[2]);
                if (parts.size() > 3) sp.extra = std::stoi(parts[3]);
                sch.spawns.push_back(sp);
                break;
            }
            case 'P': {
                auto parts = split(rest, ',', 6);
                if (parts.size() < 5) throw std::runtime_error("bad -P line: " + path.string());
                PowerupRule pr;
                pr.id = std::stoi(parts[0]);
                pr.born_with = std::stoi(parts[1]);
                pr.has_override = std::stoi(parts[2]);
                pr.override_value = std::stoi(parts[3]);
                pr.forbidden = std::stoi(parts[4]);
                if (parts.size() > 5) pr.comment = trim(parts[5]);
                sch.powerups.push_back(pr);
                break;
            }
            default: break;  // unknown directive: ignore, format is versioned
        }
    }

    if (sch.rows.empty()) throw std::runtime_error("scheme has no rows: " + path.string());
    for (auto& row : sch.rows)
        if (row.size() != sch.rows[0].size())
            throw std::runtime_error("scheme rows differ in width: " + path.string());
    return sch;
}

}  // namespace bomber::assets::sch
