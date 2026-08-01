#include "bomber/frontend/boot_screen.hpp"

#include <cstdint>

#include "bomber/frontend/asset_screen.hpp"
#include "bomber/ui/screen.hpp"  // ScreenDef

namespace bomber::game {

namespace {

constexpr int kBootMusicId = 1000;  // 0x3E8 — TITLE.RSS, the continuous boot track
constexpr int kTitleStingLo = 2800;  // group base; the run itself ends at 2810
constexpr std::uint32_t kBootDwellMs = 7000;  // getvalue(12) == 7 s

ScreenDef logo_screen(const char* bg) {
    return ScreenDef{bg, {}, /*dwell_ms*/ kBootDwellMs, /*skippable*/ true};
}
ScreenDef title_screen() {
    return ScreenDef{"TITLE", {}, /*dwell_ms*/ kBootDwellMs, /*skippable*/ true};
}

}  // namespace

// The boot presentation (sub_42B060 @0x42B060) — STRAIGHT-LINE, no loop, and no
// attract re-run of the logos or title. Each screen advances on a key OR the
// getvalue(12) = 7 s timeout (which synthesizes Enter), and after the title the
// routine simply returns so the caller drops into the menu.
//
// TWO rules here are easy to break and silent when broken:
//
//  - The boot music is started ONCE and plays CONTINUOUSLY across the logos and
//    the title — the logos are NOT silent. start_music replaces the current track,
//    so a per-screen call would restart it every time.
//  - Escape ADVANCES one screen like every other accept key. present_asset_screen
//    maps Escape to Back, but sub_42B060 treats it as an advance, so a single
//    Escape on IPLOGO must step to HSLOGO rather than short-circuit the whole
//    chain. Only Quit short-circuits.
//
// The title sting is a group pick over the eleven "ATOMIC BOMBERMAN!" takes at
// 2800, none of which the load-time cull touches, so the first pick at boot is a
// flat 1-in-11 (four launches of the original picked ZAI08A, GEN8C2, GEN8A,
// ZAI08F). It does NOT block: the art appears at once and the sting plays over it.
AppInput BootScreen::run() {
    ctx_.audio.start_music(kBootMusicId);
    AppInput ev = present_asset_screen(ctx_, logo_screen("IPLOGO"));
    if (ev == AppInput::Quit) return ev;
    ev = present_asset_screen(ctx_, logo_screen("HSLOGO"));
    if (ev == AppInput::Quit) return ev;
    ctx_.audio.play_sting(kTitleStingLo);
    ev = present_asset_screen(ctx_, title_screen());
    if (ev == AppInput::Quit || ev == AppInput::Back) return ev;
    return AppInput::Advance;  // key OR 7 s timeout -> the caller enters the menu
}

}  // namespace bomber::game
