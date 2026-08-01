#include "bomber/netplay/lobby_screen.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "bomber/game_util/list_dialog_geometry.hpp"  // list_dialog_width (centring)
#include "bomber/netui/net_setup_roster.hpp"          // wire_safe_text
#include "bomber/platform/frame_clock.hpp"
#include "bomber/render/sprites.hpp"    // Sprite
#include "bomber/ui/dialog_chrome.hpp"  // the pinned chrome primitives
#include "lobby_chrome.hpp"             // the backdrop / ack modal / SFX ids these screens share

#if defined(BOMBER_HAS_LOBBY)
#include "bomber/net/build_hash.hpp"    // net::build_hash()
#include "bomber/net/lobby_client.hpp"  // net::LobbyClient
#include "bomber/net/lobby_flow.hpp"    // net::LobbyFlow
#include "bomber/net/udp_transport.hpp"  // net::UdpTransport
#endif

namespace bomber::game {

namespace {

// Presentation-only tunables (the original has no online lobby — ADR-0011 is our
// port's own path; docs/re/audit/multiplayer-deep.md §2.5 records that NO
// "waiting for players" screen has been located in the binary).
constexpr float kListY = 110.0f;        // list-dialog window top (the help browser uses 100)
constexpr float kJoinPromptY = 180.0f;  // sub_4028D2's CONFIRMED save-as prompt anchor
constexpr std::size_t kCodeLen = 6;     // lobby codes are 6 Crockford base-32 chars
constexpr float kMinListW = 260.0f;     // keeps a 1-row list from collapsing
constexpr float kHintWrapW = 440.0f;    // hints wider than this wrap instead of widening the window

// PORT-ONLY window placement. sub_42DBCC takes an explicit x (list_dialog_
// geometry.hpp — sub_43C734 really is 6-arg) and both RE'd callers pass the
// literal 100. These lobby screens have no original to copy a position from, so
// they keep the centred look they have always had — stated here rather than
// smuggled into the shared primitive.
float centered_list_x(const FontTextures& font, const std::string& title, float item_w) {
    const int w = list_dialog_width(static_cast<int>(item_w), font.measure(title));
    return (kScreenW - static_cast<float>(w)) / 2.0f;
}

// Crockford base-32: the digits plus the letters MINUS I, L, O and U — dropped
// so a code read out loud cannot be misheard (ADR-0011 lobby codes).
constexpr char kCrockford[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

// Field order is pointer-first to keep the struct padding-free (.clang-tidy's
// clang-analyzer-optin.performance.Padding).
struct MenuRow {
    const char* label;
    LobbyMenuChoice choice;
    bool online;  // needs the matchmaker (hidden on a lobby-off build)
};

// The lobby-size range the matchmaker accepts (PROTOCOL.md §3 CreateLobby:
// "max_seats: int, clamped to [2,10]"). 10 is also sim::kMaxPlayers, so a full
// lobby is a full board with no AI slot left over.
constexpr int kMinLobbySeats = 2;
constexpr int kMaxLobbySeats = 10;

// The two direct rows are the ADR-0010 path that needs no server at all and must
// keep working — hence their own rows rather than a mode hidden behind online.
constexpr MenuRow kRows[] = {
    {"HOST PRIVATE GAME", LobbyMenuChoice::HostOnline, true},
    {"HOST PUBLIC GAME", LobbyMenuChoice::HostPublic, true},
    {"JOIN BY CODE", LobbyMenuChoice::JoinOnline, true},
    {"BROWSE PUBLIC GAMES", LobbyMenuChoice::BrowsePublic, true},
    {"HOST LAN GAME", LobbyMenuChoice::HostDirect, false},
    {"JOIN BY IP ADDRESS", LobbyMenuChoice::JoinDirect, false},
};

// --- the simple list picker ------------------------------------------------
//
// Shared by the NETWORK GAME menu and HOW MANY PLAYERS, which are the same
// screen with different rows: the generic bevel list dialog sub_42DBCC
// (draw_list_dialog) over the MAINMENU backdrop, driven by the widget's own
// model — the whole list is visible, so its handlers can only move the
// highlight and they CLAMP at both ends; Enter picks, Esc backs out. The
// original's net rows play SFX 20 on any key (setup-screens.md "Screen A"),
// which is what the nav blip mirrors.
enum class PickOutcome : std::uint8_t { Ignored, Consumed, Picked, Cancelled, WindowClosed };

// One picker, running: everything the event half and the draw half both read.
struct PickState {
    std::string title;
    std::vector<std::string> labels;
    HintBlock hints;
    float content_w = 0.0f;
    int sel = 0;
    ListDialogWidget pressed = ListDialogWidget::None;

    int count() const { return static_cast<int>(labels.size()); }
    int footer_lines() const { return static_cast<int>(hints.lines.size()); }
};

// The window is sized to the widest of the title, the items and the hint block,
// so no hint can land outside the border.
void pick_measure(const FontTextures& font, PickState& st) {
    float w = static_cast<float>(font.measure(st.title));
    for (const std::string& l : st.labels) w = std::max(w, static_cast<float>(font.measure(l)));
    st.content_w = std::max({w, kMinListW, st.hints.width});
}

ListDialogGeometry pick_geometry(const FontTextures& font, const PickState& st) {
    return list_dialog_layout_for(
        font, ListDialogSpec{st.title, centered_list_x(font, st.title, st.content_w), kListY,
                             st.content_w, st.count(), st.count(), 0, st.footer_lines()});
}

PickOutcome pick_mouse(ScreenContext ctx, PickState& st, const SDL_Event& ev) {
    float mx = 0.0f;
    float my = 0.0f;
    if (!list_mouse_point(ctx.sdl, ev, mx, my)) return PickOutcome::Ignored;
    // The whole menu fits, so the track and the page keys are inert here exactly
    // as they are in the original for a list that fits — but the rows and the two
    // arrow buttons are live.
    const ListDialogGeometry g = pick_geometry(ctx.front_font, st);
    const ListDialogHit hit =
        list_dialog_hit_for(ctx.front_font, g, st.count(), SDL_FPoint{mx, my});
    ListDialogNav nav{0, st.sel};
    if (ev.type == SDL_EVENT_MOUSE_MOTION) {
        if (ev.motion.state == 0) list_dialog_mouse_move(nav, hit, st.count());
        st.sel = nav.highlight;
        return PickOutcome::Consumed;
    }
    if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN && ev.button.button == SDL_BUTTON_LEFT) {
        st.pressed = hit.widget;
        const ListDialogAction act =
            list_dialog_mouse_down(nav, g, hit, st.count(), st.count(), static_cast<int>(my));
        st.sel = nav.highlight;
        if (act != ListDialogAction::Activate) return PickOutcome::Consumed;
        ctx.audio.play(kSfxAccept);
        return PickOutcome::Picked;
    }
    if (ev.type == SDL_EVENT_MOUSE_BUTTON_UP && ev.button.button == SDL_BUTTON_LEFT) {
        const ListDialogWidget was = st.pressed;
        st.pressed = ListDialogWidget::None;
        if (list_dialog_mouse_up(hit, was) != ListDialogAction::Cancel)
            return PickOutcome::Consumed;
        ctx.audio.play(kSfxBlip);
        return PickOutcome::Cancelled;
    }
    return PickOutcome::Consumed;
}

PickOutcome pick_key(ScreenContext ctx, PickState& st, const SDL_Event& ev) {
    if (ev.type != SDL_EVENT_KEY_DOWN || ev.key.repeat) return PickOutcome::Ignored;
    if (ev.key.key == SDLK_RETURN || ev.key.key == SDLK_KP_ENTER) {
        ctx.audio.play(kSfxAccept);
        return PickOutcome::Picked;
    }
    if (ev.key.key == SDLK_ESCAPE) {
        ctx.audio.play(kSfxBlip);
        return PickOutcome::Cancelled;
    }
    if (ev.key.key != SDLK_UP && ev.key.key != SDLK_DOWN) return PickOutcome::Ignored;
    ListDialogNav nav{0, st.sel};
    list_dialog_key(nav, list_dialog_key_code(ev.key.key), st.count(), st.count());
    st.sel = nav.highlight;
    ctx.audio.play(kSfxBlip);
    return PickOutcome::Consumed;
}

void pick_draw(ScreenContext ctx, const PickState& st) {
    const ListDialogLayout lay = draw_list_dialog(
        DialogPen{ctx.sdl, ctx.front_font},
        ListDialogSpec{st.title, centered_list_x(ctx.front_font, st.title, st.content_w), kListY,
                       st.content_w, st.count(), st.count(), 0, st.footer_lines()});
    for (int i = 0; i < st.count(); ++i) {
        const float ty = lay.item_y0 + static_cast<float>(i) * lay.item_h;
        // The selection LIGHTENS the row (sub_442C28), it does not invert it, so
        // the ink is the same either way.
        if (i == st.sel) draw_list_selection(ctx.sdl, lay, i);
        ctx.front_font.draw(ctx.sdl, st.labels[static_cast<std::size_t>(i)],
                            SDL_FPoint{lay.item_x, ty}, TextStyle{kDialogInk});
    }
    for (std::size_t i = 0; i < st.hints.lines.size(); ++i)
        draw_centred(ctx, st.hints.lines[i], lay.footer_y0 + static_cast<float>(i) * lay.item_h);
}

// Run one picker to a decision. `st.sel` carries the default in and the choice out.
PickOutcome run_pick(ScreenContext ctx, PickState& st) {
    pick_measure(ctx.front_font, st);
    platform::FrameClock frame_clock(ctx.window);
    while (true) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return PickOutcome::WindowClosed;
            PickOutcome out = pick_mouse(ctx, st, ev);
            if (out == PickOutcome::Ignored) out = pick_key(ctx, st, ev);
            if (out == PickOutcome::Picked || out == PickOutcome::Cancelled) return out;
        }
        ctx.audio.update_music();
        draw_lobby_backdrop(ctx);
        pick_draw(ctx, st);
        SDL_RenderPresent(ctx.sdl);
        frame_clock.pace();
    }
}

