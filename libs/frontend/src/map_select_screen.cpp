#include "bomber/frontend/map_select_screen.hpp"

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "bomber/game_util/anim_pace.hpp"      // anim_step_index
#include "bomber/game_util/frontend_util.hpp"  // pick_glue
#include "bomber/game_util/hud_format.hpp"     // fmt_s / fmt_us
#include "bomber/match/level_registry.hpp"     // LevelRegistry / LevelDef
#include "bomber/render/sprites.hpp"           // Sprite, Anim, resolve_sequence
#include "bomber/sim/constants.hpp"            // sim::kTileW / kTileH
#include "bomber/ui/bmscreen.hpp"              // HelpBrowser

namespace bomber::game {

namespace {

// The generic level-name fallback (RANDOM is getstring(149)). The level list,
// count, and per-level fallback names all come from the level registry now
// (ctx_.assets.levels(), seeded with the 11 built-ins named VALUELST 450-460 /
// getstring(150+n)); a name still comes from the user's MESSAGES.TXT at runtime
// via getstring and is never committed. Sourcing from the registry means a
// custom map added there shows up here with no edit to this screen.
const char* level_fallback(const match::LevelRegistry& levels, int idx) {
    const match::LevelDef* def = levels.find(idx);
    return def != nullptr ? def->name_fallback.c_str() : "LEVEL";
}

// The screen's VALUELST-driven geometry, read once on entry — a parameter object
// (§3) in place of the twelve consecutive locals that used to open run().
//
// Row list 735 (X,Y,YS,clip), sample-block preview 730 (X,Y = grid origin,
// XSize/YSize = grid size IN CELLS, 5x5 — docs/re/setup-screens.md "The sample
// block preview", sub_406AA3), footer 790, cursor blink 690. Cell pitch is the
// same 40x36 the in-match renderer uses (sim::kTileW/kTileH), 1:1, no stretching.
struct MapSelectLayout {
    float lx, ly, lys, lw;         // the 2 text rows (735)
    float fcx, ffy;                // footer 790
    int px, py, pxsize, pysize;    // preview grid origin + size in cells (730)
    int blink_base, blink_spread;  // cursor blink base + spread, seconds (690)
};

float column_f(const assets::res::ValueList& values, int id, int column, int fallback) {
    return static_cast<float>(values.column_or(id, column, fallback));
}

MapSelectLayout read_layout(const assets::res::ValueList& v) {
    return MapSelectLayout{column_f(v, 735, 0, 55),
                           column_f(v, 735, 1, 170),
                           column_f(v, 735, 2, 24),
                           column_f(v, 735, 3, 300),
                           column_f(v, 790, 0, 320),
                           column_f(v, 790, 1, 440),
                           static_cast<int>(v.column_or(730, 0, 400)),
                           static_cast<int>(v.column_or(730, 1, 100)),
                           static_cast<int>(v.column_or(730, 2, 5)),
                           static_cast<int>(v.column_or(730, 3, 5)),
                           static_cast<int>(v.column_or(690, 0, 2)),
                           static_cast<int>(v.column_or(690, 1, 2))};
}

// A cell of the 5x5 sample block is SOLID on the (odd,odd) parity, exactly like
// the arena's own pillar rule — pure geometry, so it never needs re-rolling.
bool is_solid_cell(int col, int row) {
    return (col & 1) != 0 && (row & 1) != 0;
}

// The screen's frame loop as its own object (the shape the netplay extraction
// established): the layout, the WORKING level/wins copies, the debounce deadline
// and the rolled preview pattern become members, so each step below is a named
// method. `std::optional<AppInput>` means nullopt = keep looping, a value =
// run() returns it now.
class MapSelectLoop {
public:
    MapSelectLoop(ScreenContext ctx, MapSelectState state, NetSetupLink net, ChatOverlay* chat)
        : ctx_(ctx),
          state_(state),
          net_(net),
          chat_(chat),
          layout_(read_layout(ctx.values)),
          // pick_glue advances the shared presentation LCG and its draw
          // order/count is observable — it stays the first thing the loop does.
          glue_(pick_glue(state.setup_lcg, ctx.values)),
          // Level count from the registry. For the stock 11 built-ins this equals
          // the original getvalue(35)=11, so the cycle bounds and the RANDOM
          // per-cell pick are unchanged; a registered custom map extends the cycle
          // with no edit here.
          level_count_(static_cast<int>(ctx.assets.levels().all().size())),
          // WORKING COPIES (sub_406DDE 8092-8093: dword_45E0B8/45E0B4 seeded from
          // the committed globals on entry): edits touch only these; Enter/Space
          // commits them (LABEL_101, 8261-8271) and Escape DISCARDS them — the old
          // in-place member edits leaked cancelled changes into the next visit.
          level_(state.selected_level),
          wins_(state.win_target),
          // Enter/Space debounce (8100/8220-8228): accept is IGNORED until 1 s
          // (sub_4148AC() = 1 locally) after entry or the last value change — the
          // original's guard against a held Enter from the previous screen
          // committing instantly.
          accept_after_ms_(SDL_GetTicks() + 1000),
          // tile_of[row][col]: the stage index whose "tile <n> solid/brick" art
          // that cell draws, or -1 for a blank cell.
          tile_of_(static_cast<std::size_t>(layout_.pysize),
                   std::vector<int>(static_cast<std::size_t>(layout_.pxsize), -1)),
          net_mode_(net_setup_active(net)),
          net_guest_(net_setup_readonly(net)) {}

