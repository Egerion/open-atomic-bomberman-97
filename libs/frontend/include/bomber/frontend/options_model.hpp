#pragma once

#include <cstdint>
#include <string>

// The Options SETTINGS MODEL, SDL-free and screen-free: the row enum, the
// snapshot every screen and the match runner read, and the play-time chain.
// Split out of options_screen.hpp, which was simultaneously this model and an SDL
// screen, so five downstream headers pulled SDL in to name one plain struct.
//
// Per-row disposition, chrome pins and key dispatch: docs/frontend-options-rows.md.

namespace bomber::game {

// Row index == the case index of sub_4080DC's per-row dispatch switch, so the
// order is the original's and may not be sorted or extended in the middle.
enum class OptionRow : std::uint8_t {
    TeamPlay,
    RandomStart,
    NodeName,  // opens the node-name text-entry prompt (sub_4074DC)
    ConveyorSpeed,
    StompedBombs,
    WinByKills,
    GoldBomberman,
    EnclosementDepth,
    SchemeFile,  // opens the *.SCH picker (sub_407582)
    PlayTime,
    AssignKeyboard,
    DiseasesDestroy,
    LostNetRevertAI,
    DisableMusic,
    Modem,  // display-only
    KeyRemap,
    NetProtocol,  // display-only
    SmallMemory,
    kCount,
};

// CONFIRMED literal (pseudo.c 9086): 18 rows, cursor and draw alike. An earlier
// "19 drawn, row 18 unreachable" reading was retracted — see the docs page.
inline constexpr int kCursorRowCount = 18;

// Every editable/displayable setting, passed in on OptionsScreen::enter() and
// read back once done().
struct OptionsSnapshot {
    bool team_play = false;
    // The three VALUELST-seeded toggles carry the original's getvalue seeds
    // (sub_41095A). Init re-seeds from the live VALUELST + options.ini, so these
    // literals only matter for a snapshot never fed through it.
    bool random_start = true;            // getvalue(40) = 1
    int conveyor_speed_index = 1;        // 0 low / 1 medium / 2 high
    bool stomped_bombs_detonate = true;  // getvalue(46) = 1
    bool win_by_kills = false;
    bool goldman = false;
    int enclosement_depth = 1;  // 0..3
    // Row 8 (schemefilename=, byte_4648C4), shown VERBATIM — the picker, not the
    // display, is what normalises it.
    std::string scheme_filename;
    int playtime_seconds = 150;  // one of kPlayTimeChoices, or the unlimited sentinel
    bool assign_keyboards = false;
    bool diseases_destroyable = true;  // getvalue(120) = 1
    bool lost_net_revert_ai = false;
    bool disable_game_music = false;
    bool small_memory = false;  // row 17; the LABEL is the inverse of this value
    // Row 2 (sub_40FE34's runtime buffer unk_460140): lives in its own
    // install-root NODENAME.INI, NOT in options.ini's 22 keys.
    std::string node_name;
    // Row 14, display-only: options.ini modemport=/modemirq=/modembaud=/
    // modemdial=. Defaults are the shipped install's values.
    int modemport = 2;
    int modemirq = 3;
    int modembaud = 19200;
    std::string modemdial = "555-1212";
};

// The CONFIRMED fixed chain (sub_4076FE, pseudo.c 8489-8562), wrapping; 1001 is
// the "Infinite" sentinel (rendered via getstring(280)).
inline constexpr int kPlayTimeChoices[] = {60, 90, 120, 150, 180, 240, 300, 600, 1001};
inline constexpr int kPlayTimeChoiceCount =
    static_cast<int>(sizeof(kPlayTimeChoices) / sizeof(kPlayTimeChoices[0]));

// The "Infinite" sentinel and the finite clock the sim gets instead: ticks_left is
// a plain countdown, so "Infinite" is a clock long enough never to expire while
// the HUD is told separately to hide it. BOTH HALVES MUST AGREE, and ONLINE they
// must agree ACROSS PEERS — the netplay path once hardcoded "not untimed", so a
// host playing Infinite left every peer watching a 27-hour countdown tick down.
// Only tuning.game_seconds crosses the wire, so the receiving side recovers the
// intent by comparing against it; hence names rather than literals at each site.
inline constexpr int kPlayTimeUnlimited = 1001;
inline constexpr int kUnlimitedGameSeconds = 99999;
inline constexpr bool is_unlimited_game_seconds(int game_seconds) {
    return game_seconds >= kUnlimitedGameSeconds;
}

}  // namespace bomber::game
