#include "bomber/netui/chat_overlay.hpp"

#if defined(BOMBER_HAS_LOBBY)

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "bomber/net/lobby_flow.hpp"    // net::LobbyFlow, net::ChatLine
#include "bomber/ui/dialog_chrome.hpp"  // the chrome inks + draw_dialog_text

namespace bomber::game {

namespace {

// THE TOGGLE KEY: F2. The collision survey behind that choice, since every other
// obvious candidate is already spoken for on the screens this appears over:
//   * Tab, F7, F8, F9, F11 and Alt+Enter are eaten by GameApp's global
//     SDL_EventFilter and never reach a screen at all;
//   * F1 is the RE'd help browser on BOTH setup screens (sub_410F81 / sub_406DDE);
//   * Enter/Space accept, Esc cancels, and the arrows navigate, everywhere;
//   * on the roster screen T toggles teams, C×5 arms the campaign trigger, and
//     0/O clear a slot; in the waiting room R and Space toggle ready; in the
//     public browser R/F5 refresh.
// F2 is free in all of them and — unlike a letter or the backquote — means the
// same physical key on every keyboard layout.
constexpr SDL_Keycode kToggleKey = SDLK_F2;

// renderer.hpp's kScreenW/kScreenH — the surface every front-end screen is laid
// out against.
constexpr float kScreenW = 640.0f;
constexpr float kScreenH = 480.0f;

constexpr float kMargin = 6.0f;          // gap from the screen's bottom-right corner
constexpr float kPad = 4.0f;             // inner padding
constexpr float kPanelW = 300.0f;        // open width; ~36 FONT6 columns
constexpr std::size_t kVisibleRows = 6;  // WRAPPED rows shown, not messages
constexpr Uint8 kPanelAlpha = 160;       // "şeffaf": the backdrop reads through
constexpr Uint8 kTabAlpha = 130;         // the collapsed tab sits back further

constexpr char kTabLabel[] = "F2 CHAT";
constexpr char kEntryPrompt[] = "> ";
constexpr char kEntryHint[] = "ENTER SENDS   ESC CLOSES";

// SFX ids the front end already uses: 20 = nav blip, 10 = accept sting, 40 = the
// "you can't do that here" buzz (frontend-flow.md §SFX).
constexpr int kSfxBlip = 20;
constexpr int kSfxAccept = 10;
constexpr int kSfxDenied = 40;

// One laid-out row of the panel. `split` is how many leading characters of
// `text` are the speaker's name (0 for a wrapped continuation row), and
// `split_w` their pixel width, so the name can carry its own ink.
struct ChatRow {
    std::string text;
    std::size_t split = 0;
    float split_w = 0.0f;
    bool mine = false;  // spoken by the local seat
};

// Greedy word wrap on the font's own metrics. A run too long for a whole row is
// broken where it stands rather than dropped.
std::vector<std::string> wrap_text(const FontTextures& font, const std::string& s, float max_w) {
    std::vector<std::string> rows;
    std::string row;
    for (const char c : s) {
        row += c;
        if (static_cast<float>(font.measure(row)) <= max_w) continue;
        const std::size_t space = row.find_last_of(' ');
        if (space != std::string::npos && space > 0) {
            rows.push_back(row.substr(0, space));
            row = row.substr(space + 1);
        } else {
            const std::string carry(1, row.back());
            row.pop_back();
            if (!row.empty()) rows.push_back(row);
            row = carry;
        }
    }
    if (!row.empty()) rows.push_back(row);
    return rows;
}

// Lay the ring out into drawable rows, newest last, keeping only the final
// kVisibleRows so the panel cannot grow off the top of the screen.
std::vector<ChatRow> layout_rows(const FontTextures& font, const std::vector<net::ChatLine>& log,
                                 float max_w, int my_seat) {
    std::vector<ChatRow> rows;
    for (const net::ChatLine& line : log) {
        const std::string prefix = line.name + ": ";
        const std::vector<std::string> wrapped = wrap_text(font, prefix + line.text, max_w);
        for (std::size_t i = 0; i < wrapped.size(); ++i) {
            ChatRow r;
            r.text = wrapped[i];
            r.mine = my_seat >= 0 && line.seat == my_seat;
            // Only the FIRST row of a message carries the name — and only if the
            // wrap did not land inside it (it cannot at kChatMaxNameBytes, but the
            // check costs nothing and keeps the substr honest).
            if (i == 0 && r.text.size() >= prefix.size() &&
                r.text.compare(0, prefix.size(), prefix) == 0) {
                r.split = prefix.size();
                r.split_w = static_cast<float>(font.measure(prefix));
            }
            rows.push_back(std::move(r));
        }
    }
    if (rows.size() > kVisibleRows)
        rows.erase(rows.begin(),
                   rows.begin() + static_cast<std::ptrdiff_t>(rows.size() - kVisibleRows));
    return rows;
}

// The translucent slab + its 1-px frame. Both colours are the real chrome's: the
// window base coat washed over whatever is behind it, framed in the button-label
// grey the list dialogs already use for their own edges.
void draw_slab(SDL_Renderer* ren, const SDL_FRect& r, Uint8 alpha) {
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren, kDialogFill.r, kDialogFill.g, kDialogFill.b, alpha);
    SDL_RenderFillRect(ren, &r);
    SDL_SetRenderDrawColor(ren, kDialogDim.r, kDialogDim.g, kDialogDim.b, alpha);
    SDL_RenderRect(ren, &r);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
}

// Clamp to the wire cap as it is typed: the server REJECTS an over-long line
// rather than trimming it (PROTOCOL.md §7.2), so refusing the keystroke is the
// only way not to lose the message.
void chat_type(std::string& entry, const char* text) {
    for (const char* p = text; p != nullptr && *p != '\0'; ++p) {
        if (entry.size() >= net::kChatMaxBytes) break;
        const auto u = static_cast<unsigned char>(*p);
        if (u >= 32 && u < 127) entry += *p;  // what the FON can draw
    }
}

// Collapsed: the small tab, plus an unread count so a message arriving behind
// the panel is not invisible. An unread tab lights up in the bright ink; an idle
// one stays grey, so the corner does not compete with the screen it sits on.
void draw_tab(ScreenContext ctx, unsigned unread, float line_h) {
    const FontTextures& font = ctx.front_font;
    const std::string label = unread == 0 ? std::string(kTabLabel)
                                          : kTabLabel + std::string(" (") +
                                                std::to_string(std::min(unread, 9u)) +
                                                (unread > 9u ? "+)" : ")");
    const float w = static_cast<float>(font.measure(label)) + 2.0f * kPad;
    const float h = line_h + 2.0f * kPad;
    const SDL_FRect box{kScreenW - kMargin - w, kScreenH - kMargin - h, w, h};
    draw_slab(ctx.sdl, box, kTabAlpha);
    draw_dialog_text(DialogPen{ctx.sdl, font}, label, SDL_FPoint{box.x + kPad, box.y + kPad},
                     DialogInk{unread != 0 ? kDialogInk : kDialogDim});
}

// One laid-out row. Your OWN name goes in the chrome grey, everyone else's in
// the bright ink: you already know who you are, and the eye should go to them.
// The message body is always the bright ink — legibility first.
void draw_row(ScreenContext ctx, const ChatRow& row, SDL_FPoint at) {
    const DialogPen pen{ctx.sdl, ctx.front_font};
    if (row.split == 0) {
        draw_dialog_text(pen, row.text, at, DialogInk{kDialogInk});
        return;
    }
    draw_dialog_text(pen, row.text.substr(0, row.split), at,
                     DialogInk{row.mine ? kDialogDim : kDialogInk});
    draw_dialog_text(pen, row.text.substr(row.split), SDL_FPoint{at.x + row.split_w, at.y},
                     DialogInk{kDialogInk});
}

// The message rows. Returns the y the input line goes on.
float draw_rows(ScreenContext ctx, const std::vector<ChatRow>& rows, const SDL_FRect& box) {
    const float lh = static_cast<float>(ctx.front_font.line_height());
    float y = box.y + kPad;
    for (const ChatRow& row : rows) {
        draw_row(ctx, row, SDL_FPoint{box.x + kPad, y});
        y += lh;
    }
    return y;
}

// The input line. It scrolls with the caret rather than wrapping: a draft is one
// line by construction, and losing sight of what you are typing is worse than
// losing sight of its start. `line` is {x, y, available width, height}.
void draw_entry(ScreenContext ctx, const std::string& entry, const SDL_FRect& line) {
    const FontTextures& font = ctx.front_font;
    const DialogPen pen{ctx.sdl, font};
    const float prompt_w = static_cast<float>(font.measure(kEntryPrompt));
    draw_dialog_text(pen, kEntryPrompt, SDL_FPoint{line.x, line.y}, DialogInk{kDialogInk});
    if (entry.empty()) {
        draw_dialog_text(pen, kEntryHint, SDL_FPoint{line.x + prompt_w, line.y},
                         DialogInk{kDialogDim});
        return;
    }
    std::string shown = entry;
    while (!shown.empty() && static_cast<float>(font.measure(shown + "_")) > line.w - prompt_w)
        shown.erase(shown.begin());
    draw_dialog_text(pen, shown, SDL_FPoint{line.x + prompt_w, line.y}, DialogInk{kDialogInk});
    // A plain blinking underscore caret, on the presentation clock — the sim and
    // its RNG are nowhere near this (ADR-0004).
    if ((SDL_GetTicks() / 500ull) % 2ull == 0ull)
        draw_dialog_text(
            pen, "_",
            SDL_FPoint{line.x + prompt_w + static_cast<float>(font.measure(shown)), line.y},
            DialogInk{kDialogInk});
}

}  // namespace