// --- the JOIN BY CODE line edit --------------------------------------------

// The draft and the prompt label above it, which doubles as the rejection
// message: a bad key re-labels in place rather than entering a character the
// server would reject.
struct CodeEntry {
    std::string entry;
    std::string label = "JOIN BY CODE:";
};

enum class EntryOutcome : std::uint8_t { Continue, Accepted, Cancelled, WindowClosed };

void code_type(ScreenContext ctx, CodeEntry& c, const char* text) {
    for (const char* p = text; p != nullptr && *p != '\0'; ++p) {
        if (c.entry.size() >= kCodeLen) break;
        const char up = static_cast<char>(std::toupper(static_cast<unsigned char>(*p)));
        if (std::strchr(kCrockford, up) != nullptr) {
            c.entry += up;
            continue;
        }
        ctx.audio.play(kSfxBlip);
        c.label = "CODE IS 0-9 A-Z (NO I L O U)";
    }
}

EntryOutcome code_event(ScreenContext ctx, CodeEntry& c, const SDL_Event& ev) {
    if (ev.type == SDL_EVENT_QUIT) return EntryOutcome::WindowClosed;
    if (ev.type == SDL_EVENT_TEXT_INPUT) {
        code_type(ctx, c, ev.text.text);
        return EntryOutcome::Continue;
    }
    if (ev.type != SDL_EVENT_KEY_DOWN) return EntryOutcome::Continue;
    if (ev.key.key == SDLK_BACKSPACE) {
        if (!c.entry.empty()) c.entry.pop_back();
        return EntryOutcome::Continue;
    }
    if (ev.key.key == SDLK_ESCAPE) {
        ctx.audio.play(kSfxBlip);
        return EntryOutcome::Cancelled;
    }
    if (ev.key.key != SDLK_RETURN && ev.key.key != SDLK_KP_ENTER) return EntryOutcome::Continue;
    if (c.entry.size() == kCodeLen) {
        ctx.audio.play(kSfxAccept);
        return EntryOutcome::Accepted;
    }
    ctx.audio.play(kSfxBlip);
    c.label = "ENTER ALL 6 CHARACTERS";
    return EntryOutcome::Continue;
}

}  // namespace