    AppInput run();

private:
    // --- the three phases of one frame ---
    std::optional<AppInput> pump_events();
    std::optional<AppInput> pump_link();
    void present_frame();

    // --- event dispatch ---
    std::optional<AppInput> handle_event(const SDL_Event& ev);
    std::optional<AppInput> on_key(SDL_Keycode k);
    AppInput on_escape();
    std::optional<AppInput> on_accept();
    void on_value_key(SDL_Keycode k);
    void step_level(int delta);
    void step_wins(int delta);
    std::optional<AppInput> run_help_browser();

    // --- drawing ---
    void roll_pattern();
    void draw_frame();
    void draw_backdrop();
    void draw_preview();
    void draw_preview_cells();
    void draw_rows();
    std::string local_level_name() const;
    std::string displayed_level_name() const;
    void draw_footer();
    void draw_cursor();

    ScreenContext ctx_;
    MapSelectState state_;
    NetSetupLink net_;
    ChatOverlay* chat_;
    MapSelectLayout layout_;
    std::string glue_;
    // GUEST: the level LABEL the host has on screen, taken verbatim off the wire
    // (SetupPreviewFrame::level_name) rather than re-resolved from this install's
    // getstring(150+n) — so a custom map the guest does not have still reads
    // correctly instead of showing the wrong name or a blank.
    std::string net_level_name_;
    int level_count_;
    int level_;
    int wins_;
    std::uint64_t accept_after_ms_;
    std::vector<std::vector<int>> tile_of_;
    // The sample-block pattern is re-rolled only on screen entry and on a LEVEL
    // row change (sub_406AA3 re-arms its own roll flag there), NEVER every frame
    // — pinned in docs/re/setup-screens.md. -2 is a sentinel forcing the first
    // roll.
    int pattern_level_ = -2;
    int field_stage_ = -1;  // the field-swatch stage picked alongside tile_of_
    int row_ = 0;           // 0 = level, 1 = wins (sub_406DDE navigates exactly 2 rows)
    bool net_mode_;
    bool net_guest_;
    bool net_dirty_ = false;  // a value changed this frame -> re-publish the preview
    bool accepted_ = false;
};

AppInput MapSelectLoop::run() {
    while (true) {
        if (const std::optional<AppInput> exit = pump_events()) return *exit;
        // The accept path deliberately falls THROUGH one more frame rather than
        // returning from the event pump: the original breaks out of the poll loop
        // and still runs the publish/pump/draw tail below before it leaves.
        if (const std::optional<AppInput> exit = pump_link()) return *exit;
        present_frame();
        if (accepted_) return AppInput::Advance;
    }
}

std::optional<AppInput> MapSelectLoop::pump_events() {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (const std::optional<AppInput> exit = handle_event(ev)) return *exit;
        if (accepted_) break;
    }
    return std::nullopt;
}

std::optional<AppInput> MapSelectLoop::handle_event(const SDL_Event& ev) {
    if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
    // The F2 lobby-chat overlay gets first refusal (chat_overlay.hpp — PORT-ONLY,
    // not RE'd). Open, it consumes every key, so typing never also cycles the
    // level behind it; closed, it takes only F2.
    if (chat_ != nullptr && chat_->handle_event(ev, ctx_)) return std::nullopt;
    if (ev.type != SDL_EVENT_KEY_DOWN) return std::nullopt;
    return on_key(ev.key.key);
}

std::optional<AppInput> MapSelectLoop::on_key(SDL_Keycode k) {
    if (k == SDLK_ESCAPE) return on_escape();
    if (k == SDLK_F1) {
        // 0x13B -> sub_41431C (pseudo.c 8208-8216): the same generic *.BM help
        // browser, composited over this screen.
        ctx_.audio.play(20);
        // CORRECTED 2026-07-28: F1 is guest-gated too. sub_406DDE's F1 arm
        // @0x407481 opens with the same `sub_40C06A() == 1` test every other arm
        // uses and buzzes (0x407490) instead of opening the browser.
        if (net_guest_) {
            ctx_.audio.play(40);
            return std::nullopt;
        }
        return run_help_browser();
    }
    if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) return on_accept();
    ctx_.audio.play(20);
    // §7's read-only gate — CORRECTED 2026-07-28. The old comment here claimed
    // "Up/Down (pure navigation) and F1 stay live on a guest; every value-changing
    // key buzzes", citing sub_406DDE. sub_406DDE has no such carve-out: EVERY arm
    // of its dispatch opens with the same `sub_40C06A() == 1` test and the same
    // SFX-40 buzz — Up (0x4071A5), Down (0x4071D6), the 0x174 stepper (0x407209),
    // Right (0x4072B3), Left (0x407355), Enter/Space (0x4073F4), F1 (0x407490)
    // and Alt+D (0x4074AD). The ONLY ungated key is Escape (0x407464), which sets
    // its flags and leaves silently. The host owns this screen completely; a guest
    // can look and leave.
    if (net_guest_) {
        ctx_.audio.play(40);
        return std::nullopt;
    }
    on_value_key(k);
    return std::nullopt;
}

