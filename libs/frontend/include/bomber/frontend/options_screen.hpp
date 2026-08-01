#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "bomber/audio/audio_engine.hpp"
#include "bomber/frontend/options_model.hpp"  // OptionsSnapshot / OptionRow / kCursorRowCount
#include "bomber/game_util/cursor_indicator.hpp"
#include "bomber/render/asset_store.hpp"
#include "bomber/ui/bmscreen.hpp"

// The interactive Options screen — sub_4080DC @0x4080DC. The settings MODEL it
// edits lives in options_model.hpp; the per-row disposition, the layout pins,
// the chrome corrections and the key dispatch are in
// docs/frontend-options-rows.md.

namespace bomber::game {

class OptionsScreen {
public:
    OptionsScreen(const AssetStore& assets, const FontTextures& font)
        : assets_(&assets), font_(&font) {}

    // (Re)enter with the current settings and a backdrop name the caller's
    // pick_glue() already chose.
    void enter(const OptionsSnapshot& current, std::string backdrop);

    // Feed one SDL keycode. Escape is the ONLY key that ends the screen;
    // Enter/Space/Left/Right all act on the highlighted row and never exit.
    void on_key(SDL_Keycode key, AudioEngine& audio);

    // Faithful sub_413BD6 pacing (cursor_indicator.hpp): the sprite IDLES on step
    // 0 and blinks through the sequence, one step per rendered frame, every
    // getvalue(690) + rand()%getvalue(691) seconds — which is what the caller
    // passes alongside the wall clock.
    void tick(std::uint64_t now_s, int blink_base_s, int blink_spread_s) {
        blink_now_s_ = now_s;
        blink_base_s_ = blink_base_s;
        blink_spread_s_ = blink_spread_s;
    }

    void draw(SDL_Renderer* ren) const;

    bool done() const { return done_; }
    // Any setting differs from what enter() was called with — the caller only
    // writes options.ini when this is true.
    bool changed() const { return changed_; }
    // The Team Play or Gold Bomberman row was PRESSED, regardless of the net
    // before/after value: the original clears dword_46492C inline on EVERY press,
    // so toggling one an even number of times still forfeits a pending gold
    // player, and a snapshot-diff at exit would miss that.
    bool gold_forfeiting_row_touched() const { return goldman_touched_ || team_play_touched_; }
    // Set for one press of the "Define keyboard layouts" row. enter() resets it
    // and the app's loop reads it once per press, so nothing has to clear it.
    bool open_keyremap() const { return open_keyremap_; }
    // Same contract for row 8 "Scheme File" — the *.SCH picker.
    bool open_scheme_picker() const { return open_scheme_picker_; }
    // The picker's write-back (sub_407582's strcpy + strupr happen in the caller).
    void set_scheme_filename(std::string name) {
        snap_.scheme_filename = std::move(name);
        changed_ = true;
    }
    // Same contract for row 2 "Node Name" — sub_4074DC's text-entry prompt.
    bool open_node_name_prompt() const { return open_node_name_prompt_; }
    void set_node_name(std::string name) {
        snap_.node_name = std::move(name);
        changed_ = true;
    }

    const OptionsSnapshot& snapshot() const { return snap_; }

private:
    // The per-row action Left/Right/Enter/Space dispatches to: toggles ignore
    // `dir`, cyclers step by it, and the sub-screen rows open regardless of it.
    void activate_row(int dir);
    void toggle(bool& value);
    void toggle_team_play();
    void step_playtime(int dir);
    std::array<std::string, static_cast<std::size_t>(OptionRow::kCount)> row_text() const;
    void draw_backdrop(SDL_Renderer* ren) const;
    void draw_rows(SDL_Renderer* ren) const;
    void draw_footer(SDL_Renderer* ren) const;
    void draw_cursor(SDL_Renderer* ren) const;

    const AssetStore* assets_ = nullptr;
    const FontTextures* font_ = nullptr;

    std::string backdrop_;  // "GLUE<n>", supplied by the caller's pick_glue()

    int row_ = 0;  // an OptionRow, as int for the wrap arithmetic; always < kCursorRowCount
    // Mutable because draw() is const and the blink step advances at draw time,
    // once per rendered frame — exactly where sub_413BD6 sits in the original.
    mutable CursorIndicator cursor_blink_;
    std::uint64_t blink_now_s_ = 0;
    int blink_base_s_ = 2;
    int blink_spread_s_ = 2;
    OptionsSnapshot snap_;
    bool changed_ = false;
    bool done_ = false;
    bool open_keyremap_ = false;
    bool open_scheme_picker_ = false;
    bool open_node_name_prompt_ = false;
    bool goldman_touched_ = false;  // see gold_forfeiting_row_touched()
    bool team_play_touched_ = false;
};

}  // namespace bomber::game