LobbyMenuChoice LobbyScreen::run_menu(bool online_available) {
    std::vector<const MenuRow*> rows;
    for (const MenuRow& r : kRows)
        if (online_available || !r.online) rows.push_back(&r);
    if (rows.empty()) return LobbyMenuChoice::Cancel;

    PickState st;
    st.title = "NETWORK GAME";
    for (const MenuRow* r : rows) st.labels.emplace_back(r->label);

    const PickOutcome out = run_pick(ctx_, st);
    if (out == PickOutcome::WindowClosed) return LobbyMenuChoice::WindowClosed;
    if (out != PickOutcome::Picked) return LobbyMenuChoice::Cancel;
    return rows[static_cast<std::size_t>(st.sel)]->choice;
}

bool LobbyScreen::run_seat_count(int& seats, bool& window_closed) {
    PickState st;
    st.title = "HOW MANY PLAYERS";
    for (int n = kMinLobbySeats; n <= kMaxLobbySeats; ++n)
        st.labels.push_back(std::to_string(n) + " PLAYERS");
    // The footer says what the number actually MEANS, because "players" is only
    // half true: these are network seats (one machine each) and the roster screen
    // adds AI slots on top of them.
    st.hints = pack_hint_lines(ctx_.front_font, {"MACHINES IN THE LOBBY", "(ADD AI SLOTS LATER)"},
                               {}, kHintWrapW);
    st.sel = std::clamp(seats, kMinLobbySeats, kMaxLobbySeats) - kMinLobbySeats;

    const PickOutcome out = run_pick(ctx_, st);
    window_closed = out == PickOutcome::WindowClosed;
    if (out != PickOutcome::Picked) return false;
    seats = st.sel + kMinLobbySeats;
    return true;
}

bool LobbyScreen::run_code_entry(std::string& code, bool& window_closed) {
    // The sub_42E938 text-entry family at the CONFIRMED y=180 anchor — the same
    // call NetplayConnectScreen::run_join makes for its host:port line.
    CodeEntry c;
    platform::FrameClock frame_clock(ctx_.window);
    SDL_StartTextInput(ctx_.window);

    while (true) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            const EntryOutcome out = code_event(ctx_, c, ev);
            if (out == EntryOutcome::Continue) continue;
            SDL_StopTextInput(ctx_.window);
            window_closed = out == EntryOutcome::WindowClosed;
            if (out != EntryOutcome::Accepted) return false;
            code = c.entry;
            return true;
        }

        ctx_.audio.update_music();
        draw_lobby_backdrop(ctx_);
        draw_text_entry_dialog(DialogPen{ctx_.sdl, ctx_.front_font}, kJoinPromptY,
                               TextEntryLabels{c.label, c.entry, "Join", "Cancel"});
        SDL_RenderPresent(ctx_.sdl);
        frame_clock.pace();
    }
}

#if defined(BOMBER_HAS_LOBBY)