AppInput MapSelectLoop::on_escape() {
    // sub_406DDE's own Esc handler (pseudo.c 8186-8191) is called straight from
    // sub_410F81's TAIL (pseudo.c 15516, gated `if (!dword_464A68)`) with NO loop
    // back to the player screen afterwards — so this aborts the WHOLE Play flow to
    // the menu, exactly like the Goldman wheel's own Esc (doc §5), NOT "back one
    // screen" to present_setup. It also forfeits any pending gold player
    // (`dword_46492C = -1`, doc §2's "Cleared to -1 by" list). The WORKING
    // level/wins copies are simply dropped (8092-8093 re-seed on the next entry) —
    // the committed selections stay untouched.
    ctx_.audio.play(20);
    state_.gold_player = -1;
    return AppInput::Back;
}

// Accept (LABEL_101, 8261-8271) — Space accepts too, and the key first rides the
// any-key blip 20 (8176), then the accept sting 10. Ignored inside the 1 s
// debounce window (8220-8228): the key still blips, nothing commits.
std::optional<AppInput> MapSelectLoop::on_accept() {
    ctx_.audio.play(20);
    // §7: the HOST owns the screen advance (`sub_40F064(902)`, kind 32); a guest
    // pressing Enter lands on LABEL_97's SFX 40.
    if (net_guest_) {
        ctx_.audio.play(40);
        return std::nullopt;
    }
    if (SDL_GetTicks() < accept_after_ms_) return std::nullopt;
    ctx_.audio.play(10);
    state_.selected_level = level_;  // commit the working copies
    state_.win_target = wins_;
    accepted_ = true;
    return std::nullopt;
}

