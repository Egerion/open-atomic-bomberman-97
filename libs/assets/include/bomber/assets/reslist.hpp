#pragma once

#include <cstddef>  // column_or's std::size_t; do not rely on a transitive include (§7)
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace bomber::assets::res {

// Parsers for the game's commented "id,value" text resources:
//   DATA/RES/VALUELST.RES  — gameplay tuning values (id -> integer)
//   DATA/RES/SOUNDLST.RES  — sound events (id -> RSS base name)

// bugprone-exception-escape (NOLINT below) — same std::map-default-ctor false
// positive documented on bomber::assets::res::Messages (messages.hpp); this
// struct's std::map members can't avoid it without a container-type change.
struct ValueList {  // NOLINT(bugprone-exception-escape)
    // id -> FIRST column. The sim/tuning path reads only this.
    std::map<int, std::int64_t> values;
    // id -> ALL columns, in file order. Some rows carry several numbers, and the
    // original's getvalue() FLATTENS them into consecutive slots, so
    // getvalue(701) is columns[700][1]. Presentation-only — this never feeds
    // Tuning::apply, so it cannot reach the sim or the golden hashes.
    std::map<int, std::vector<std::int64_t>> columns;
    std::vector<std::string> warnings;  // lines that looked like data but didn't parse

    std::int64_t at_or(int id, std::int64_t fallback) const {
        auto it = values.find(id);
        return it == values.end() ? fallback : it->second;
    }

    // `column_or(700, 1, x)` is the original's getvalue(701).
    std::int64_t column_or(int id, std::size_t index, std::int64_t fallback) const {
        auto it = columns.find(id);
        if (it == columns.end() || index >= it->second.size()) return fallback;
        return it->second[index];
    }
};

// bugprone-exception-escape (NOLINT below) — same std::map-default-ctor false
// positive documented on bomber::assets::res::Messages (messages.hpp).
struct SoundList {                     // NOLINT(bugprone-exception-escape)
    std::map<int, std::string> names;  // id -> base name (as written, e.g. "bmdrop2")
    std::vector<std::string> warnings;
};

ValueList load_values(const std::filesystem::path& path);
SoundList load_sounds(const std::filesystem::path& path);

}  // namespace bomber::assets::res