namespace {

constexpr float kNameCol = 26.0f;       // seat-number column width inside a roster row
constexpr std::size_t kNameChars = 20;  // clamp for an untrusted wire-supplied name

// A name that arrived over the wire is another player's typing — untrusted
// input, run through netui's ONE printable-ASCII filter (wire_safe_text). The
// clamp here is the ROW-WIDTH cap, not the wire cap: kNameChars is what keeps
// one long name from widening the roster dialog off the 640-px screen. The
// fallback stands in for a name the filter emptied.
std::string safe_wire_name(const std::string& raw, const char* fallback) {
    std::string out = wire_safe_text(raw, kNameChars);
    return out.empty() ? std::string(fallback) : out;
}

// --- the waiting room ------------------------------------------------------

constexpr char kOpenSeatMark[] = "OPEN";

// The ready hint's two wordings differ in length, so both are budgeted at the
// longer one and pressing SPACE never resizes the dialog under the player.
HintBlock room_hints(const FontTextures& font, const net::LobbyFlow& flow, bool local_ready) {
    std::vector<std::string> parts{local_ready ? "SPACE = NOT READY" : "SPACE = READY"};
    std::vector<std::string> budget{"SPACE = NOT READY"};
    if (flow.is_host()) {
        parts.emplace_back("ENTER = START");
        budget.emplace_back("ENTER = START");
    }
    parts.emplace_back("ESC = LEAVE");
    budget.emplace_back("ESC = LEAVE");
    if (!flow.is_host()) {
        parts.emplace_back("(HOST STARTS THE MATCH)");
        budget.emplace_back("(HOST STARTS THE MATCH)");
    }
    return pack_hint_lines(font, parts, budget, kHintWrapW);
}

// seat column + name + the widest marker pair the row can show
float room_row_w(const FontTextures& font, const net::RosterEntry& e) {
    return kNameCol + static_cast<float>(font.measure(safe_wire_name(e.name, "PLAYER"))) +
           static_cast<float>(font.measure("  HOST  WAITING"));
}

void draw_room_seats(ScreenContext ctx, const net::LobbyFlow& flow, const ListDialogLayout& lay) {
    const std::vector<net::RosterEntry>& roster = flow.roster();
    for (std::size_t i = 0; i < roster.size(); ++i) {
        const net::RosterEntry& e = roster[i];
        const float ty = lay.item_y0 + static_cast<float>(i) * lay.item_h;
        // Own-seat highlight: the lightening band, same ink (sub_442C28).
        if (e.seat == flow.my_seat()) draw_list_selection(ctx.sdl, lay, static_cast<int>(i));
        ctx.front_font.draw(ctx.sdl, std::to_string(e.seat + 1), SDL_FPoint{lay.item_x, ty},
                            TextStyle{kDialogInk});
        ctx.front_font.draw(ctx.sdl, safe_wire_name(e.name, "PLAYER"),
                            SDL_FPoint{lay.item_x + kNameCol, ty}, TextStyle{kDialogInk});
        // Right-aligned status column. Words rather than tick/cross glyphs: the
        // original FON fonts have no check/cross codepoint, and drawing one would
        // mean inventing art.
        const std::string mark =
            std::string(e.is_host ? "HOST  " : "") + (e.ready ? "READY" : "WAITING");
        const float mw = static_cast<float>(ctx.front_font.measure(mark));
        ctx.front_font.draw(ctx.sdl, mark, SDL_FPoint{lay.item_x + lay.item_w - mw, ty},
                            TextStyle{kDialogInk});
    }
}

// The seats the host reserved that nobody has taken yet. Drawn in the chrome's
// own grey (kDialogDim*, the ink the browser dims an unjoinable row with) so an
// empty seat reads as absence, not as a player.
void draw_open_seats(ScreenContext ctx, const ListDialogLayout& lay, std::size_t taken, int rows) {
    for (std::size_t i = taken; i < static_cast<std::size_t>(rows); ++i) {
        const float ty = lay.item_y0 + static_cast<float>(i) * lay.item_h;
        ctx.front_font.draw(ctx.sdl, std::to_string(i + 1), SDL_FPoint{lay.item_x, ty},
                            TextStyle{kDialogDim});
        const float mw = static_cast<float>(ctx.front_font.measure(kOpenSeatMark));
        ctx.front_font.draw(ctx.sdl, kOpenSeatMark, SDL_FPoint{lay.item_x + lay.item_w - mw, ty},
                            TextStyle{kDialogDim});
    }
}

// The lobby CODE goes in the list dialog's pinned centred title strip — it is
// the one string the host reads out to friends, so it goes where the chrome
// already puts a prominent centred label. `max_seats` (0 = a guest, which
// JoinAccepted gives no lobby size) draws an OPEN row for every seat past the
// roster, so the host can watch its chosen lobby fill up instead of guessing.
void draw_room(ScreenContext ctx, const net::LobbyFlow& flow, bool local_ready, int max_seats) {
    const std::vector<net::RosterEntry>& roster = flow.roster();
    const int rows = std::max({static_cast<int>(roster.size()), max_seats, 1});
    const std::string title =
        "LOBBY CODE   " + (flow.code().empty() ? std::string("------") : flow.code());
    // The hints are part of the window's CONTENT, not something floating under
    // it: they are packed by measured width and the window is sized to the widest
    // resulting line, so no hint can land outside the border.
    const HintBlock hints = room_hints(ctx.front_font, flow, local_ready);

    float content_w = static_cast<float>(ctx.front_font.measure(title));
    for (const net::RosterEntry& e : roster)
        content_w = std::max(content_w, room_row_w(ctx.front_font, e));
    content_w = std::max({content_w, kMinListW, hints.width});

    const ListDialogLayout lay = draw_list_dialog(
        DialogPen{ctx.sdl, ctx.front_font},
        ListDialogSpec{title, centered_list_x(ctx.front_font, title, content_w), kListY, content_w,
                       rows, rows, 0, static_cast<int>(hints.lines.size())});
    draw_room_seats(ctx, flow, lay);
    draw_open_seats(ctx, lay, roster.size(), rows);
    for (std::size_t i = 0; i < hints.lines.size(); ++i)
        draw_centred(ctx, hints.lines[i], lay.footer_y0 + static_cast<float>(i) * lay.item_h);
}

// One waiting room, running.
struct RoomState {
    net::LobbyFlow* flow = nullptr;
    ChatOverlay* chat = nullptr;
    AckChrome ack;
    std::string failure;  // non-empty once Phase::Failed latched -> the ack modal
    bool local_ready = false;
};

enum class RoomOutcome : std::uint8_t { Continue, Leave, WindowClosed };

RoomOutcome room_event(ScreenContext ctx, RoomState& st, const SDL_Event& ev) {
    if (ev.type == SDL_EVENT_QUIT) return RoomOutcome::WindowClosed;
    // The chat overlay gets first refusal (chat_overlay.hpp): while it is open it
    // eats every key, so typing never also drives the room.
    if (st.chat->handle_event(ev, ctx)) return RoomOutcome::Continue;
    if (ev.type != SDL_EVENT_KEY_DOWN || ev.key.repeat) return RoomOutcome::Continue;
    if (!st.failure.empty())
        return ack_dismiss_key(ctx, ev.key.key) ? RoomOutcome::Leave : RoomOutcome::Continue;
    if (ev.key.key == SDLK_ESCAPE) {
        ctx.audio.play(kSfxBlip);
        return RoomOutcome::Leave;  // the flow's dtor closes the socket
    }
    // The room's controls are live only while actually IN the room — during
    // Connecting/Rendezvous there is nothing to toggle.
    if (st.flow->phase() != net::LobbyFlow::Phase::InLobby) return RoomOutcome::Continue;
    if (ev.key.key == SDLK_SPACE || ev.key.key == SDLK_R) {
        st.local_ready = !st.local_ready;
        st.flow->set_ready(st.local_ready);
        ctx.audio.play(kSfxAccept);
        return RoomOutcome::Continue;
    }
    if (ev.key.key != SDLK_RETURN && ev.key.key != SDLK_KP_ENTER) return RoomOutcome::Continue;
    if (!st.flow->is_host()) {
        ctx.audio.play(kSfxDenied);
        return RoomOutcome::Continue;
    }
    st.flow->start_match();  // the server validates all-ready + build parity
    ctx.audio.play(kSfxAccept);
    return RoomOutcome::Continue;
}

void room_draw(ScreenContext ctx, const RoomState& st, const LobbyRoomView& view) {
    if (!st.failure.empty()) {
        // LobbyFlow::error() strings are already player-facing ("LOBBY IS FULL",
        // "VERSION MISMATCH - UPDATE THE GAME", ...).
        draw_ack(ctx, st.ack, "NETWORK ERROR", st.failure);
        return;
    }
    const net::LobbyFlow::Phase phase = st.flow->phase();
    if (phase == net::LobbyFlow::Phase::InLobby) {
        draw_room(ctx, *st.flow, st.local_ready, view.max_seats);
        return;
    }
    // One prompt for the whole connect step. Which path won — punched, or
    // forwarded after the punch failed — is not the player's problem, and
    // Verifying is where a one-sided punch is caught and rerouted rather than
    // becoming a match that carries nothing.
    if (phase == net::LobbyFlow::Phase::Rendezvous || phase == net::LobbyFlow::Phase::Verifying ||
        phase == net::LobbyFlow::Phase::Relaying) {
        draw_ack(ctx, st.ack, "STARTING MATCH", "CONNECTING TO PLAYERS...");
        return;
    }
    draw_ack(ctx, st.ack, view.host ? "HOSTING A GAME" : "JOINING " + view.code,
             "CONTACTING THE SERVER...");
}

// --- the PUBLIC GAMES browser (Phase 3) ------------------------------------

constexpr int kBrowseRows = 10;  // the *.BM help browser's own visible-row count
// How often the browser re-asks the matchmaker while it is open. Short enough
// that a lobby someone hosts while you are looking turns up on its own, long
// enough that a screen left open is background noise. R / F5 asks immediately.
constexpr std::uint64_t kBrowseRefreshMs = 4000;
constexpr float kColGap = 12.0f;  // gap between two row columns
// The incompatible-build marker. A WORD, for the same reason draw_room's
// HOST/READY/WAITING are words: the original FON fonts carry no tick/cross
// codepoint, so a symbol would have to be drawn art. Matches the wording
// LobbyFlow::error() already uses ("VERSION MISMATCH ...").
constexpr char kStaleMark[] = "VERSION";
constexpr char kBrowseTitle[] = "PUBLIC GAMES";

std::string occupancy(const net::PublicLobby& l) {
    return std::to_string(l.players) + "/" + std::to_string(l.max);
}

std::string display_name(const std::string& raw) {
    return safe_wire_name(raw, "GAME");
}

HintBlock browse_hints(ScreenContext ctx) {
    return pack_hint_lines(ctx.front_font, {"ENTER = JOIN", "R = REFRESH", "ESC = BACK"}, {},
                           kHintWrapW);
}

// Column widths come from the WHOLE list, not just the visible window, so the
// columns do not jump around as the list scrolls. Measured in one place so the
// mouse pump can rebuild EXACTLY the geometry the last frame drew — a hit test
// against a re-derived width would drift off the visible rows.
struct BrowseColumns {
    float name = 0.0f;
    float occ = 0.0f;
    float code = 0.0f;
    float mark = 0.0f;
    float content = 0.0f;  // the item column the window is sized to
};

BrowseColumns browse_columns(ScreenContext ctx, const std::vector<net::PublicLobby>& list) {
    const FontTextures& font = ctx.front_font;
    BrowseColumns c;
    c.mark = static_cast<float>(font.measure(kStaleMark));
    for (const net::PublicLobby& l : list) {
        c.name = std::max(c.name, static_cast<float>(font.measure(display_name(l.name))));
        c.occ = std::max(c.occ, static_cast<float>(font.measure(occupancy(l))));
        c.code = std::max(c.code, static_cast<float>(font.measure(l.code)));
    }
    c.content = std::max({c.name + c.occ + c.code + c.mark + 3.0f * kColGap,
                          static_cast<float>(font.measure(kBrowseTitle)), kMinListW,
                          browse_hints(ctx).width});
    return c;
}

void draw_browser(ScreenContext ctx, const std::vector<net::PublicLobby>& list, int sel, int top) {
    const int count = static_cast<int>(list.size());
    const int visible = std::min(count, kBrowseRows);
    const HintBlock hints = browse_hints(ctx);
    const BrowseColumns col = browse_columns(ctx, list);

    const ListDialogLayout lay = draw_list_dialog(
        DialogPen{ctx.sdl, ctx.front_font},
        ListDialogSpec{kBrowseTitle, centered_list_x(ctx.front_font, kBrowseTitle, col.content),
                       kListY, col.content, visible, count, top,
                       static_cast<int>(hints.lines.size())});
    // Columns are laid out from the RIGHT edge of the item area — marker, code,
    // occupancy — so the name takes what is left and a scrollbar (which narrows
    // that area) simply shifts them.
    const float mark_x = lay.item_x + lay.item_w - col.mark;
    const float code_x = mark_x - kColGap - col.code;
    const float occ_x = code_x - kColGap - col.occ;

    for (int i = top; i < top + visible; ++i) {
        const net::PublicLobby& l = list[static_cast<std::size_t>(i)];
        const float ty = lay.item_y0 + static_cast<float>(i - top) * lay.item_h;
        // A row whose build_ok is false cannot be joined, so it is drawn in the
        // chrome's own grey and carries the VERSION marker. The selection band
        // LIGHTENS the row instead of inverting it, so those rows keep their dim
        // ink while selected — which the earlier inverted band could not express
        // without inventing a third colour.
        const TextStyle row_style{l.build_ok ? kDialogInk : kDialogDim};
        if (i == sel) draw_list_selection(ctx.sdl, lay, i - top);
        ctx.front_font.draw(ctx.sdl, display_name(l.name), SDL_FPoint{lay.item_x, ty}, row_style);
        const std::string occ = occupancy(l);
        const float ow = static_cast<float>(ctx.front_font.measure(occ));
        ctx.front_font.draw(ctx.sdl, occ, SDL_FPoint{occ_x + col.occ - ow, ty}, row_style);
        ctx.front_font.draw(ctx.sdl, l.code, SDL_FPoint{code_x, ty}, row_style);
        if (!l.build_ok)
            ctx.front_font.draw(ctx.sdl, kStaleMark, SDL_FPoint{mark_x, ty}, row_style);
    }

    for (std::size_t i = 0; i < hints.lines.size(); ++i)
        draw_centred(ctx, hints.lines[i], lay.footer_y0 + static_cast<float>(i) * lay.item_h);
}

// One browser, running.
struct BrowseState {
    net::LobbyFlow* flow = nullptr;
    AckChrome ack;
    ListDialogNav nav;  // sub_42DBCC's own two registers: first visible row + highlight offset
    // The SELECTION's identity is the row's CODE, not its index: the list is
    // re-queried underneath the player every few seconds and a lobby appearing or
    // filling up would otherwise slide the highlight onto a different game.
    std::string sel_code;
    std::string picked;
    std::string failure;  // non-empty once Phase::Failed latched -> the ack modal
    std::uint64_t last_query_ms = 0;
    unsigned first_rev = 0;
    unsigned seen_rev = 0;
    int sel = 0;
    ListDialogWidget pressed = ListDialogWidget::None;