void MapSelectLoop::on_value_key(SDL_Keycode k) {
    if (k == SDLK_UP || k == SDLK_DOWN) {
        row_ = (row_ + 1) % 2;  // 2 rows: either arrow toggles
        return;
    }
    if (row_ == 0) {
        if (k == SDLK_LEFT) step_level(-1);
        if (k == SDLK_RIGHT) step_level(+1);
        return;
    }
    if (k == SDLK_LEFT) step_wins(-1);
    if (k == SDLK_RIGHT) step_wins(+1);
    // +-5 on PgUp/PgDn (batch_0x405B3A.cpp 1097-1128, codes 372/371) — WINS row
    // only. Rebound from the old invented Ctrl+Left/Right, which the original
    // never used.
    if (k == SDLK_PAGEUP) step_wins(+5);
    if (k == SDLK_PAGEDOWN) step_wins(-5);
}

// Level cycle, wrapping [-1 .. level_count-1] where -1 is RANDOM. Every value
// change re-arms the accept debounce (8325).
void MapSelectLoop::step_level(int delta) {
    level_ += delta;
    if (level_ < -1) level_ = level_count_ - 1;
    if (level_ >= level_count_) level_ = -1;
    accept_after_ms_ = SDL_GetTicks() + 1000;
    net_dirty_ = true;
}

void MapSelectLoop::step_wins(int delta) {
    wins_ += delta;
    if (wins_ < 1) wins_ = 1;
    if (wins_ > 100) wins_ = 100;
    accept_after_ms_ = SDL_GetTicks() + 1000;
    net_dirty_ = true;
}

std::optional<AppInput> MapSelectLoop::run_help_browser() {
    HelpBrowser browser(ctx_.assets, ctx_.front_font);
    browser.enter(ctx_.values.at_or(15, 1) != 0);
    while (!browser.done()) {
        SDL_Event hev;
        while (SDL_PollEvent(&hev)) {
            if (hev.type == SDL_EVENT_QUIT) return AppInput::Quit;
            if (hev.type == SDL_EVENT_KEY_DOWN) browser.on_key(hev.key.key, ctx_.audio);
        }
        if (browser.viewing() && browser.viewer().done()) browser.close_viewer();
        net_setup_pump(net_);                 // the link must not go silent under the browser
        if (chat_ != nullptr) chat_->pump();  // nor the lobby's heartbeat
        ctx_.audio.update_music();
        draw_frame();
        browser.draw(ctx_.sdl);
        SDL_RenderPresent(ctx_.sdl);
        SDL_Delay(2);
    }
    return std::nullopt;
}