ChatOverlay::~ChatOverlay() {
    close();
}

void ChatOverlay::close() {
    open_ = false;
    entry_.clear();
    if (text_input_window_ != nullptr) {
        // Stopped on the window input was STARTED on, not on some caller's idea
        // of the current one — a screen may well have gone away by now.
        SDL_StopTextInput(text_input_window_);
        text_input_window_ = nullptr;
    }
}

void ChatOverlay::pump() {
    if (flow_ == nullptr) return;
    flow_->step(static_cast<std::int64_t>(SDL_GetTicks()));
}

bool ChatOverlay::open_panel(const SDL_Event& ev, ScreenContext ctx) {
    if (ev.type != SDL_EVENT_KEY_DOWN || ev.key.repeat || ev.key.key != kToggleKey) return false;
    open_ = true;
    seen_revision_ = flow_->chat_revision();  // opening clears the unread mark
    text_input_window_ = ctx.window;
    SDL_StartTextInput(ctx.window);
    ctx.audio.play(kSfxBlip);
    return true;
}

void ChatOverlay::handle_key(SDL_Keycode key, ScreenContext ctx) {
    if (key == SDLK_ESCAPE || key == kToggleKey) {
        close();
        ctx.audio.play(kSfxBlip);
        return;
    }
    if (key == SDLK_BACKSPACE) {
        if (!entry_.empty()) entry_.pop_back();
        return;
    }
    if (key != SDLK_RETURN && key != SDLK_KP_ENTER) return;
    // send_chat refuses an empty line and refuses a flood; either way the draft
    // is KEPT and the buzz says so, rather than the text vanishing into a silent
    // server-side drop (PROTOCOL.md §7.3).
    if (flow_->send_chat(entry_, static_cast<std::int64_t>(SDL_GetTicks()))) {
        entry_.clear();
        ctx.audio.play(kSfxAccept);
        return;
    }
    ctx.audio.play(kSfxDenied);
}

