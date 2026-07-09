#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>

#include "bomber/game/asset_store.hpp"
#include "bomber/game/audio_engine.hpp"
#include "bomber/game/bmscreen.hpp"

// The interactive Options screen — sub_4080DC @0x4080DC (docs/re/results-and-
// options.md §3, CONFIRMED: a 19-item interactive settings list; the doc
// corrects frontend-flow.md's earlier "map editor" mislabel). Every row below
// mirrors §3's table by POSITION (row 0..18); the on-select handler and
// backing options.ini key match that table. Layout is the CONFIRMED
// `745,55,40,22,500` VALUELST block (x=55, y0=40, ystep=22, colour=500);
// backdrop is the same random GLUE<n> convention present_setup/
// present_map_select share (docs/re/setup-screens.md "Backdrop", sub_4148E5).
//
// Per-row disposition (see options_screen.cpp's kRows table for the same
// list inline with the handlers):
//   0  Team Play                    — LIVE toggle, persisted (team_play=)
//   1  Random Start                 — LIVE toggle, persisted (random_start=);
//                                      wired into match::build_match_config's
//                                      `random_start` param — CONFIRMED as the
//                                      original's 200-pair-swap spawn shuffle
//                                      (sub_421793; docs/re/facts.md "Options
//                                      toggles"); absent-key default =
//                                      getvalue(40) = 1 (ON)
//   2  Node Name                    — OMITTED: net identity string, no
//                                      network play in this port and §3
//                                      explicitly notes it is not one of the
//                                      22 options.ini keys
//   3  Conveyor Speed               — LIVE cycle, persisted (conveyor_speed=)
//   4  Stomped Bombs Detonate       — LIVE toggle, persisted
//                                      (stomped_bombs_detonate=); REAL sim
//                                      consumer: Tuning::wall_detonates ->
//                                      EnclosureSystem::drop_wall (a closing
//                                      wall landing on a bomb detonates vs
//                                      silently eats it — sub_426818, docs/
//                                      re/facts.md "Options toggles");
//                                      absent-key default = getvalue(46) = 1
//   5  Win Matches By Kill Total    — LIVE toggle, persisted (win_by_kills=),
//                                      forced off with Team Play (§3); NO
//                                      match-clinch consumer yet (RESULTS
//                                      tally, docs/re/results-and-options.md
//                                      §1, is a separate deferred effort) —
//                                      shown+persisted, consumer TODO
//   6  Gold Bomberman                — LIVE toggle, persisted (goldman=); NO
//                                      roulette-wheel consumer yet (§4's
//                                      sub_4034BC is a separate deferred
//                                      feature) — shown+persisted, TODO
//   7  Enclosement Depth             — LIVE cycle, persisted
//                                      (enclosement_depth=); Tuning::
//                                      enclosement_depth has a REAL sim
//                                      consumer (enclosure.cpp/ai.cpp)
//   8  Scheme File                   — OMITTED: no on-disk .SCH browser/
//                                      stepper UI exists in this port yet
//                                      (present_setup/map_select don't expose
//                                      one either); the key still round-trips
//                                      via Options::schemefilename
//   9  Play Time                     — LIVE cycle (60s/120s/180s/300s/
//                                      unlimited), persisted (playtime=);
//                                      Tuning::game_seconds has a REAL sim
//                                      consumer (setup.cpp's ticks_left)
//   10 Assign Keyboard Player        — OMITTED: no distinct consumer beyond
//                                      what present_setup's KEYBOARD 0/1
//                                      slot picker already does
//   11 Diseases Can Be Destroyed     — LIVE toggle, persisted
//                                      (diseases_destroyable=); REAL sim
//                                      consumer: Tuning::diseases_destroyable
//                                      -> FlameSystem::spread_to + BombSystem
//                                      ::slide (OFF relocates a destroyed
//                                      floor skull to a random free tile —
//                                      sub_4230A5/sub_42331C -> sub_4255B2,
//                                      docs/re/facts.md "Options toggles");
//                                      absent-key default = getvalue(120) = 1
//   12 Lost net players revert to AI — OMITTED: no network play
//   13 Disable music during gameplay — LIVE toggle, persisted
//                                      (disable_game_music=); REAL consumer —
//                                      GameApp gates start_match's
//                                      audio_.start_music(stage) call on it
//   14 Modem: P/I/B/#                — OMITTED: no modem/net play
//   15 Define keyboard layouts       — LIVE: opens KeyRemapScreen (§2)
//   16 Set Default Network Protocol  — OMITTED: no network play
//   17 Use Enhanced Memory Model     — OMITTED: no memory-model concept in a
//                                      modern build
//   18 Adjust Audio                  — OMITTED: no nested volume sub-screen
//                                      exists (AudioEngine has no volume
//                                      control to adjust)
// Rows 2/8/10/12/14/16/17/18 (net/gfx/legacy/unbuilt-UI rows) are therefore
// not drawn at all rather than shown greyed — the row list below is the
// 11-entry LIVE subset, in the original's row order, so the cursor still
// walks the same relative sequence.
namespace bomber::game {

// The live-row subset, in the original 19-row order (see the file doc above
// for the full disposition and the omitted rows).
enum class OptionRow : std::uint8_t {
    TeamPlay,        // row 0
    RandomStart,     // row 1
    ConveyorSpeed,   // row 3
    StompedBombs,    // row 4
    WinByKills,      // row 5
    GoldBomberman,   // row 6
    EnclosementDepth,// row 7
    PlayTime,        // row 9
    DiseasesDestroy, // row 11
    DisableMusic,    // row 13
    KeyRemap,        // row 15 — opens the key-remap sub-screen
    kCount,
};

// Snapshot of every editable setting, passed in on enter() and read back via
// the matching accessor once done(). Kept as one struct (rather than growing
// OptionsScreen's own ad hoc fields per row) since the row count roughly
// tripled from the Team Play/Conveyor Speed original.
struct OptionsSnapshot {
    bool team_play = false;
    // The three VALUELST-seeded toggles default to the original's getvalue
    // seeds (sub_41095A: ids 40/46/120, all = 1 in the shipped install);
    // GameApp::init re-seeds them from the live VALUELST + options.ini, so
    // these literals only matter for a snapshot never fed through init.
    bool random_start = true;            // getvalue(40) = 1
    int conveyor_speed_index = 1;    // 0 low / 1 medium / 2 high
    bool stomped_bombs_detonate = true;  // getvalue(46) = 1
    bool win_by_kills = false;
    bool goldman = false;
    int enclosement_depth = 1;       // 0..3
    int playtime_seconds = 150;      // one of kPlayTimeChoices, or 1001 = unlimited
    bool diseases_destroyable = true;    // getvalue(120) = 1
    bool disable_game_music = false;
};

// The Play Time cycle's choices (§3 row 9: `sub_4076FE`, no exact value list
// pinned — this is our own reasonable stepper set). 1001 is the CONFIRMED
// "unlimited" sentinel (Options::playtime's clamp note, §3).
inline constexpr int kPlayTimeChoices[] = {60, 120, 180, 300, 600, 1001};
inline constexpr int kPlayTimeChoiceCount =
    static_cast<int>(sizeof(kPlayTimeChoices) / sizeof(kPlayTimeChoices[0]));

class OptionsScreen {
public:
    OptionsScreen(const AssetStore& assets, const FontTextures& font)
        : assets_(&assets), font_(&font) {}