// The HOST broadcasts each change (§7: level kind 43, rounds kind 44) — on the
// edit, not every frame. `rounds = wins` (never 0 here, the row clamps to 1..100)
// is also what tells the guest the host has LEFT the roster screen
// (net_setup_link.hpp's sentinel), so the first publish fires on entry.
std::optional<AppInput> MapSelectLoop::pump_link() {
    if (net_mode_ && !net_guest_ && (net_dirty_ || !net_setup_on_level_screen(net_))) {
        net_setup_publish_level(net_, level_, local_level_name(), wins_);
        net_dirty_ = false;
    }
    net_setup_pump(net_);
    if (chat_ != nullptr) chat_->pump();
    if (!net_guest_) return std::nullopt;
    // Read-only: the level/rounds ARE the host's newest preview.
    net_setup_apply_level(net_, level_count_, level_, wins_, net_level_name_);
    if (net_setup_final(net_)) {
        // COMMIT the mirrored working copies, exactly as the host's own Enter does
        // above. The WIN TARGET is not part of the confirmed MatchConfig (it is a
        // front-end match-scope value, the original's dword_464A7C, broadcast as
        // its own kind-44 message), so without this the guest ran the host's board
        // with ITS OWN stale target and the two peers disagreed about when the
        // match was over — one starting round N+1 while the other showed VICTORY.
        state_.selected_level = level_;
        state_.win_target = wins_;
        return AppInput::Advance;
    }
    // Timed out / the host vanished. Back returns to the caller, which reads
    // net_setup_failed() to tell this from an Esc.
    if (net_setup_failed(net_)) return AppInput::Back;
    return std::nullopt;
}

void MapSelectLoop::present_frame() {
    ctx_.audio.update_music();
    draw_frame();
    if (chat_ != nullptr) chat_->draw(ctx_);  // last: the panel sits on top
    SDL_RenderPresent(ctx_.sdl);
    SDL_Delay(2);
}

// Re-roll the sample-block pattern on entry and whenever the LEVEL changes
// (sub_406AA3 re-arms its own roll flag) — never every frame. Solid/brick-ness
// itself is re-derived at draw time from the (col&1,row&1) parity rule, which is
// pure geometry and does not need rolling; what is rolled is WHICH cells carry
// art and which level's tileset each drawn cell uses.
void MapSelectLoop::roll_pattern() {
    pattern_level_ = level_;
    const int max_n = level_count_ > 1 ? level_count_ : 1;
    for (int i = 0; i < layout_.pysize; ++i) {
        for (int j = 0; j < layout_.pxsize; ++j) {
            bool brick_cell = false;
            if (!is_solid_cell(j, i) && (j > 1 || i > 1)) {
                state_.setup_lcg = state_.setup_lcg * 1664525u + 1013904223u;
                brick_cell = (state_.setup_lcg >> 16) % 5 != 0;  // rand()%5 != 0
            }
            int n = -1;
            if (is_solid_cell(j, i) || brick_cell) {
                n = level_;
                if (n < 0) {  // RANDOM: re-pick per cell (pinned quirk)
                    state_.setup_lcg = state_.setup_lcg * 1664525u + 1013904223u;
                    n = static_cast<int>((state_.setup_lcg >> 16) % static_cast<unsigned>(max_n));
                }
            }
            tile_of_[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)] = n;
        }
    }
    field_stage_ = level_;
    if (field_stage_ >= 0) return;
    state_.setup_lcg = state_.setup_lcg * 1664525u + 1013904223u;
    field_stage_ = static_cast<int>((state_.setup_lcg >> 16) % static_cast<unsigned>(max_n));
}

// One frame of the screen (sub_406DDE's per-frame body 8107-8153).
void MapSelectLoop::draw_frame() {
    if (level_ != pattern_level_) roll_pattern();
    draw_backdrop();
    draw_preview();
    draw_rows();
    draw_footer();
    draw_cursor();
}

void MapSelectLoop::draw_backdrop() {
    SDL_SetRenderDrawColor(ctx_.sdl, 0, 0, 0, 255);
    SDL_RenderClear(ctx_.sdl);
    const Sprite& bg = ctx_.assets.frontend_pcx(glue_);
    if (bg.tex == nullptr) return;
    SDL_FRect dst{0.0f, 0.0f, static_cast<float>(bg.w), static_cast<float>(bg.h)};
    SDL_RenderTexture(ctx_.sdl, bg.tex, nullptr, &dst);
}