    const std::vector<net::PublicLobby>& list() const { return flow->public_lobbies(); }
    int count() const { return static_cast<int>(list().size()); }
    // Only the FIRST answer gets the blocking modal. Once a list exists it stays
    // on screen through every later query, so a refresh never blanks what the
    // player is reading.
    bool searching() const { return flow->public_list_revision() == first_rev; }
};

PickOutcome browse_activate(ScreenContext ctx, BrowseState& st) {
    const net::PublicLobby& row = st.list()[static_cast<std::size_t>(st.sel)];
    if (!row.build_ok) {
        // SFX 40 — the same "you can't do that here" buzz the waiting room fires
        // at a non-host pressing Enter. The server would refuse this join with
        // VERSION MISMATCH, so the row is a dead end by design, not a failure to
        // show.
        ctx.audio.play(kSfxDenied);
        return PickOutcome::Consumed;
    }
    ctx.audio.play(kSfxAccept);
    st.picked = row.code;
    return PickOutcome::Picked;
}

// Ask NOW rather than waiting out the interval. Ignored while an answer is still
// outstanding — the flow would refuse it anyway (browse_public is Idle/Failed
// only). It no longer swaps the list for a modal: the timer would put one up
// every few seconds otherwise, and a list that keeps vanishing is unusable.
PickOutcome browse_refresh(ScreenContext ctx, BrowseState& st) {
    if (st.flow->phase() != net::LobbyFlow::Phase::Idle) return PickOutcome::Consumed;
    st.flow->browse_public();
    st.last_query_ms = SDL_GetTicks();
    ctx.audio.play(kSfxBlip);
    return PickOutcome::Consumed;
}

PickOutcome browse_mouse(ScreenContext ctx, BrowseState& st, const SDL_Event& ev) {
    float mx = 0.0f;
    float my = 0.0f;
    if (!list_mouse_point(ctx.sdl, ev, mx, my)) return PickOutcome::Ignored;
    // The list's own widgets are live only once a real list is up: both modals
    // own the screen while they are showing.
    if (!st.failure.empty() || st.searching() || st.count() == 0) return PickOutcome::Consumed;
    const int visible = std::min(st.count(), kBrowseRows);
    const float content_w = browse_columns(ctx, st.list()).content;
    const ListDialogGeometry g = list_dialog_layout_for(
        ctx.front_font,
        ListDialogSpec{kBrowseTitle, centered_list_x(ctx.front_font, kBrowseTitle, content_w),
                       kListY, content_w, visible, st.count(), st.nav.top_row,
                       static_cast<int>(browse_hints(ctx).lines.size())});
    const ListDialogHit hit = list_dialog_hit_for(ctx.front_font, g, visible, SDL_FPoint{mx, my});
    if (ev.type == SDL_EVENT_MOUSE_MOTION) {
        if (ev.motion.state == 0) list_dialog_mouse_move(st.nav, hit, st.count());
        return PickOutcome::Consumed;
    }
    if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN && ev.button.button == SDL_BUTTON_LEFT) {
        st.pressed = hit.widget;
        if (list_dialog_mouse_down(st.nav, g, hit, visible, st.count(), static_cast<int>(my)) !=
            ListDialogAction::Activate)
            return PickOutcome::Consumed;
        st.sel = st.nav.top_row + st.nav.highlight;
        return browse_activate(ctx, st);
    }
    if (ev.type == SDL_EVENT_MOUSE_BUTTON_UP && ev.button.button == SDL_BUTTON_LEFT) {
        const ListDialogWidget was = st.pressed;
        st.pressed = ListDialogWidget::None;
        if (list_dialog_mouse_up(hit, was) != ListDialogAction::Cancel)
            return PickOutcome::Consumed;
        ctx.audio.play(kSfxBlip);
        return PickOutcome::Cancelled;
    }
    return PickOutcome::Consumed;
}

