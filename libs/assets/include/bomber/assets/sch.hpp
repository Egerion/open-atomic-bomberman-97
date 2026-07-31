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
    // Optional 4th field, stored as a BOOLEAN by sub_403EEE (docs/re/facts.md
    // "The .SCH -S row's 4th field is the per-slot TEAM"). 19 of the 67 shipped
    // schemes author a non-parity layout here.
    int team = 0;
    // ABSENT and PRESENT-AND-ZERO are different answers, and `team` alone cannot
    // carry the distinction: sub_403EEE writes the slot's team record only on a
    // four-field row, so absent leaves sub_4049C0's alternating parity seed
    // standing while an explicit 0 overrides it. The writer always emits the
    // field, so anything round-tripped through to_text() comes back with it set.
    bool has_team = false;
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
    int brick_density = 100;        // 0-100 percent
    std::vector<std::string> rows;  // '#' solid, ':' brick, '.' blank
    std::vector<Spawn> spawns;
    std::vector<PowerupRule> powerups;

    int width() const { return rows.empty() ? 0 : static_cast<int>(rows[0].size()); }
    int height() const { return static_cast<int>(rows.size()); }
    Cell at(int x, int y) const { return static_cast<Cell>(rows[y][x]); }
};

Scheme load(const std::filesystem::path& path);

// Serializes back to the exact shipped .SCH text format (sub_403C16,
// docs/re/results-and-options.md §5; field layout in sch.cpp).
//
// ROUND-TRIP CONTRACT: load(to_text(s)) reproduces every field load() reads.
std::string to_text(const Scheme& scheme);
// Throws std::runtime_error if the file cannot be opened for writing.
void write(const Scheme& scheme, const std::filesystem::path& path);

}  // namespace bomber::assets::sch
