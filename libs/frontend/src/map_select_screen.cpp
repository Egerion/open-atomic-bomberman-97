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

// The generic level-name fallback (RANDOM is getstring(149)). The list, count and
// fallback names all come from the level registry, so a custom map added there
// shows up here with no edit to this screen; the displayed name still comes from
// the user's MESSAGES.TXT at runtime and is never committed.
const char* level_fallback(const match::LevelRegistry& levels, int idx) {
    const match::LevelDef* def = levels.find(idx);
    return def != nullptr ? def->name_fallback.c_str() : "LEVEL";
}

// The screen's VALUELST-driven geometry, read once on entry — a parameter object
// (§3) in place of twelve consecutive locals. Cell pitch is the same 40x36 the
// in-match renderer uses, 1:1, no stretching.
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

// The screen's frame loop as its own object. std::optional<AppInput> means
// nullopt = keep looping, a value = run() returns it now.
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
          // For the stock 11 built-ins this equals the original getvalue(35)=11,
          // so the cycle bounds and the RANDOM per-cell pick are unchanged.
          level_count_(static_cast<int>(ctx.assets.levels().all().size())),
          // WORKING COPIES (sub_406DDE 8092-8093): edits touch only these,
          // Enter/Space commits them and Escape DISCARDS them. The old in-place
          // member edits leaked cancelled changes into the next visit.
          level_(state.selected_level),
          wins_(state.win_target),
          // Accept debounce (8100/8220-8228): IGNORED until 1 s after entry or the
          // last value change — the original's guard against a held Enter from the
          // previous screen committing instantly.
          accept_after_ms_(SDL_GetTicks() + 1000),
          // tile_of[row][col]: the stage index whose solid/brick art that cell
          // draws, or -1 for a blank cell.
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
    int roll_cell(int col, int row, int max_n);
    void roll_pattern();
    void draw_frame();
    void draw_backdrop();
    void draw_preview();
    void draw_preview_cells();
    void draw_preview_cell(int col, int row);
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
    // §7's read-only gate — CORRECTED 2026-07-28: there is NO navigation
    // carve-out. Every arm of sub_406DDE's dispatch buzzes on a guest, Up/Down and
    // F1 included; the ONLY ungated key is Escape (docs/frontend-setup-screens.md).
    if (net_guest_) {
        ctx_.audio.play(40);
        return std::nullopt;
    }
    on_value_key(k);
    return std::nullopt;
}

// sub_406DDE's Esc handler (pseudo.c 8186-8191) is called straight from
// sub_410F81's TAIL with NO loop back to the player screen, so this aborts the
// WHOLE Play flow to the menu — NOT "back one screen" — and forfeits any pending
// gold player. The working level/wins copies are simply dropped.
AppInput MapSelectLoop::on_escape() {
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

// The HOST broadcasts each change on the EDIT, not every frame. `rounds = wins`
// (never 0 here — the row clamps to 1..100) is also what tells the guest the host
// has LEFT the roster screen, so the first publish fires on entry.
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
        // COMMIT the mirrored working copies. The WIN TARGET is not part of the
        // confirmed MatchConfig, so without this the guest ran the host's board
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

// One preview cell: -1 for blank, else the stage index whose tileset it draws.
// LCG-ORDER-SENSITIVE — 0..2 draws, brick roll first and the RANDOM per-cell
// stage pick second, over a row-major walk. Every screen shares one LCG.
int MapSelectLoop::roll_cell(int col, int row, int max_n) {
    const bool solid = is_solid_cell(col, row);
    bool brick_cell = false;
    if (!solid && (col > 1 || row > 1)) {
        state_.setup_lcg = state_.setup_lcg * 1664525u + 1013904223u;
        brick_cell = (state_.setup_lcg >> 16) % 5 != 0;  // rand()%5 != 0
    }
    if (!solid && !brick_cell) return -1;
    if (level_ >= 0) return level_;
    // RANDOM: re-pick per cell (pinned quirk).
    state_.setup_lcg = state_.setup_lcg * 1664525u + 1013904223u;
    return static_cast<int>((state_.setup_lcg >> 16) % static_cast<unsigned>(max_n));
}

// Re-rolled on entry and on a LEVEL change (sub_406AA3 re-arms its own flag) —
// NEVER every frame. Solid/brick-ness is re-derived at draw time from the parity
// rule, so what is rolled is only WHICH cells carry art and whose tileset.
void MapSelectLoop::roll_pattern() {
    pattern_level_ = level_;
    const int max_n = level_count_ > 1 ? level_count_ : 1;
    for (int cell = 0; cell < layout_.pysize * layout_.pxsize; ++cell) {
        const int row = cell / layout_.pxsize;
        const int col = cell % layout_.pxsize;
        tile_of_[static_cast<std::size_t>(row)][static_cast<std::size_t>(col)] =
            roll_cell(col, row, max_n);
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

// Sample-block preview panel (sub_406AA3, level & rounds audit 2026-07-12).
// Border fill is the general WHITE byte_49D38F, NOT the old invented (40,40,60);
// the field swatch is a 1:1 CROP of FIELDn.PCX starting 48 rows down and NOT a
// stretch. docs/frontend-setup-screens.md "The sample-block preview".
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
    for (int cell = 0; cell < layout_.pysize * layout_.pxsize; ++cell)
        draw_preview_cell(cell % layout_.pxsize, cell / layout_.pxsize);
}

void MapSelectLoop::draw_preview_cell(int col, int row) {
    const int n = tile_of_[static_cast<std::size_t>(row)][static_cast<std::size_t>(col)];
    if (n < 0) return;
    const AssetStore::StagePreview& prev = ctx_.assets.stage_preview(n);
    const Anim& a = is_solid_cell(col, row) ? prev.solid : prev.brick;
    if (a.steps.empty()) return;
    const Sprite& sp = a.steps[0];
    if (sp.tex == nullptr) return;
    SDL_FRect dst{static_cast<float>(layout_.px + col * sim::kTileW),
                  static_cast<float>(layout_.py + row * sim::kTileH),
                  static_cast<float>(sim::kTileW), static_cast<float>(sim::kTileH)};
    SDL_RenderTexture(ctx_.sdl, sp.tex, nullptr, &dst);
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

// Both rows in the SAME white ink over black; the original marks the active row
// with the cursor sprite ALONE, and the old per-row highlight was invented. The
// wins line takes TWO specifiers (pseudo.c 8124-8127) — the old single-%u splice
// left a literal "%s" on screen with the real MESSAGES.TXT.
void MapSelectLoop::draw_rows() {
    const std::string level_line = fmt_s(ctx_.assets.getstring(210, "%s"), displayed_level_name());
    ctx_.front_font.draw_outlined(ctx_.sdl, level_line, layout_.lx, layout_.ly, 240, 248, 252, 0, 0,
                                  0, layout_.lw);
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

// The LEVEL & ROUNDS screen (sub_406DDE @0x406DDE, VALUELST getvalue 730/735) —
// screen 2 of the pre-match flow, presentation only. Rows, keys and the online
// half are in docs/frontend-setup-screens.md. A default NetSetupLink is ordinary
// local play and every net branch is inert.
AppInput MapSelectScreen::run() {
    return MapSelectLoop(ctx_, state_, net_, chat_).run();
}

}  // namespace bomber::game
