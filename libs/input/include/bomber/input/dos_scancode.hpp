#pragma once

// The original's raw keyboard scancode space (docs/re/results-and-options.md
// §2). BM95.EXE stores key bindings — the `keydef=<set>,<action>,<scancode>`
// options.ini triples, sub_40614A's defaults, sub_407AD9's capture — as DOS/AT
// "set 1" scancodes: the base set 1..88 plus E0-extended keys folded to
// `0x80 | code` (Up = 200, Left = 203, Right = 205, Down = 208). This port
// keeps SDL_Scancode in its live KeySet (input.hpp) and translates at the two
// boundaries where a shared install's options.ini must stay interchangeable
// with BM95.EXE: the `keydef=` read/write, and the key-remap screen's names.
//
// Key NAMES are the original's own 0x59-entry table (off_45B914 @ 0x45B914,
// entries at 0x45867E.., read from the data section 2026-07-13).

namespace bomber::game {

// off_45B914's exhaustive content (0x59 == 89 entries).
inline constexpr int kDosKeyNameCount = 0x59;
inline constexpr const char* kDosKeyNames[kDosKeyNameCount] = {
    "",        "Esc",    "1",      "2",      "3",       "4",      "5",       "6",
    "7",       "8",      "9",      "0",      "-_",      "+=",     "BS",      "Tab",
    "Q",       "W",      "E",      "R",      "T",       "Y",      "U",       "I",
    "O",       "P",      "[{",     "]}",     "Enter",   "Ctrl",   "A",       "S",
    "D",       "F",      "G",      "H",      "J",       "K",      "L",       ";",
    "'",       "`~",     "LShift", "\\|",    "Z",       "X",      "C",       "V",
    "B",       "N",      "M",      ",<",     ".>",      "/?",     "RShift",  "(*)",
    "Alt",     "Space",  "Caps",   "F1",     "F2",      "F3",     "F4",      "F5",
    "F6",      "F7",     "F8",     "F9",     "F10",     "Num",    "Scroll",  "(7)Home",
    "(8)Up",   "(9)PgUp", "(-)",   "(4)Left", "(5)",    "(6)Right", "(+)",   "(1)End",
    "(2)Down", "(3)PgDn", "(0)Ins", "(.)Del", "",       "",       "",        "F11",
    "F12",
};

// sub_407B9D's display rule (pseudo.c 8743-8744): mask to 7 bits, name only if
// inside the table — nullptr means "draw no name line at all". The mask is why
// the extended arrows alias their numpad names ("(8)Up", "(4)Left"), which is
// what the real Define-keys screen shows. Entry 0 and the 84..86 gap are empty
// strings IN the table, so an unbound 0 shows "Key: ''" rather than no line.
inline const char* dos_scancode_name(int dos_scancode) {
    const int masked = dos_scancode & 0x7F;
    if (masked < 0 || masked >= kDosKeyNameCount) return nullptr;
    return kDosKeyNames[masked];
}

// DOS/AT set-1 scancode <-> SDL_Scancode (as plain int). Unmappable inputs
// return 0 (DOS 0 / SDL_SCANCODE_UNKNOWN — the "unbound" value in both
// spaces). Implemented in dos_scancode.cpp (needs the SDL_SCANCODE_ enum).
int dos_scancode_from_sdl(int sdl_scancode);
int sdl_scancode_from_dos(int dos_scancode);

}  // namespace bomber::game