PickOutcome browse_key(ScreenContext ctx, BrowseState& st, const SDL_Event& ev) {
    if (ev.type != SDL_EVENT_KEY_DOWN || ev.key.repeat) return PickOutcome::Ignored;
    const SDL_Keycode key = ev.key.key;
    if (!st.failure.empty())
        return ack_dismiss_key(ctx, key) ? PickOutcome::Cancelled : PickOutcome::Consumed;
    if (key == SDLK_ESCAPE) {
        ctx.audio.play(kSfxBlip);
        return PickOutcome::Cancelled;  // back to the NETWORK GAME menu, nothing joined
    }
    if (key == SDLK_R || key == SDLK_F5) return browse_refresh(ctx, st);
    if (st.searching()) return PickOutcome::Consumed;  // the transient modal has nothing to steer
    if (st.count() == 0) {
        // The answered-but-empty state draws the acknowledge modal, whose dismiss
        // keys close it (the *.BM browser's empty-glob path does the same); every
        // other key blips.
        if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE)
            return PickOutcome::Cancelled;
        ctx.audio.play(kSfxBlip);
        return PickOutcome::Consumed;
    }
    // sub_42DBCC's own navigation model (list_dialog_geometry.hpp): arrows move
    // the highlight and STOP at the ends, Home/End/PageUp/PageDown scroll the
    // VIEW only. No letter-jump here — R is the refresh key.
    const int code_key = (key == SDLK_SPACE) ? kListKeyEnter : list_dialog_key_code(key);
    if (code_key == kListKeyEscape) return PickOutcome::Consumed;  // handled above
    if (code_key == kListKeyEnter) {
        st.sel = st.nav.top_row + st.nav.highlight;  // @0x42E39A
        return browse_activate(ctx, st);
    }
    if (code_key == 0) return PickOutcome::Consumed;
    list_dialog_key(st.nav, code_key, kBrowseRows, st.count());
    ctx.audio.play(kSfxBlip);
    return PickOutcome::Consumed;
}