    // (Re)enter the screen with the current settings (loaded from options.ini /
    // Tuning defaults by the caller) and a backdrop name already picked by the
    // caller's pick_glue() (docs/re/setup-screens.md "Backdrop" — the same
    // random-GLUE<n> convention present_setup/present_map_select use).
    void enter(const OptionsSnapshot& current, std::string backdrop);

    // Feed one SDL keycode. Returns true once Enter/Escape ends the screen;
    // check changed()/confirmed() to see what the caller should persist. A
    // Right/Enter on the "Define keyboard layouts" row instead sets
    // open_keyremap() so the caller can push the KeyRemapScreen on top
    // (§2 — that sub-screen is not part of this class, it edits
    // KeyboardMapper's live bindings directly via the caller).
    void on_key(SDL_Keycode key, AudioEngine& audio);

    void draw(SDL_Renderer* ren) const;

    bool done() const { return done_; }
    // True if any setting differs from what enter() was called with — the
    // caller only writes options.ini when this is true.
    bool changed() const { return changed_; }
    // True if the Team Play or Gold Bomberman row was pressed at ALL during
    // this visit, regardless of the net before/after value (pseudo.c
    // 9310-9311/9334-9335/9410-9412/9436-9437, docs/re/results-and-
    // options.md §3 rows 0/6): the original clears `dword_46492C` INLINE,
    // unconditionally, on every press of either row — so toggling one an
    // even number of times (ending back at its original value) still
    // forfeits a pending gold player in the original, which a plain
    // snapshot-diff at screen-exit would miss. The caller (GameApp) checks
    // this instead of comparing snapshots for the gold-player clear.
    bool gold_forfeiting_row_touched() const { return goldman_touched_ || team_play_touched_; }
    // True for exactly one frame's worth of on_key() calls: the highlighted
    // row was "Define keyboard layouts" and Enter/Right was pressed. The
    // caller checks this AFTER on_key(), pushes the key-remap screen, then
    // must clear it is not needed — enter() resets it, and it is only ever
    // read once per press in the app's own loop (see game_app.cpp).
    bool open_keyremap() const { return open_keyremap_; }

    const OptionsSnapshot& snapshot() const { return snap_; }

private:
    const AssetStore* assets_ = nullptr;
    const FontTextures* font_ = nullptr;

    std::string backdrop_;   // "GLUE<n>", supplied by the caller's pick_glue()

    int row_ = 0;  // OptionRow, as an int for the wrap arithmetic
    OptionsSnapshot snap_;
    bool changed_ = false;
    bool done_ = false;
    bool open_keyremap_ = false;
    bool goldman_touched_ = false;    // see gold_forfeiting_row_touched()
    bool team_play_touched_ = false;
};

}  // namespace bomber::game
