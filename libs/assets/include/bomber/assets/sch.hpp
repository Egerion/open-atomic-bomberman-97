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

// Serializes a Scheme back to the exact shipped .SCH text format (sub_403C16
// @0x403C16, docs/re/results-and-options.md §5: header comment lines, "-V"
// version, "-N" name, "-B" density, the "-R" row array, 10 "-S" player
// starts, and 13 "-P" powerup rows whose trailing comment is getstring(800+i)
// — see write()'s own doc comment in sch.cpp for the exact field layout).
// Round-trip contract: load(write(s)) reproduces every field load() reads
// (version/name/density/rows/spawns/powerups) — see tests/test_sch_write.cpp.
std::string to_text(const Scheme& scheme);
// Convenience: to_text() + write to `path`. Throws std::runtime_error if the
// file cannot be opened for writing (e.g. a bad target directory).
void write(const Scheme& scheme, const std::filesystem::path& path);

}  // namespace bomber::assets::sch