// A fresh answer replaces the whole vector, so the CODE — not the index — is
// what carries the highlight across it. A row that vanished leaves the selection
// where it was (browse_clamp puts it back in range), which is the least
// surprising thing that can happen to a cursor whose target is gone.
void browse_reanchor(BrowseState& st) {
    st.seen_rev = st.flow->public_list_revision();
    if (st.sel_code.empty()) return;
    const std::vector<net::PublicLobby>& list = st.list();
    const auto row = std::find_if(list.begin(), list.end(), [&st](const net::PublicLobby& l) {
        return l.code == st.sel_code;
    });
    if (row == list.end()) return;
    // Re-anchor BOTH registers on the row that moved: keep the view where it is
    // if the row is still inside it, otherwise scroll to bring it back.
    const int at = static_cast<int>(row - list.begin());
    if (at < st.nav.top_row || at >= st.nav.top_row + kBrowseRows)
        st.nav.top_row = std::min(at, std::max(0, static_cast<int>(list.size()) - kBrowseRows));
    st.nav.highlight = at - st.nav.top_row;
}

// A refresh can shrink the list under the cursor; restore the widget's own
// invariants before drawing. PORT-ONLY: sub_42DBCC's list never changes under
// it, so there is nothing here to be faithful to.
void browse_clamp(BrowseState& st) {
    const int count = st.count();
    st.nav.top_row = std::clamp(st.nav.top_row, 0, std::max(0, count - kBrowseRows));
    st.nav.highlight =
        std::clamp(st.nav.highlight, 0, std::max(0, std::min(count, kBrowseRows) - 1));
    if (st.nav.top_row + st.nav.highlight >= count)
        st.nav.highlight = std::max(0, count - 1 - st.nav.top_row);
    st.sel = st.nav.top_row + st.nav.highlight;
    st.sel_code = count > 0 ? st.list()[static_cast<std::size_t>(st.sel)].code : std::string();
}

void browse_poll(ScreenContext ctx, BrowseState& st) {
    const std::uint64_t now_ms = SDL_GetTicks();
    st.flow->step(static_cast<std::int64_t>(now_ms));
    if (st.flow->phase() == net::LobbyFlow::Phase::Failed && st.failure.empty()) {
        st.failure = st.flow->error().empty() ? std::string("CONNECTION FAILED") : st.flow->error();
        ctx.audio.play(kSfxBlip);
    }
    // THE BACKGROUND RE-QUERY. Opening this screen before anyone has hosted used
    // to leave "NO PUBLIC GAMES" on screen until the player thought to press R —
    // the list was a snapshot of one instant. Idle is the gate: it means the
    // previous answer has landed, so a slow server throttles this by itself
    // instead of stacking requests.
    if (st.failure.empty() && st.flow->phase() == net::LobbyFlow::Phase::Idle &&
        now_ms - st.last_query_ms >= kBrowseRefreshMs) {
        st.flow->browse_public();
        st.last_query_ms = now_ms;
    }
    if (st.flow->public_list_revision() != st.seen_rev) browse_reanchor(st);
    browse_clamp(st);
}

