#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>

#include "bomber/game/asset_store.hpp"
#include "bomber/game/audio_engine.hpp"
#include "bomber/game/bmscreen.hpp"

// The interactive Options screen — sub_4080DC @0x4080DC (docs/re/results-and-
// options.md §3, CONFIRMED: a 19-row settings list; the doc corrects
// frontend-flow.md's earlier "map editor" mislabel). Every row below mirrors
// §3's table by POSITION (row 0..18); the on-select handler and backing
// options.ini key match that table. Layout is the CONFIRMED `745,55,40,22,
// 500` VALUELST block (x=55, y0=40, ystep=22, W=500 — a text-clip width, NOT
// a colour, despite the earlier comment: `sub_41696C`'s 4th arg is a width,
// confirmed by cross-referencing the SAME "id,X,Y,YS,W" column shape §1's
// RESULTS-screen VALUELST rows already use); backdrop is the same random
// GLUE<n> convention present_setup/present_map_select share (docs/re/
// setup-screens.md "Backdrop", sub_4148E5).
//
// 2026-07-09 FULL AUDIT (re-read of sub_4080DC's decompiled body, pseudo.c
// 8914-9491) found the port's earlier "hide the 8 unwired rows" choice was
// itself the bug: the original draws all 19 rows unconditionally, in the
// SAME general ink (`byte_49D38F`, RGB(255,255,255) — §1's LUT decode) for
// EVERY row including the currently-selected one — there is no per-row
// colour distinction for "wired" vs "not wired", "selected" vs not. Every
// row is therefore shown here too, even the net/modem/legacy ones this port
// cannot act on (documented no-op stubs below), matching CLAUDE.md's "no
// invented visuals" contract: the original SHOWS these rows, so hiding them
// was the invented deviation, not showing them.
//
// Three more corrections from the same re-read:
//  - NO title/header text draws anywhere in sub_4080DC's body (no getstring
//    call before the row loop, and the row-3 dispatch in the caller,
//    pseudo.c ~30910, doesn't wrap the call with a header draw either,
//    unlike e.g. the key-remap sub-screen's own confirmed getstring(1100)
//    header). The port's previous "OPTIONS" text at a guessed (55,20) had NO
//    RE citation — removed rather than invented. (The four external chrome
//    primitives sub_41043C/sub_415CA4/sub_415C1F/sub_429790 have no visible
//    body in the decompile, so this can't be proven to be the FULL picture —
//    documented as an open question, not asserted as certainly chrome-free —
//    but nothing here justifies a specific guessed title string/position.)
//  - The selection indicator is NOT a text recolour — it is a real cursor
//    sprite, CONFIRMED: `sub_413BD6`'s own body (pseudo.c 16691-16721)
//    resolves the ANI sequence name `"cursor1"` (`aCursor1`) and blits it at
//    (x-20, row_y) with its own self-paced frame timer (getvalue 690/691
//    driven). `"cursor1"` lives in MISC.ANI (already loaded via
//    `AssetStore::misc()` for the editor's teamring markers; its sequence
//    table is confirmed `cursor1, goldman, ring, safe, scan, teamring0,
//    teamring1`). Ported as a real sprite draw here instead of the earlier
//    yellow-recolour + "> " prefix stand-in.
//  - The row-navigation wrap count is a literal **18**, not 19
//    (`v168 = 18;`, pseudo.c 9086, used verbatim by both the Up-key
//    underflow wrap and the Down-key overflow wrap) — i.e. row 18 ("Adjust
//    Audio") is drawn but is PERMANENTLY UNREACHABLE via Up/Down in the
//    shipped binary (a genuine off-by-one in the original, not an RE
//    ambiguity: the switch still has a live `case 18` — dead code because
//    the cursor variable can never hold 18 when the switch runs). Faithfully
//    reproduced: `kCursorRowCount` gates navigation to rows 0..17; row 18 is
//    drawn every frame but never receives the cursor and never dispatches.
//  - `sub_427961(20)` (nav blip) is the ONLY sound sub_4080DC ever plays,
//    unconditionally for any real keypress (`if (v165 != -1 && v165 != -2)
//    sub_427961(20);`, pseudo.c 9298-9299) — there is no separate "accept"
//    jingle (SFX 10) anywhere in this function, unlike screens that
//    genuinely do fire one on Enter. The port's previous `audio.play(10)`
//    calls on Enter/Escape/"open key-remap" were invented — replaced with
//    the same uniform SFX 20 every other key already gets.
//
// Per-row disposition (see options_screen.cpp's row table for the same list
// inline with the handlers):
//   0  Team Play                    — LIVE toggle, persisted (team_play=);
//                                      ALSO clears the pending Goldman winner
//                                      (dword_46492C=-1, same as row 6) —
//                                      caller-side (game_app.cpp) resets
//                                      gold_player_ on EITHER team_play or
//                                      goldman changing, not goldman alone.
//   1  Random Start                 — LIVE toggle, persisted (random_start=)
//   2  Node Name                    — SHOWN, non-interactive: net identity
//                                      string, no network play in this port,
//                                      and §3 explicitly notes it is not one
//                                      of the 22 options.ini keys. Displays
//                                      empty (no net-identity concept here).
//   3  Conveyor Speed               — LIVE cycle, persisted (conveyor_speed=)
//   4  Stomped Bombs Detonate       — LIVE toggle, persisted
//                                      (stomped_bombs_detonate=)
//   5  Win Matches By Kill Total    — LIVE toggle, persisted (win_by_kills=),
//                                      forced off with Team Play (§3)
//   6  Gold Bomberman                — LIVE toggle, persisted (goldman=);
//                                      also clears the pending Goldman winner
//   7  Enclosement Depth             — LIVE cycle, persisted
//                                      (enclosement_depth=)
//   8  Scheme File                   — SHOWN, non-interactive: displays the
//                                      currently-loaded scheme's filename
//                                      (schemefilename=, round-tripped) but
//                                      stepping through the on-disk `.SCH`
//                                      list has no browsing UI in this port.
//   9  Play Time                     — LIVE cycle (our own reasonable
//                                      60/120/180/300/600/unlimited set — no
//                                      exact original stepper list RE'd),
//                                      persisted (playtime=)
//   10 Assign Keyboard Player        — LIVE toggle, persisted
//                                      (assign_keyboards=); no distinct sim
//                                      consumer (present_setup's own KEYBOARD
//                                      0/1 slot picker already covers this),
//                                      but it is a trivial real boolean with
//                                      a real options.ini key, so it round-
//                                      trips like every other toggle here.
//   11 Diseases Can Be Destroyed     — LIVE toggle, persisted
//                                      (diseases_destroyable=)
//   12 Lost net players revert to AI — LIVE toggle, persisted
//                                      (lost_net_revert_ai=); no consumer (no
//                                      network play), round-trips like row 10
//   13 Disable music during gameplay — LIVE toggle, persisted
//                                      (disable_game_music=)
//   14 Modem: P/I/B/#                — SHOWN, non-interactive: no modem/net
//                                      play; displays a fixed "N/A" (the
//                                      original's own value formatting here
//                                      is a single %d of modemport only, per
//                                      pseudo.c 9249-9251 — not a 4-field
//                                      P/I/B/# breakdown despite the label).
//   15 Define keyboard layouts       — LIVE: opens KeyRemapScreen (§2)
//   16 Set Default Network Protocol  — SHOWN, non-interactive: no network
//                                      play; displays "N/A".
//   17 Use Enhanced Memory Model     — LIVE toggle, persisted (smallmemory=);
//                                      label is INVERTED versus the backing
//                                      value (`getstring((dword_464824==0)+
//                                      25)`, pseudo.c 9280) — smallmemory==0
//                                      shows "YES", ==1 shows "NO". No
//                                      consumer (no memory-model concept in
//                                      a modern build); round-trips.
//   18 Adjust Audio                  — SHOWN, PERMANENTLY UNREACHABLE (see
//                                      the v168=18 note above) — drawn every
//                                      frame, never selectable, never
//                                      dispatches. No nested volume sub-
//                                      screen exists here either way
//                                      (AudioEngine has no volume control).
namespace bomber::game {

// All 19 rows, in the original's exact order/positions (row index == the
// original's `v166` switch case). See the file doc above for each row's
// disposition; `kCursorRowCount` below governs which are reachable.
enum class OptionRow : std::uint8_t {
    TeamPlay,          // row 0
    RandomStart,       // row 1
    NodeName,          // row 2  — display-only
    ConveyorSpeed,     // row 3
    StompedBombs,      // row 4
    WinByKills,        // row 5
    GoldBomberman,     // row 6
    EnclosementDepth,  // row 7
    SchemeFile,        // row 8  — display-only
    PlayTime,          // row 9
    AssignKeyboard,    // row 10
    DiseasesDestroy,   // row 11
    LostNetRevertAI,   // row 12
    DisableMusic,      // row 13
    Modem,             // row 14 — display-only
    KeyRemap,          // row 15 — opens the key-remap sub-screen
    NetProtocol,       // row 16 — display-only
    SmallMemory,       // row 17
    AdjustAudio,       // row 18 — CONFIRMED unreachable, see file doc
    kCount,
};

// CONFIRMED literal (pseudo.c 9086, `v168 = 18;`): Up/Down navigation wraps
// over rows [0, kCursorRowCount), NOT [0, kCount) — row 18 is drawn but can
// never receive the cursor. Do not "fix" this to kCount; it is a faithful
// reproduction of the shipped binary's own off-by-one, not an RE ambiguity.
inline constexpr int kCursorRowCount = 18;

// Snapshot of every editable/displayable setting, passed in on enter() and
// read back via the matching accessor once done(). Kept as one struct
// (rather than growing OptionsScreen's own ad hoc fields per row).
struct OptionsSnapshot {
    bool team_play = false;
    // The three VALUELST-seeded toggles default to the original's getvalue
    // seeds (sub_41095A: ids 40/46/120, all = 1 in the shipped install);
    // GameApp::init re-seeds them from the live VALUELST + options.ini, so
    // these literals only matter for a snapshot never fed through init.
    bool random_start = true;            // getvalue(40) = 1
    int conveyor_speed_index = 1;        // 0 low / 1 medium / 2 high
    bool stomped_bombs_detonate = true;  // getvalue(46) = 1
    bool win_by_kills = false;
    bool goldman = false;
    int enclosement_depth = 1;  // 0..3
    // Row 8, display-only (see file doc) — the currently-loaded scheme's
    // filename, round-tripped through options.ini's `schemefilename=` even
    // though this port has no in-screen `.SCH` browser to change it.
    std::string scheme_filename;
    int playtime_seconds = 150;        // one of kPlayTimeChoices, or 1001 = unlimited
    bool assign_keyboards = false;     // row 10, getvalue-less default per §3 (no seed cited)
    bool diseases_destroyable = true;  // getvalue(120) = 1
    bool lost_net_revert_ai = false;   // row 12
    bool disable_game_music = false;
    bool small_memory = false;  // row 17 backing value; LABEL is inverted, see file doc
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

