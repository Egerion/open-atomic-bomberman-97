#pragma once

// The single place a playable level is described: the stage index, the generic
// fallback name, and the three art-file base names. Adding a map is one `add()`
// (or one entry in with_builtins()) that every consumer reads — the rotation,
// the asset paths and the map-select screen used to hardcode it separately.

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "bomber/sim/tuning.hpp"

namespace bomber::match {

// The 11 stock levels. Named because three places must agree on it:
// with_builtins() seeds this many, enabled_stages() uses it as the
// built-in/custom cutoff, and it is the width of the VALUELST level_enabled mask
// (ids 1150..1160).
inline constexpr int kBuiltinLevelCount = 11;

// Readable placeholders only — the real names live in the user's MESSAGES.TXT
// and load at runtime via getstring(150+i). Never committed asset material.
inline constexpr std::array<std::string_view, kBuiltinLevelCount> kBuiltinLevelNames{
    "NEW TRADITIONALIST",
    "CLASSIC GREEN ACRES",
    "HOCKEY RINK",
    "ANCIENT EGYPT",
    "COAL MINE",
    "BEACH",
    "ALIENS",
    "HAUNTED HOUSE",
    "UNDER THE OCEAN",
    "DEEP FOREST GREEN",
    "INNER CITY TRASH"};

// One playable level. `index` is the stage number (0..10 built-in, 11+ custom)
// and keys the per-level sim gates, the EXTRA<index>.RES actor overlay and the
// SOUNDLST 1100+index track. The three *_asset fields are BASE names only — no
// directory, no extension; AssetStore supplies both.
struct LevelDef {
    int index = 0;
    std::string name_fallback;
    std::string field_asset;   // e.g. "FIELD0"  -> DATA/RES/FIELD0.PCX
    std::string tiles_asset;   // e.g. "TILES0"  -> DATA/ANI/TILES0.ANI
    std::string xbrick_asset;  // e.g. "XBRICK0" -> DATA/ANI/XBRICK0.ANI
    bool builtin = false;
    // Rotation flag for CUSTOM levels ONLY. Built-ins have a VALUELST slot and
    // are gated by level_enabled[index] instead, ignoring this field.
    bool enabled = true;
};

// The catalogue of playable levels: `with_builtins()` seeds the 11 stock maps
// and `add()` is the one-edit extensibility point.
class LevelRegistry {
public:
    // GOLDEN-SENSITIVE: the order, the asset base names and the rotation this
    // feeds must reproduce exactly what the old hardcoded sites produced.
    static LevelRegistry with_builtins() {
        LevelRegistry reg;
        for (int i = 0; i < kBuiltinLevelCount; ++i) {
            LevelDef d;
            d.index = i;
            d.name_fallback = kBuiltinLevelNames[static_cast<std::size_t>(i)];
            d.field_asset = "FIELD" + std::to_string(i);
            d.tiles_asset = "TILES" + std::to_string(i);
            d.xbrick_asset = "XBRICK" + std::to_string(i);
            d.builtin = true;
            reg.add(std::move(d));
        }
        return reg;
    }

    void add(LevelDef def) { levels_.push_back(std::move(def)); }

    const std::vector<LevelDef>& all() const { return levels_; }

    // The def for a stage index, or nullptr if unknown — which tells a path
    // builder to fall back to the legacy "FIELD"+index default.
    const LevelDef* find(int index) const {
        for (const auto& l : levels_)
            if (l.index == index) return &l;
        return nullptr;
    }

    // The enabled rotation pick_stage draws from, in REGISTRATION order — which
    // for the built-ins is ascending index, so `seed % allowed.size()` picks
    // exactly what the old hardcoded loop picked. Two different gates: built-ins
    // read the VALUELST mask, custom levels their own LevelDef::enabled.
    std::vector<int> enabled_stages(const sim::Tuning& tuning) const {
        std::vector<int> allowed;
        for (const auto& l : levels_) {
            const bool on = (l.index >= 0 && l.index < kBuiltinLevelCount)
                                ? (tuning.level_enabled[l.index] != 0)  // built-in: VALUELST mask
                                : l.enabled;                            // custom: own flag
            if (on) allowed.push_back(l.index);
        }
        return allowed;
    }

private:
    std::vector<LevelDef> levels_;
};

// The default registry: the 11 built-ins, built once. Function-local static
// keeps the header ODR-safe with no .cpp.
inline const LevelRegistry& builtin_levels() {
    static const LevelRegistry kRegistry = LevelRegistry::with_builtins();
    return kRegistry;
}

}  // namespace bomber::match