// Sample-block preview panel (sub_406AA3, level&rounds audit 2026-07-12): border
// fill = the general WHITE byte_49D38F — (240,248,252), NOT the old invented
// (40,40,60) — at (378,80, 224x202); the field swatch is a 1:1 CROP of FIELDn.PCX
// starting 48 rows down (the source is taken 12*640 int-sized steps into the
// 640-byte-wide bitmap — 4 bytes a step, so 48 scanlines; NOT a stretch —
// sub_4428B4 is a plain rect copy), 220x198 at (380,82), which lines the
// backdrop's own board grid up under the drawn tiles; then the 5x5 solid/brick
// grid at native 40x36 cells.
void MapSelectLoop::draw_preview() {
    const float bx = static_cast<float>(layout_.px - 22);
    const float by = static_cast<float>(layout_.py - 20);
    const float bw = static_cast<float>(layout_.pxsize * sim::kTileW + 24);
    const float bh = static_cast<float>(layout_.pysize * sim::kTileH + 22);
    SDL_SetRenderDrawColor(ctx_.sdl, 240, 248, 252, 255);
    SDL_FRect border{bx, by, bw, bh};
    SDL_RenderFillRect(ctx_.sdl, &border);
    if (field_stage_ >= 0) {
        const AssetStore::StagePreview& fprev = ctx_.assets.stage_preview(field_stage_);
        if (fprev.field != nullptr) {
            const float sw = static_cast<float>(layout_.pxsize * sim::kTileW + 20);
            const float sh = static_cast<float>(layout_.pysize * sim::kTileH + 18);
            SDL_FRect src{0.0f, 48.0f, sw, sh};
            SDL_FRect panel{static_cast<float>(layout_.px - 20),
                            static_cast<float>(layout_.py - 18), sw, sh};
            SDL_RenderTexture(ctx_.sdl, fprev.field, &src, &panel);
        }
    }
    draw_preview_cells();
}

void MapSelectLoop::draw_preview_cells() {
    for (int i = 0; i < layout_.pysize; ++i) {
        for (int j = 0; j < layout_.pxsize; ++j) {
            const int n = tile_of_[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)];
            if (n < 0) continue;
            const AssetStore::StagePreview& prev = ctx_.assets.stage_preview(n);
            const Anim& a = is_solid_cell(j, i) ? prev.solid : prev.brick;
            if (a.steps.empty()) continue;
            const Sprite& sp = a.steps[0];
            if (sp.tex == nullptr) continue;
            SDL_FRect cell{static_cast<float>(layout_.px + j * sim::kTileW),
                           static_cast<float>(layout_.py + i * sim::kTileH),
                           static_cast<float>(sim::kTileW), static_cast<float>(sim::kTileH)};
            SDL_RenderTexture(ctx_.sdl, sp.tex, nullptr, &cell);
        }
    }
}

// This install's own name for the working level — getstring(150+n), or
// getstring(149) for RANDOM. What the HOST publishes on the wire.
std::string MapSelectLoop::local_level_name() const {
    if (level_ < 0) return ctx_.assets.getstring(149, "Random Each Game");
    return ctx_.assets.getstring(150 + level_, level_fallback(ctx_.assets.levels(), level_));
}

// The label this screen DRAWS: a guest shows the host's wire label verbatim so a
// custom map it does not have still reads correctly; everyone else shows their
// own. net_level_name_ is only ever written on the guest path, so the host draws
// its local name here either way.
std::string MapSelectLoop::displayed_level_name() const {
    return net_level_name_.empty() ? local_level_name() : net_level_name_;
}