    // Feed one SDL keycode. CONFIRMED (pseudo.c 9297-9406, activate_row's own
    // file doc in options_screen.cpp): Escape is the ONLY key that ends the
    // screen (done() becomes true) — Enter/Space/Right/Left all just act on
    // the highlighted row, exactly like Left/Right always did, and NEVER
    // exit. Left/Right/Enter/Space on the "Define keyboard layouts" row sets
    // open_keyremap() so the caller can push the KeyRemapScreen on top
    // (§2 — that sub-screen is not part of this class, it edits
    // KeyboardMapper's live bindings directly via the caller).
    void on_key(SDL_Keycode key, AudioEngine& audio);

    // Advances the "cursor1" selection-sprite's self-paced animation by one
    // drawn frame (CONFIRMED sub_413BD6 has its own frame timer, distinct
    // from any gameplay tick — call once per drawn frame, matching
    // goldman_screen.hpp's/screen.hpp's own frame_ pacer convention).
    void tick() { ++cursor_frame_; }

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
    // row was "Define keyboard layouts" and Left/Right/Enter/Space was
    // pressed (all four, per activate_row's file doc). The
    // caller checks this AFTER on_key(), pushes the key-remap screen, then
    // must clear it is not needed — enter() resets it, and it is only ever
    // read once per press in the app's own loop (see game_app.cpp).
    bool open_keyremap() const { return open_keyremap_; }

    const OptionsSnapshot& snapshot() const { return snap_; }

private:
    // The per-row action a Left/Right/Enter/Space press dispatches to
    // (options_screen.cpp's file doc on this function has the full pseudo.c
    // citation): toggles ignore `dir`, cyclers step by `dir`, and the
    // KeyRemap row opens regardless of `dir` — all matching sub_4080DC's own
    // "every direction reaches the same per-row case" dispatch table.
    void activate_row(int dir);

    const AssetStore* assets_ = nullptr;
    const FontTextures* font_ = nullptr;

    std::string backdrop_;  // "GLUE<n>", supplied by the caller's pick_glue()

    int row_ = 0;  // OptionRow, as an int for the wrap arithmetic — always in [0, kCursorRowCount)
    std::uint64_t cursor_frame_ = 0;
    OptionsSnapshot snap_;
    bool changed_ = false;
    bool done_ = false;
    bool open_keyremap_ = false;
    bool goldman_touched_ = false;  // see gold_forfeiting_row_touched()
    bool team_play_touched_ = false;
};

}  // namespace bomber::game