void browse_draw(ScreenContext ctx, const BrowseState& st) {
    if (!st.failure.empty()) {
        draw_ack(ctx, st.ack, "NETWORK ERROR", st.failure);
        return;
    }
    if (st.searching()) {
        draw_ack(ctx, st.ack, kBrowseTitle, "SEARCHING FOR GAMES...");
        return;
    }
    if (st.count() > 0) {
        draw_browser(ctx, st.list(), st.sel, st.nav.top_row);
        return;
    }
    const std::string body = "NO PUBLIC GAMES";
    draw_ack(ctx, st.ack, kBrowseTitle, body);
    // sub_414340's width is PINNED to its own two lines, so this hint cannot
    // widen the window the way the list dialogs' can — it wraps to the pinned
    // width instead, which is what keeps it from stretching past both borders.
    const DialogRect win = acknowledge_dialog_rect(ctx.front_font, kBrowseTitle, body);
    const HintBlock hints =
        pack_hint_lines(ctx.front_font, {"R = REFRESH", "ESC = BACK"}, {}, win.w);
    const float lh = static_cast<float>(ctx.front_font.line_height());
    for (std::size_t i = 0; i < hints.lines.size(); ++i)
        draw_centred(ctx, hints.lines[i], win.y + win.h + 10.0f + static_cast<float>(i) * lh);
}

// Phase::Ready: the transport is punched and connected, so the server's
// authoritative parameters go straight to the match core.
LobbyRoomResult ready_result(const net::LobbyFlow& flow) {
    LobbyRoomResult r;
    r.ready = true;
    r.is_host = flow.is_host();
    r.seed = flow.match_start().seed;
    r.local_seats_mask = flow.match_start().local_seats_mask;
    r.all_seats_mask = flow.match_start().all_seats_mask;
    return r;
}

// browse_public() rides the control connection up and drops back to Idle when
// the answer lands, so the ONLY reliable "the answer arrived" signal is the
// revision counter — phase() is Idle before and after (lobby_flow.hpp). That
// Idle is also the "no request outstanding" test the re-query needs.
BrowseState browse_begin(ScreenContext ctx, net::LobbyFlow& flow) {
    BrowseState st;
    st.flow = &flow;
    st.ack = ack_chrome(ctx);
    st.first_rev = flow.public_list_revision();
    st.seen_rev = st.first_rev;
    st.last_query_ms = SDL_GetTicks();
    flow.browse_public();
    return st;
}

net::LobbyFlow::Config flow_config(const LobbyScreen::OnlineConfig& ocfg) {
    net::LobbyFlow::Config cfg;
    cfg.server_url = ocfg.server_url;
    cfg.stun_host = ocfg.stun_host;
    cfg.stun_port = ocfg.stun_port;
    cfg.player_name = ocfg.player_name;
    cfg.build_hash = net::build_hash();  // the server flags every row against ours
    return cfg;
}

}  // namespace

LobbyRoomResult LobbyScreen::run_online(net::LobbyFlow& flow, ChatOverlay& chat,
                                        const LobbyRoomView& view) {
    LobbyRoomResult result;
    platform::FrameClock frame_clock(ctx_.window);
    RoomState st;
    st.flow = &flow;
    st.chat = &chat;
    st.ack = ack_chrome(ctx_);

    while (true) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            const RoomOutcome out = room_event(ctx_, st, ev);
            if (out == RoomOutcome::WindowClosed) {
                result.window_closed = true;
                return result;
            }
            if (out == RoomOutcome::Leave) return result;  // back to the NETWORK GAME menu
        }

        flow.step(static_cast<std::int64_t>(SDL_GetTicks()));
        if (flow.phase() == net::LobbyFlow::Phase::Ready) return ready_result(flow);
        if (flow.phase() == net::LobbyFlow::Phase::Failed && st.failure.empty()) {
            st.failure = flow.error().empty() ? std::string("CONNECTION FAILED") : flow.error();
            ctx_.audio.play(kSfxBlip);
        }

        ctx_.audio.update_music();
        draw_lobby_backdrop(ctx_);
        room_draw(ctx_, st, view);
        // Last, so the panel sits over whatever the room drew. No pump() here:
        // this screen already stepped the flow above, and the overlay shares it.
        chat.draw(ctx_);
        SDL_RenderPresent(ctx_.sdl);
        frame_clock.pace();
    }
}

bool LobbyScreen::run_public_browser(const OnlineConfig& ocfg, net::UdpTransport& transport,
                                     std::string& code, bool& window_closed) {
    const net::LobbyFlow::Config cfg = flow_config(ocfg);
    net::LobbyClient client;
    net::LobbyFlow flow(cfg, transport, client);

    BrowseState st = browse_begin(ctx_, flow);
    platform::FrameClock frame_clock(ctx_.window);
    while (true) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) {
                window_closed = true;
                return false;
            }
            PickOutcome out = browse_mouse(ctx_, st, ev);
            if (out == PickOutcome::Ignored) out = browse_key(ctx_, st, ev);
            if (out == PickOutcome::Cancelled) return false;
            if (out == PickOutcome::Picked) {
                code = st.picked;
                return true;  // -> the caller's UNCHANGED join path
            }
        }

        browse_poll(ctx_, st);
        ctx_.audio.update_music();
        draw_lobby_backdrop(ctx_);
        browse_draw(ctx_, st);
        SDL_RenderPresent(ctx_.sdl);
        frame_clock.pace();
    }
}

#endif  // BOMBER_HAS_LOBBY

}  // namespace bomber::game
