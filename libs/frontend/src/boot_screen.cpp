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

AppInput BootScreen::run() {
    // The boot presentation (sub_42B060 @0x42B060) — STRAIGHT-LINE, no loop.
    // The original's exact order:
    //   sub_42741E(0x3E8)         ; start the boot music (1000) FIRST of all
    //   if (!sub_413D01()) {      ; skip-logos gate
    //       show IPLOGO           ; sub_42A088(aIplogo, 1), a waited screen
    //       show HSLOGO           ; sub_42A088(aHslogo, 1), a waited screen
    //   }
    //   sub_427BFB(2800)          ; the one-shot title intro sting, before TITLE
    //   show TITLE                ; sub_42A088(aTitle, 1), a waited screen
    //   return                    ; caller enters the menu (sub_42B9CE)
    // sub_42B060 does NOT loop: each screen advances on a key OR the getvalue(12)
    // = 7 s timeout (which synthesizes Enter, 13), and after the title it simply
    // returns so the caller drops into the menu. There is NO attract re-run of
    // the logos/title. So the port is linear: present each screen; a plain
    // Advance (key accept OR the 7 s dwell) walks to the next; the title's
    // Advance returns to run_app, which enters present_menu and switches to the
    // 1010 menu music. Only Back/Quit short-circuit out.
    //
    // The boot music is started ONCE here and plays CONTINUOUSLY across the
    // logos and the title — the logos are NOT silent. We must not (re)start the
    // track per screen: start_music replaces the current track (sub_427342 frees
    // it first), so a per-screen call would restart the boot music every time.
    ctx_.audio.start_music(kBootMusicId);

    // Escape ADVANCES one screen like every other accept key (present_screen
    // maps Escape->Back, but sub_42B060 treats it as an advance): a single
    // Escape on IPLOGO must step to HSLOGO, not short-circuit the whole boot
    // chain into the menu. So Back falls through to the next present_asset_screen
    // here — only Quit (window close) short-circuits.
    AppInput ev = present_asset_screen(ctx_, logo_screen("IPLOGO"));
    if (ev == AppInput::Quit) return ev;
    ev = present_asset_screen(ctx_, logo_screen("HSLOGO"));
    if (ev == AppInput::Quit) return ev;

    // The one-shot title intro sting, fired right before the title image. In the
    // binary this is sub_427BFB(2800): a group pick over the contiguous SOUNDLST
    // run starting at 2800 — the eleven "ATOMIC BOMBERMAN!" takes GEN8A, GEN8B,
    // GEN8C, GEN8C2, ZAI08A..ZAI08G (the file's own "2899 is the last intro"
    // comment bounds the block). The 2800 block is NOT in the load-time cull
    // table, so all eleven survive every launch, and the play counters are zero
    // at boot — which makes this first pick a flat 1-in-11. Four consecutive
    // launches of the original picked ZAI08A, GEN8C2, GEN8A, ZAI08F.
    //
    // sub_427BFB does NOT block: the binary's other use of it (the menu quit
    // sting) is followed by an explicit Sleep(4000) precisely because playback
    // keeps running after the call returns. So the title art appears immediately
    // and the sting plays over it, as here.
    ctx_.audio.play_sting(kTitleStingLo);

    // The title: a normal waited screen. present_asset_screen returns Advance on
    // a real accept OR the 7 s timeout — both fall through to the menu here,
    // faithful to sub_42B060 synthesizing Enter on timeout and returning. Back
    // (Escape) exits the app; Quit closes the window.
    ev = present_asset_screen(ctx_, title_screen());
    if (ev == AppInput::Quit || ev == AppInput::Back) return ev;
    return AppInput::Advance;  // key OR 7 s timeout -> caller enters the menu
}

}  // namespace bomber::game
