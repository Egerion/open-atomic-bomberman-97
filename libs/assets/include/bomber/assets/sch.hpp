#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace bomber::assets::sch {

// Arena scheme (.SCH) — plain-text, self-documenting format shipped with the game.

enum class Cell : char {
    Blank = '.',
    Brick = ':',
    Solid = '#',
};

struct Spawn {
    int player = 0;
    int x = 0;
    int y = 0;
    int extra = 0;  // 4th field, purpose TBD (team/alt flag?)
};

struct PowerupRule {
    int id = 0;
    int born_with = 0;
    int has_override = 0;
    int override_value = 0;
    int forbidden = 0;
    std::string comment;
};

struct Scheme {
    int version = 0;
    std::string name;
    int brick_density = 100;         // 0-100 percent
    std::vector<std::string> rows;   // '#' solid, ':' brick, '.' blank
    std::vector<Spawn> spawns;
    std::vector<PowerupRule> powerups;

    int width() const { return rows.empty() ? 0 : static_cast<int>(rows[0].size()); }
    int height() const { return static_cast<int>(rows.size()); }
    Cell at(int x, int y) const { return static_cast<Cell>(rows[y][x]); }
};

Scheme load(const std::filesystem::path& path);

}  // namespace bomber::assets::sch