// Both text rows in the SAME white ink + black outline (sub_41696C; byte_49D38F
// decodes to (240,248,252) — LUT 0x7FFF -> idx 72, level&rounds audit), clip
// getvalue(738)=300. The original marks the active row with the cursor sprite
// ALONE — the old per-row colour highlight and the grey key legend were invented.
void MapSelectLoop::draw_rows() {
    const std::string level_line = fmt_s(ctx_.assets.getstring(210, "%s"), displayed_level_name());
    ctx_.front_font.draw_outlined(ctx_.sdl, level_line, layout_.lx, layout_.ly, 240, 248, 252, 0, 0,
                                  0, layout_.lw);
    // Wins line: getstring(211) "%u %s to win match" with the %s picked by the
    // "win by kills" option — getstring(208) "Wins" / getstring(209) "Kills"
    // (pseudo.c 8124-8127; the old single-%u splice left a literal "%s" on screen
    // with the real MESSAGES.TXT).
    const std::string wins_word = ctx_.assets.getstring(
        state_.options.win_by_kills ? 209 : 208, state_.options.win_by_kills ? "Kills" : "Wins");
    const std::string wins_line =
        fmt_us(ctx_.assets.getstring(211, "%u %s to win match"), wins_, wins_word);
    ctx_.front_font.draw_outlined(ctx_.sdl, wins_line, layout_.lx, layout_.ly + layout_.lys, 240,
                                  248, 252, 0, 0, 0, layout_.lw);
}

// Footer (sub_413FB9): centred cyan "Press F1 for help".
void MapSelectLoop::draw_footer() {
    const std::string help = ctx_.assets.getstring(330, "Press F1 for help");
    const float help_w = static_cast<float>(ctx_.front_font.measure(help));
    ctx_.front_font.draw_outlined(ctx_.sdl, help, layout_.fcx - (help_w + 2.0f) / 2.0f, layout_.ffy,
                                  96, 252, 252, 0, 0, 0);
}

// The bomber-dude row cursor (sub_413BD6 at 8140-8141): (getvalue(735) - 20,
// row_y + 16) — the +16 is the empirically pinned anchor nudge (measured off THIS
// screen's native capture; see options_screen.cpp).
void MapSelectLoop::draw_cursor() {
    const Anim cur = resolve_sequence(ctx_.assets.misc(), "cursor1");
    if (cur.steps.empty()) return;
    const std::size_t st = ctx_.cursor_blink.step(SDL_GetTicks() / 1000ull, cur.steps.size(),
                                                  layout_.blink_base, layout_.blink_spread);
    const Sprite& sp = cur.steps[anim_step_index(st, cur.steps.size())];
    if (sp.tex == nullptr) return;
    SDL_FRect d{
        layout_.lx - 20.0f - static_cast<float>(sp.hx),
        layout_.ly + layout_.lys * static_cast<float>(row_) + 16.0f - static_cast<float>(sp.hy),
        static_cast<float>(sp.w), static_cast<float>(sp.h)};
    SDL_RenderTexture(ctx_.sdl, sp.tex, nullptr, &d);
}

}  // namespace

// The LEVEL & ROUNDS screen (sub_406DDE @0x406DDE, the VALUELST "OPTIONS SCREEN"
// getvalue 730/735). Screen 2 of the pre-match flow. A 2-row list on a random
// GLUE<n> backdrop (1020 track inherited): row 0 = LEVEL (-1 RANDOM else 0..10 of
// getvalue(35)=11 built-ins, named getstring(150+n) / getstring(149)); row 1 =
// NUMBER OF WINS (1..100). Left/Right cycle the highlighted row's value (level
// wraps [-1 .. 10]; wins +-1 or +-5 on PgUp/PgDn), Up/Down switch rows. Enter
// commits the level (state_.selected_level -> dword_464998) and win target
// (state_.win_target -> dword_464A7C) and starts; Escape backs to the player
// screen. Presentation only — the committed level drives start_match's stage
// choice.
//
// ONLINE LEVEL & ROUNDS (docs/re/network-screens.md §7, net_setup_link.hpp): the
// same screen wired to the host-authoritative setup session. `net_mode` false is
// the ordinary local path and every branch below it is inert.
AppInput MapSelectScreen::run() {
    return MapSelectLoop(ctx_, state_, net_, chat_).run();
}

}  // namespace bomber::game
