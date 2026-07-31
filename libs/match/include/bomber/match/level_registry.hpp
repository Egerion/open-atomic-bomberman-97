#pragma once

// The single place a playable level is described. Before this existed, "a
// level" was spread across three hardcoded sites: pick_stage's `for i in 0..10`
// rotation (match_factory.hpp), AssetStore's `"FIELD"+to_string(stage)` path
// concatenation (asset_store.cpp), and the map-select screen's level_fallback
// name array (map_select_screen.cpp). Adding one map meant editing all three in
// lockstep. A LevelDef gathers everything those sites need — the stage index,
// the generic fallback name, and the three art-file base names — so adding a
// map is now one `add()` (or one entry in with_builtins()) that every consumer
// reads. SDL-free and dependency-free beyond bomber::sim, like the rest of
// bomber::match.

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "bomber/sim/tuning.hpp"

namespace bomber::match {

// The 11 stock levels. Named because three separate places need to agree on it:
// with_builtins() seeds exactly this many, enabled_stages() uses it as the
// built-in/custom cutoff, and it is the width of the VALUELST level_enabled mask
// (ids 1150..1160). It was a bare `11` at each of those sites.
inline constexpr int kBuiltinLevelCount = 11;

// The 11 generic fallback names (previously map_select_screen.cpp's
// level_fallback array). The real names live in the user's MESSAGES.TXT and load
// at runtime via getstring(150+i); these are never committed exe/asset material,
// just readable placeholders. At namespace scope rather than inside
// with_builtins() because it is a constant table, not a step of that factory.
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

// One playable level. `index` is the stage number used EVERYWHERE today (0..10
// for the 11 built-ins, 11+ for custom maps); it keys the per-level sim gates
// (Tuning::level_index — regen/ice), the EXTRA<index>.RES actor overlay, and the
// SOUNDLST 1100+index stage track. The three *_asset fields are BASE names only
// — no directory, no extension: AssetStore appends `DATA/RES/….PCX` for the
// field and `DATA/ANI/….ANI` for tiles/xbrick (plus the optional DATA_HD
// override). `name_fallback` is shown only when the user's MESSAGES.TXT lacks
// getstring(150+index); the real localized name always wins at runtime.
struct LevelDef {
    int index = 0;
    std::string name_fallback;
    std::string field_asset;   // e.g. "FIELD0"  -> DATA/RES/FIELD0.PCX
    std::string tiles_asset;   // e.g. "TILES0"  -> DATA/ANI/TILES0.ANI
    std::string xbrick_asset;  // e.g. "XBRICK0" -> DATA/ANI/XBRICK0.ANI
    bool builtin = false;
    // RANDOM-rotation flag for CUSTOM levels only. Built-ins are gated by the
    // VALUELST level_enabled[index] mask (ids 1150..1160) and IGNORE this field;
    // a registered custom level (index >= 11, no VALUELST slot) joins the
    // seed-based rotation only while this is true. Defaults to eligible.
    bool enabled = true;
};

// The catalogue of playable levels. `with_builtins()` seeds the 11 stock maps;
// `add()` is the one-edit extensibility point. Every level consumer
// (pick_stage, AssetStore::load_stage/stage_preview, the LEVEL & ROUNDS screen)
// reads from a registry instead of hardcoding, so the catalogue has a single
// source of truth.
class LevelRegistry {
public:
    // The 11 stock Atomic Bomberman levels: index i in [0,10] mapping to
    // {"FIELD"+i, "TILES"+i, "XBRICK"+i} with the generic English fallback name
    // (VALUELST 450-460 / getstring(150+i)). GOLDEN-SENSITIVE: the order, the
    // asset base names, and the rotation this feeds MUST reproduce exactly what
    // the old hardcoded sites produced — see enabled_stages().
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

    // Append a level. THE one-edit extensibility point: a custom map needs only
    // one add() (or one entry in with_builtins()) — every consumer picks it up.
    void add(LevelDef def) { levels_.push_back(std::move(def)); }

    const std::vector<LevelDef>& all() const { return levels_; }

    // The def for a stage index, or nullptr if unknown. Callers that build
    // asset paths use this; a null result means "fall back to the legacy
    // "FIELD"+to_string default" (never happens for the built-ins).
    const LevelDef* find(int index) const {
        for (const auto& l : levels_)
            if (l.index == index) return &l;
        return nullptr;
    }

    // The enabled rotation pick_stage draws from, in REGISTRATION order. For a
    // registry of the 11 built-ins this is byte-identical to the old
    // `for (i=0;i<11;++i) if (tuning.level_enabled[i]) allowed.push_back(i)` —
    // with_builtins() inserts index 0..10 in ascending order and each built-in
    // is gated by the SAME tuning.level_enabled[index] flag, so the produced
    // sequence (and therefore `seed % allowed.size()`) is unchanged. Custom
    // levels (index outside [0,11), no VALUELST slot) are gated by their own
    // LevelDef::enabled flag and appended after the built-ins.
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

// The default registry: the 11 built-ins, built once. pick_stage's back-compat
// overload and any caller without its own registry read this, so a caller that
// never touches a registry behaves exactly as before. Function-local static
// keeps the header ODR-safe with no .cpp.
inline const LevelRegistry& builtin_levels() {
    static const LevelRegistry kRegistry = LevelRegistry::with_builtins();
    return kRegistry;
}

}  // namespace bomber::match
