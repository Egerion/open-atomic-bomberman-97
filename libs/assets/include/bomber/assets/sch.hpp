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
    // Optional 4th field: this spawn's TEAM (docs/re/facts.md "The .SCH -S
    // row's 4th field is the per-slot TEAM"). sub_403EEE stores it as a
    // BOOLEAN — `value != 0` — into the slot's start record, and its own tail
    // loop pushes that into the player record's +84 team byte via sub_422437,
    // the same byte the setup screen's 'T' key toggles. Absent on a 3-field
    // row, in which case the slot keeps sub_4049C0's alternating parity
    // default (see match::scheme_setup_teams, which applies that rule).
    // 19 of the 67 shipped schemes author a non-parity layout here.
    int team = 0;
    // Was the 4th field actually present? sub_403EEE writes the slot's team
    // record ONLY on a four-field row, so "absent" and "present and 0" are
    // different answers: absent leaves sub_4049C0's parity seed standing,
    // 0 overrides it to team 0. `team` alone cannot carry that distinction.
    // The writer (like sub_403C16) always emits the field, so a scheme
    // round-tripped through to_text() comes back with this set.
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