bool ChatOverlay::handle_event(const SDL_Event& ev, ScreenContext ctx) {
    if (flow_ == nullptr) return false;
    // Closed: the ONLY event this steals is the toggle itself, so a screen
    // behaves exactly as it did before chat existed.
    if (!open_) return open_panel(ev, ctx);
    // Open: EVERY key and text event is ours. This is the "swallow text keys"
    // rule — without it, typing "team" on the roster screen would also toggle
    // teams and blank a slot behind the panel.
    if (ev.type == SDL_EVENT_TEXT_INPUT) {
        chat_type(entry_, ev.text.text);
        return true;
    }
    if (ev.type == SDL_EVENT_TEXT_EDITING || ev.type == SDL_EVENT_KEY_UP) return true;
    if (ev.type != SDL_EVENT_KEY_DOWN) return false;
    handle_key(ev.key.key, ctx);
    return true;
}

void ChatOverlay::draw(ScreenContext ctx) {
    if (flow_ == nullptr) return;
    const float lh = static_cast<float>(ctx.front_font.line_height());
    if (lh <= 0.0f) return;  // no font loaded: nothing to draw with

    if (!open_) {
        draw_tab(ctx, flow_->chat_revision() - seen_revision_, lh);
        return;
    }
    seen_revision_ = flow_->chat_revision();  // it is on screen: nothing is unread

    const float avail = kPanelW - 2.0f * kPad;
    const std::vector<ChatRow> rows =
        layout_rows(ctx.front_font, flow_->chat_log(), avail, flow_->my_seat());
    // Message rows + the one input row. An empty log still shows the input row,
    // so opening the panel always gives you somewhere to type.
    const float h = 2.0f * kPad + static_cast<float>(rows.size() + 1) * lh;
    const SDL_FRect box{kScreenW - kMargin - kPanelW, kScreenH - kMargin - h, kPanelW, h};
    draw_slab(ctx.sdl, box, kPanelAlpha);
    const float entry_y = draw_rows(ctx, rows, box);
    draw_entry(ctx, entry_, SDL_FRect{box.x + kPad, entry_y, avail, lh});
}

}  // namespace bomber::game

#else  // !BOMBER_HAS_LOBBY

// Without the lobby client there is no control plane to chat over, so the whole
// overlay is inert. The DEFINITIONS are switched, never the declarations: the
// header names no net type beyond a forward declaration, so it parses
// identically either way and callers link against the same symbols (the same
// discipline lobby_screen.cpp's run_online follows).

namespace bomber::game {

ChatOverlay::~ChatOverlay() = default;
void ChatOverlay::close() {}
void ChatOverlay::pump() {}
void ChatOverlay::draw(ScreenContext) {}
void ChatOverlay::handle_key(SDL_Keycode, ScreenContext) {}

bool ChatOverlay::open_panel(const SDL_Event&, ScreenContext) {
    return false;
}

bool ChatOverlay::handle_event(const SDL_Event&, ScreenContext) {
    return false;  // never consumes anything: every screen behaves as before
}

}  // namespace bomber::game

#endif  // BOMBER_HAS_LOBBY
