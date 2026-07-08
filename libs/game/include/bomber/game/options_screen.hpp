#pragma once

#include <SDL3/SDL.h>

#include <cstdint>

#include "bomber/assets/reslist.hpp"
#include "bomber/game/asset_store.hpp"
#include "bomber/game/audio_engine.hpp"
#include "bomber/game/bmscreen.hpp"

// The interactive Options screen — Team Play + Conveyor Speed. Real gameplay
// options here are toggled by the player, not just described in the OPTIONS.BM
// help overlay (docs/re/frontend-flow.md "Interactive settings ... DEFERRED").
//
// TODO(RE): the original's exact Options/game-type screen layout is NOT pinned
// in docs/re/setup-screens.md — that doc only confirms `sub_42B0CE` as *A*
// game-options screen (net-game flow, VALUELST 765-778), not the local Team
// Play / Conveyor Speed pane this task asks for. Rather than invent a
// sub_XXXX citation, this screen is a clean-room minimal list consistent with
// the documented glue-screen conventions that ARE confirmed elsewhere
// (docs/re/setup-screens.md "Backdrop", "Music"): a random GLUE<n> backdrop
// via getvalue(16), FONT6 text, Up/Down to pick a row, Left/Right to change
// its value, Enter accepts (SFX 10) and Esc cancels (SFX 10) — mirroring
// present_setup's SFX 20 nav / 10 accept convention. Every literal screen
// coordinate below is OUR OWN layout, flagged inline.
namespace bomber::game {

// Row model: what the screen can edit. Kept tiny and explicit rather than a
// generic key-value list, since only two settings are documented enough to
// expose (see the class doc + the task's Random Start note below).
enum class OptionRow {
    TeamPlay,
    ConveyorSpeed,
    kCount,
};

class OptionsScreen {
public:
    OptionsScreen(const AssetStore& assets, const FontTextures& font, const assets::res::ValueList& values)
        : assets_(&assets), font_(&font), values_(&values) {}

    // (Re)enter the screen with the current settings (loaded from options.ini /
    // Tuning defaults by the caller). Picks a fresh random GLUE<n> backdrop
    // (docs/re/setup-screens.md "Backdrop", getvalue(16) = how many; a
    // presentation LCG, never State::rng — mirrors the original's rand()).
    void enter(bool team_play, int conveyor_speed_index);

    // Feed one SDL keycode. Returns true once Enter/Escape ends the screen;
    // check changed()/confirmed() to see what the caller should persist.
    void on_key(SDL_Keycode key, AudioEngine& audio);

    void draw(SDL_Renderer* ren) const;

    bool done() const { return done_; }
    // True if any setting differs from what enter() was called with — the
    // caller only writes options.ini when this is true (task requirement 3:
    // "write ONLY when the user changes a setting").
    bool changed() const { return changed_; }

    bool team_play() const { return team_play_; }
    int conveyor_speed_index() const { return conveyor_speed_index_; }

private:
    const AssetStore* assets_ = nullptr;
    const FontTextures* font_ = nullptr;
    const assets::res::ValueList* values_ = nullptr;

    std::string backdrop_;   // "GLUE<n>", picked fresh in enter()
    std::uint32_t lcg_ = 0xC0FFEE42u;  // presentation-only RNG (renderer.cpp's panic_lcg_ pattern)

    int row_ = 0;  // OptionRow, as an int for the wrap arithmetic
    bool team_play_ = false;
    int conveyor_speed_index_ = 1;  // 0 low / 1 medium / 2 high (Tuning default)
    bool changed_ = false;
    bool done_ = false;

    std::uint32_t next_rand();
};

}  // namespace bomber::game
