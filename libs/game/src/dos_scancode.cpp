#include "bomber/game/dos_scancode.hpp"

#include <SDL3/SDL.h>

namespace bomber::game {

namespace {

// One row per DOS/AT set-1 scancode the original can store (the base set
// sub_407AD9's byte_4A2BA0[256] poll reports at index 1..88, plus the
// E0-extended keys folded to 0x80|code — the encoding sub_40614A's arrow
// defaults 200/203/205/208 prove). Order follows the DOS code.
struct DosSdlPair {
    int dos;
    int sdl;
};

constexpr DosSdlPair kDosSdlMap[] = {
    {1, SDL_SCANCODE_ESCAPE},
    {2, SDL_SCANCODE_1},
    {3, SDL_SCANCODE_2},
    {4, SDL_SCANCODE_3},
    {5, SDL_SCANCODE_4},
    {6, SDL_SCANCODE_5},
    {7, SDL_SCANCODE_6},
    {8, SDL_SCANCODE_7},
    {9, SDL_SCANCODE_8},
    {10, SDL_SCANCODE_9},
    {11, SDL_SCANCODE_0},
    {12, SDL_SCANCODE_MINUS},
    {13, SDL_SCANCODE_EQUALS},
    {14, SDL_SCANCODE_BACKSPACE},
    {15, SDL_SCANCODE_TAB},
    {16, SDL_SCANCODE_Q},
    {17, SDL_SCANCODE_W},
    {18, SDL_SCANCODE_E},
    {19, SDL_SCANCODE_R},
    {20, SDL_SCANCODE_T},
    {21, SDL_SCANCODE_Y},
    {22, SDL_SCANCODE_U},
    {23, SDL_SCANCODE_I},
    {24, SDL_SCANCODE_O},
    {25, SDL_SCANCODE_P},
    {26, SDL_SCANCODE_LEFTBRACKET},
    {27, SDL_SCANCODE_RIGHTBRACKET},
    {28, SDL_SCANCODE_RETURN},
    {29, SDL_SCANCODE_LCTRL},
    {30, SDL_SCANCODE_A},
    {31, SDL_SCANCODE_S},
    {32, SDL_SCANCODE_D},
    {33, SDL_SCANCODE_F},
    {34, SDL_SCANCODE_G},
    {35, SDL_SCANCODE_H},
    {36, SDL_SCANCODE_J},
    {37, SDL_SCANCODE_K},
    {38, SDL_SCANCODE_L},
    {39, SDL_SCANCODE_SEMICOLON},
    {40, SDL_SCANCODE_APOSTROPHE},
    {41, SDL_SCANCODE_GRAVE},
    {42, SDL_SCANCODE_LSHIFT},
    {43, SDL_SCANCODE_BACKSLASH},
    {44, SDL_SCANCODE_Z},
    {45, SDL_SCANCODE_X},
    {46, SDL_SCANCODE_C},
    {47, SDL_SCANCODE_V},
    {48, SDL_SCANCODE_B},
    {49, SDL_SCANCODE_N},
    {50, SDL_SCANCODE_M},
    {51, SDL_SCANCODE_COMMA},
    {52, SDL_SCANCODE_PERIOD},
    {53, SDL_SCANCODE_SLASH},
    {54, SDL_SCANCODE_RSHIFT},
    {55, SDL_SCANCODE_KP_MULTIPLY},
    {56, SDL_SCANCODE_LALT},
    {57, SDL_SCANCODE_SPACE},
    {58, SDL_SCANCODE_CAPSLOCK},
    {59, SDL_SCANCODE_F1},
    {60, SDL_SCANCODE_F2},
    {61, SDL_SCANCODE_F3},
    {62, SDL_SCANCODE_F4},
    {63, SDL_SCANCODE_F5},
    {64, SDL_SCANCODE_F6},
    {65, SDL_SCANCODE_F7},
    {66, SDL_SCANCODE_F8},
    {67, SDL_SCANCODE_F9},
    {68, SDL_SCANCODE_F10},
    {69, SDL_SCANCODE_NUMLOCKCLEAR},
    {70, SDL_SCANCODE_SCROLLLOCK},
    {71, SDL_SCANCODE_KP_7},
    {72, SDL_SCANCODE_KP_8},
    {73, SDL_SCANCODE_KP_9},
    {74, SDL_SCANCODE_KP_MINUS},
    {75, SDL_SCANCODE_KP_4},
    {76, SDL_SCANCODE_KP_5},
    {77, SDL_SCANCODE_KP_6},
    {78, SDL_SCANCODE_KP_PLUS},
    {79, SDL_SCANCODE_KP_1},
    {80, SDL_SCANCODE_KP_2},
    {81, SDL_SCANCODE_KP_3},
    {82, SDL_SCANCODE_KP_0},
    {83, SDL_SCANCODE_KP_PERIOD},
    {87, SDL_SCANCODE_F11},
    {88, SDL_SCANCODE_F12},
    // E0-extended keys, stored 0x80|code (the arrows are the ones the
    // shipped defaults/name-table aliasing prove; the rest follow the same
    // driver fold).
    {0x80 | 28, SDL_SCANCODE_KP_ENTER},   // 156
    {0x80 | 29, SDL_SCANCODE_RCTRL},      // 157
    {0x80 | 53, SDL_SCANCODE_KP_DIVIDE},  // 181
    {0x80 | 56, SDL_SCANCODE_RALT},       // 184
    {0x80 | 71, SDL_SCANCODE_HOME},       // 199
    {0x80 | 72, SDL_SCANCODE_UP},         // 200
    {0x80 | 73, SDL_SCANCODE_PAGEUP},     // 201
    {0x80 | 75, SDL_SCANCODE_LEFT},       // 203
    {0x80 | 77, SDL_SCANCODE_RIGHT},      // 205
    {0x80 | 79, SDL_SCANCODE_END},        // 207
    {0x80 | 80, SDL_SCANCODE_DOWN},       // 208
    {0x80 | 81, SDL_SCANCODE_PAGEDOWN},   // 209
    {0x80 | 82, SDL_SCANCODE_INSERT},     // 210
    {0x80 | 83, SDL_SCANCODE_DELETE},     // 211
};

}  // namespace

int dos_scancode_from_sdl(int sdl_scancode) {
    for (const auto& p : kDosSdlMap)
        if (p.sdl == sdl_scancode) return p.dos;
    return 0;
}

int sdl_scancode_from_dos(int dos_scancode) {
    for (const auto& p : kDosSdlMap)
        if (p.dos == dos_scancode) return p.sdl;
    return 0;
}

}  // namespace bomber::game
