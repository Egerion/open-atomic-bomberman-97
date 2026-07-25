#include "bomber/game/screens/lobby_screen.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "bomber/game/dialog_chrome.hpp"  // the pinned chrome primitives
#include "bomber/game/sprites.hpp"        // Sprite
#include "bomber/platform/frame_clock.hpp"

#if defined(BOMBER_HAS_LOBBY)
#include "bomber/net/build_hash.hpp"    // net::build_hash()
#include "bomber/net/lobby_client.hpp"  // net::LobbyClient
#include "bomber/net/lobby_flow.hpp"    // net::LobbyFlow
#include "bomber/net/udp_transport.hpp"  // net::UdpTransport
#endif

namespace bomber::game {

namespace {

// Presentation-only tunables (the original has no online lobby — ADR-0011 is
// our port's own path; see docs/re/audit/multiplayer-deep.md §2.5, which records
// that NO "waiting for players" screen has been located in the binary).
constexpr float kListY = 110.0f;        // list-dialog window top (the help browser uses 100)
constexpr float kJoinPromptY = 180.0f;  // sub_4028D2's CONFIRMED save-as prompt anchor
constexpr std::size_t kCodeLen = 6;     // lobby codes are 6 Crockford base-32 chars
constexpr float kMinListW = 260.0f;     // keeps a 1-row list from collapsing
// Key hints wider than this wrap to another line (pack_hint_lines) instead of
// widening the window further — the widened window then still sits well inside
// the 640-px screen. Presentation-only, like everything else in this file.
constexpr float kHintWrapW = 440.0f;

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

// The NETWORK GAME rows. The two direct rows are the ADR-0010 path that needs no
// server at all and must keep working — hence their own rows rather than a mode
// hidden behind the online ones.
constexpr MenuRow kRows[] = {
    {"HOST PRIVATE GAME", LobbyMenuChoice::HostOnline, true},
    {"HOST PUBLIC GAME", LobbyMenuChoice::HostPublic, true},
    {"JOIN BY CODE", LobbyMenuChoice::JoinOnline, true},
    {"BROWSE PUBLIC GAMES", LobbyMenuChoice::BrowsePublic, true},
    {"HOST LAN GAME", LobbyMenuChoice::HostDirect, false},
    {"JOIN BY IP ADDRESS", LobbyMenuChoice::JoinDirect, false},
};

}  // namespace

void LobbyScreen::draw_backdrop() {
    SDL_SetRenderDrawColor(ctx_.sdl, 0, 0, 0, 255);
    SDL_RenderClear(ctx_.sdl);
    const Sprite& bg = ctx_.assets.frontend_pcx("MAINMENU");
    if (bg.tex) {
        SDL_FRect d{0, 0, static_cast<float>(bg.w), static_cast<float>(bg.h)};
        SDL_RenderTexture(ctx_.sdl, bg.tex, nullptr, &d);
    }
}

LobbyMenuChoice LobbyScreen::run_menu(bool online_available) {
    // The generic bevel list dialog (sub_42DBCC) over the MAINMENU backdrop —
    // the SAME primitive and navigation model the *.BM help browser's picker
    // uses (bmscreen.cpp): up/down wrap the highlight, Enter selects, Esc backs
    // out. The original's own net rows play SFX 20 on any key (setup-screens.md
    // "Screen A"/"Screen B"), which is what the nav blip here mirrors.
    std::vector<const MenuRow*> rows;
    for (const MenuRow& r : kRows)
        if (online_available || !r.online) rows.push_back(&r);
    if (rows.empty()) return LobbyMenuChoice::Cancel;

    const int count = static_cast<int>(rows.size());
    int sel = 0;
    platform::FrameClock frame_clock(ctx_.window);
    const std::string title = "NETWORK GAME";

    float content_w = static_cast<float>(ctx_.front_font.measure(title));
    for (const MenuRow* r : rows)
        content_w = std::max(content_w, static_cast<float>(ctx_.front_font.measure(r->label)));
    content_w = std::max(content_w, kMinListW);

    while (true) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return LobbyMenuChoice::WindowClosed;
            if (ev.type != SDL_EVENT_KEY_DOWN || ev.key.repeat) continue;
            if (ev.key.key == SDLK_UP) {
                sel = (sel + count - 1) % count;
                ctx_.audio.play(20);
            } else if (ev.key.key == SDLK_DOWN) {
                sel = (sel + 1) % count;
                ctx_.audio.play(20);
            } else if (ev.key.key == SDLK_RETURN || ev.key.key == SDLK_KP_ENTER) {
                ctx_.audio.play(10);  // accept sting
                return rows[static_cast<std::size_t>(sel)]->choice;
            } else if (ev.key.key == SDLK_ESCAPE) {
                ctx_.audio.play(20);
                return LobbyMenuChoice::Cancel;
            }
        }

        ctx_.audio.update_music();
        draw_backdrop();
        const ListDialogLayout lay = draw_list_dialog(ctx_.sdl, ctx_.front_font, title, kListY,
                                                      content_w, count, count, 0);
        for (int i = 0; i < count; ++i) {
            const float ty = lay.item_y0 + static_cast<float>(i) * lay.item_h;
            const std::string label = rows[static_cast<std::size_t>(i)]->label;
            if (i == sel) {
                // Inverted-band selection: dark base-coat ink over the light band.
                draw_list_selection(ctx_.sdl, lay, i);
                ctx_.front_font.draw(ctx_.sdl, label, lay.item_x, ty, kDialogFillR, kDialogFillG,
                                     kDialogFillB);
            } else {
                ctx_.front_font.draw(ctx_.sdl, label, lay.item_x, ty, kDialogInkR, kDialogInkG,
                                     kDialogInkB);
            }
        }
        SDL_RenderPresent(ctx_.sdl);
        frame_clock.pace();
    }
}

bool LobbyScreen::run_code_entry(std::string& code, bool& window_closed) {
    // The sub_42E938 text-entry family at the CONFIRMED y=180 anchor — the same
    // call NetplayConnectScreen::run_join makes for its host:port line. Only
    // Crockford base-32 characters are accepted (uppercased as typed); anything
    // else buzzes and re-labels the prompt in place rather than entering a
    // character the server would reject.
    std::string entry;
    std::string label = "JOIN BY CODE:";
    platform::FrameClock frame_clock(ctx_.window);
    SDL_StartTextInput(ctx_.window);

    while (true) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) {
                SDL_StopTextInput(ctx_.window);
                window_closed = true;
                return false;
            }
            if (ev.type == SDL_EVENT_TEXT_INPUT) {
                for (const char* p = ev.text.text; p != nullptr && *p != '\0'; ++p) {
                    if (entry.size() >= kCodeLen) break;
                    const char up = static_cast<char>(std::toupper(static_cast<unsigned char>(*p)));
                    if (std::strchr(kCrockford, up) != nullptr) {
                        entry += up;
                    } else {
                        ctx_.audio.play(20);
                        label = "CODE IS 0-9 A-Z (NO I L O U)";
                    }
                }
            } else if (ev.type == SDL_EVENT_KEY_DOWN) {
                if (ev.key.key == SDLK_BACKSPACE) {
                    if (!entry.empty()) entry.pop_back();
                } else if (ev.key.key == SDLK_RETURN || ev.key.key == SDLK_KP_ENTER) {
                    if (entry.size() == kCodeLen) {
                        ctx_.audio.play(10);
                        SDL_StopTextInput(ctx_.window);
                        code = entry;
                        return true;
                    }
                    ctx_.audio.play(20);
                    label = "ENTER ALL 6 CHARACTERS";
                } else if (ev.key.key == SDLK_ESCAPE) {
                    ctx_.audio.play(20);
                    SDL_StopTextInput(ctx_.window);
                    return false;
                }
            }
        }

        ctx_.audio.update_music();
        draw_backdrop();
        draw_text_entry_dialog(ctx_.sdl, ctx_.front_font, kJoinPromptY, label, entry, "Join",
                               "Cancel");
        SDL_RenderPresent(ctx_.sdl);
        frame_clock.pace();
    }
}

#if defined(BOMBER_HAS_LOBBY)

namespace {

constexpr float kScreenW = 640.0f;  // renderer.hpp's kScreenW, what dialog_rect centres against
constexpr float kNameCol = 26.0f;   // seat-number column width inside a roster row

// Draw one centred line of the pinned outlined dialog text (sub_41696C).
void draw_centred(SDL_Renderer* ren, const FontTextures& font, const std::string& s, float y) {
    const float w = static_cast<float>(font.measure(s));
    draw_dialog_text(ren, font, s, (kScreenW - w) / 2.0f, y, kDialogInkR, kDialogInkG, kDialogInkB);
}

// The waiting room proper: the lobby CODE in the list dialog's pinned centred
// title strip (the one string the host reads out to friends, so it goes where
// the chrome already puts a prominent centred label), one item row per seat, and
// the key hints on outlined text lines in the window's reserved footer. Every
// pixel here comes from draw_list_dialog / draw_list_selection / draw_dialog_text
// — no new chrome. The LOCAL player's row carries the selection band so you can
// see which seat is yours at a glance.
void draw_room(ScreenContext& ctx, const net::LobbyFlow& flow, bool local_ready) {
    const std::vector<net::RosterEntry>& roster = flow.roster();
    const int rows = std::max(static_cast<int>(roster.size()), 1);
    const std::string title =
        "LOBBY CODE   " + (flow.code().empty() ? std::string("------") : flow.code());

    // The hints are part of the window's content, not something floating under
    // it: they are packed by MEASURED width and the window is sized to the
    // widest resulting line, so no hint can land outside the border. The ready
    // hint's two wordings differ in length, so both are budgeted at the longer
    // one and pressing SPACE never resizes the dialog.
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
    const HintBlock hints = pack_hint_lines(ctx.front_font, parts, budget, kHintWrapW);

    float content_w = static_cast<float>(ctx.front_font.measure(title));
    for (const net::RosterEntry& e : roster) {
        // seat column + name + the widest marker pair the row can show
        const float w = kNameCol + static_cast<float>(ctx.front_font.measure(e.name)) +
                        static_cast<float>(ctx.front_font.measure("  HOST  WAITING"));
        content_w = std::max(content_w, w);
    }
    content_w = std::max(content_w, kMinListW);
    content_w = std::max(content_w, hints.width);

    const ListDialogLayout lay =
        draw_list_dialog(ctx.sdl, ctx.front_font, title, kListY, content_w, rows, rows, 0,
                         static_cast<int>(hints.lines.size()));

    for (std::size_t i = 0; i < roster.size(); ++i) {
        const net::RosterEntry& e = roster[i];
        const float ty = lay.item_y0 + static_cast<float>(i) * lay.item_h;
        Uint8 r = kDialogInkR;
        Uint8 g = kDialogInkG;
        Uint8 b = kDialogInkB;
        if (e.seat == flow.my_seat()) {
            draw_list_selection(ctx.sdl, lay, static_cast<int>(i));
            r = kDialogFillR;
            g = kDialogFillG;
            b = kDialogFillB;
        }
        ctx.front_font.draw(ctx.sdl, std::to_string(e.seat + 1), lay.item_x, ty, r, g, b);
        ctx.front_font.draw(ctx.sdl, e.name, lay.item_x + kNameCol, ty, r, g, b);
        // Right-aligned status column. Words rather than tick/cross glyphs: the
        // original FON fonts have no check/cross codepoint, and drawing one would
        // mean inventing art.
        const std::string mark =
            std::string(e.is_host ? "HOST  " : "") + (e.ready ? "READY" : "WAITING");
        const float mw = static_cast<float>(ctx.front_font.measure(mark));
        ctx.front_font.draw(ctx.sdl, mark, lay.item_x + lay.item_w - mw, ty, r, g, b);
    }

    for (std::size_t i = 0; i < hints.lines.size(); ++i)
        draw_centred(ctx.sdl, ctx.front_font, hints.lines[i],
                     lay.footer_y0 + static_cast<float>(i) * lay.item_h);
}

// --- the PUBLIC GAMES browser (Phase 3) ------------------------------------

constexpr int kBrowseRows = 10;         // the *.BM help browser's own visible-row count
constexpr float kColGap = 12.0f;        // gap between two row columns
constexpr std::size_t kNameChars = 20;  // clamp for the untrusted server-supplied name
// The incompatible-build marker. A WORD, for the same reason draw_room's
// HOST/READY/WAITING are words: the original FON fonts carry no tick/cross
// codepoint, so a symbol would have to be drawn art. Matches the wording
// LobbyFlow::error() already uses for the refusal ("VERSION MISMATCH ...").
constexpr char kStaleMark[] = "VERSION";

std::string occupancy(const net::PublicLobby& l) {
    return std::to_string(l.players) + "/" + std::to_string(l.max);
}

// The lobby name is another player's typed node name arriving over the wire —
// untrusted input, same posture as the 1997 files: keep only codes the FON can
// actually draw and clamp the length, so no single row can widen the window off
// the 640-px screen.
std::string display_name(const std::string& raw) {
    std::string out;
    for (const char c : raw) {
        if (out.size() >= kNameChars) break;
        const unsigned char u = static_cast<unsigned char>(c);
        if (u >= 32 && u < 127) out += c;
    }
    return out.empty() ? std::string("GAME") : out;
}

// One frame of the browser list: the SAME sub_42DBCC list dialog the NETWORK
// GAME menu and the *.BM help picker draw, one item row per open public lobby,
// and the key hints in the window's reserved footer (draw_room's shape).
// Columns are laid out from the RIGHT edge of the item area — marker, code,
// occupancy — so the name takes what is left and a scrollbar (which narrows
// that area) simply shifts them.
//
// A row whose build_ok is false cannot be joined, so it is drawn in the chrome's
// own grey (kDialogDim*, dword_45C478 — the ink the button labels and the title
// strip already use) and carries the VERSION marker.
void draw_browser(ScreenContext& ctx, const std::vector<net::PublicLobby>& list, int sel, int top) {
    const std::string title = "PUBLIC GAMES";
    const int count = static_cast<int>(list.size());
    const int visible = std::min(count, kBrowseRows);

    // Column widths come from the WHOLE list, not just the visible window, so
    // the columns do not jump around as the list scrolls.
    float name_w = 0.0f;
    float occ_w = 0.0f;
    float code_w = 0.0f;
    for (const net::PublicLobby& l : list) {
        name_w = std::max(name_w, static_cast<float>(ctx.front_font.measure(display_name(l.name))));
        occ_w = std::max(occ_w, static_cast<float>(ctx.front_font.measure(occupancy(l))));
        code_w = std::max(code_w, static_cast<float>(ctx.front_font.measure(l.code)));
    }
    const float mark_w = static_cast<float>(ctx.front_font.measure(kStaleMark));
    // Same measured-footer contract as draw_room: the hints are window content,
    // so they can never overflow the border.
    const HintBlock hints = pack_hint_lines(
        ctx.front_font, {"ENTER = JOIN", "R = REFRESH", "ESC = BACK"}, {}, kHintWrapW);
    float content_w = name_w + occ_w + code_w + mark_w + 3.0f * kColGap;
    content_w = std::max(content_w, static_cast<float>(ctx.front_font.measure(title)));
    content_w = std::max(content_w, kMinListW);
    content_w = std::max(content_w, hints.width);

    const ListDialogLayout lay =
        draw_list_dialog(ctx.sdl, ctx.front_font, title, kListY, content_w, visible, count, top,
                         static_cast<int>(hints.lines.size()));
    const float mark_x = lay.item_x + lay.item_w - mark_w;
    const float code_x = mark_x - kColGap - code_w;
    const float occ_x = code_x - kColGap - occ_w;

    for (int i = top; i < top + visible; ++i) {
        const net::PublicLobby& l = list[static_cast<std::size_t>(i)];
        const int vi = i - top;
        const float ty = lay.item_y0 + static_cast<float>(vi) * lay.item_h;
        Uint8 r = l.build_ok ? kDialogInkR : kDialogDimR;
        Uint8 g = l.build_ok ? kDialogInkG : kDialogDimG;
        Uint8 b = l.build_ok ? kDialogInkB : kDialogDimB;
        if (i == sel) {
            // Inverted-band selection, as everywhere else. On the light band the
            // readable ink is the dark base coat; a DIMMED variant of it would be
            // a new colour, so a selected incompatible row leans on its VERSION
            // marker (and the buzz on Enter) instead of a third ink.
            draw_list_selection(ctx.sdl, lay, vi);
            r = kDialogFillR;
            g = kDialogFillG;
            b = kDialogFillB;
        }
        ctx.front_font.draw(ctx.sdl, display_name(l.name), lay.item_x, ty, r, g, b);
        const std::string occ = occupancy(l);
        const float ow = static_cast<float>(ctx.front_font.measure(occ));
        ctx.front_font.draw(ctx.sdl, occ, occ_x + occ_w - ow, ty, r, g, b);
        ctx.front_font.draw(ctx.sdl, l.code, code_x, ty, r, g, b);
        if (!l.build_ok) ctx.front_font.draw(ctx.sdl, kStaleMark, mark_x, ty, r, g, b);
    }

    for (std::size_t i = 0; i < hints.lines.size(); ++i)
        draw_centred(ctx.sdl, ctx.front_font, hints.lines[i],
                     lay.footer_y0 + static_cast<float>(i) * lay.item_h);
}

}  // namespace

LobbyRoomResult LobbyScreen::run_online(net::LobbyFlow& flow, ChatOverlay& chat, bool host,
                                        const std::string& code) {
    LobbyRoomResult result;

    platform::FrameClock frame_clock(ctx_.window);
    const Sprite* winz = &ctx_.assets.frontend_pcx("WINZ");
    const std::string ok_label = ctx_.assets.getstring(27, " Ok ");
    bool local_ready = false;
    std::string failure;  // non-empty once Phase::Failed latched -> the ack modal

    while (true) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) {
                result.window_closed = true;
                return result;
            }
            // The chat overlay gets first refusal (chat_overlay.hpp): while it
            // is open it eats every key, so typing never also drives the room.
            if (chat.handle_event(ev, ctx_)) continue;
            if (ev.type != SDL_EVENT_KEY_DOWN || ev.key.repeat) continue;

            if (!failure.empty()) {
                // sub_414340's own key loop: the nav blip for ANY real key, and
                // it closes only on Enter / Space / Esc.
                if (ev.key.key == SDLK_RETURN || ev.key.key == SDLK_KP_ENTER ||
                    ev.key.key == SDLK_SPACE || ev.key.key == SDLK_ESCAPE)
                    return result;  // back to the NETWORK GAME menu
                ctx_.audio.play(20);
                continue;
            }
            if (ev.key.key == SDLK_ESCAPE) {
                ctx_.audio.play(20);
                return result;  // leave the lobby (the flow's dtor closes the socket)
            }
            // The room's controls are live only while actually IN the room —
            // during Connecting/Rendezvous there is nothing to toggle.
            if (flow.phase() != net::LobbyFlow::Phase::InLobby) continue;
            if (ev.key.key == SDLK_SPACE || ev.key.key == SDLK_R) {
                local_ready = !local_ready;
                flow.set_ready(local_ready);
                ctx_.audio.play(10);
            } else if (ev.key.key == SDLK_RETURN || ev.key.key == SDLK_KP_ENTER) {
                if (flow.is_host()) {
                    flow.start_match();  // the server validates all-ready + build parity
                    ctx_.audio.play(10);
                } else {
                    // SFX 40 — the net non-host "you can't do that here" buzz the
                    // original fires on its own wait loops (frontend-flow.md §SFX
                    // 40, quoted in multiplayer-deep.md §2.5).
                    ctx_.audio.play(40);
                }
            }
        }

        flow.step(static_cast<std::int64_t>(SDL_GetTicks()));

        if (flow.phase() == net::LobbyFlow::Phase::Ready) {
            // The transport is punched and connected; the server's authoritative
            // parameters go straight to the match core.
            result.ready = true;
            result.is_host = flow.is_host();
            result.seed = flow.match_start().seed;
            result.local_seats_mask = flow.match_start().local_seats_mask;
            return result;
        }
        if (flow.phase() == net::LobbyFlow::Phase::Failed && failure.empty()) {
            // LobbyFlow::error() strings are already player-facing ("LOBBY IS
            // FULL", "VERSION MISMATCH - UPDATE THE GAME", ...).
            failure = flow.error().empty() ? std::string("CONNECTION FAILED") : flow.error();
            ctx_.audio.play(20);
        }

        ctx_.audio.update_music();
        draw_backdrop();
        if (!failure.empty()) {
            draw_acknowledge_dialog(ctx_.sdl, ctx_.front_font, winz, "NETWORK ERROR", failure,
                                    ok_label, kDialogInkR, kDialogInkG, kDialogInkB);
        } else if (flow.phase() == net::LobbyFlow::Phase::InLobby) {
            draw_room(ctx_, flow, local_ready);
        } else if (flow.phase() == net::LobbyFlow::Phase::Rendezvous) {
            draw_acknowledge_dialog(ctx_.sdl, ctx_.front_font, winz, "STARTING MATCH",
                                    "CONNECTING TO PLAYER...", ok_label, kDialogInkR, kDialogInkG,
                                    kDialogInkB);
        } else {
            draw_acknowledge_dialog(ctx_.sdl, ctx_.front_font, winz,
                                    host ? "HOSTING A GAME" : "JOINING " + code,
                                    "CONTACTING THE SERVER...", ok_label, kDialogInkR, kDialogInkG,
                                    kDialogInkB);
        }
        // Last, so the panel sits over whatever the room drew. No pump() here:
        // this screen already stepped the flow above, and the overlay shares it.
        chat.draw(ctx_);
        SDL_RenderPresent(ctx_.sdl);
        frame_clock.pace();
    }
}

bool LobbyScreen::run_public_browser(const OnlineConfig& ocfg, net::UdpTransport& transport,
                                     std::string& code, bool& window_closed) {
    net::LobbyFlow::Config cfg;
    cfg.server_url = ocfg.server_url;
    cfg.stun_host = ocfg.stun_host;
    cfg.stun_port = ocfg.stun_port;
    cfg.player_name = ocfg.player_name;
    cfg.build_hash = net::build_hash();  // the server flags every row against ours

    net::LobbyClient client;
    net::LobbyFlow flow(cfg, transport, client);
    // browse_public() rides the control connection up and drops back to Idle when
    // the answer lands, so the ONLY reliable "the answer arrived" signal is the
    // revision counter — phase() is Idle before and after (lobby_flow.hpp).
    unsigned asked_rev = flow.public_list_revision();
    flow.browse_public();

    platform::FrameClock frame_clock(ctx_.window);
    const Sprite* winz = &ctx_.assets.frontend_pcx("WINZ");
    const std::string ok_label = ctx_.assets.getstring(27, " Ok ");
    const std::string head = "PUBLIC GAMES";
    int sel = 0;
    int top = 0;
    std::string failure;  // non-empty once Phase::Failed latched -> the ack modal

    while (true) {
        const std::vector<net::PublicLobby>& list = flow.public_lobbies();
        int count = static_cast<int>(list.size());
        bool searching = flow.public_list_revision() == asked_rev;

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) {
                window_closed = true;
                return false;
            }
            if (ev.type != SDL_EVENT_KEY_DOWN || ev.key.repeat) continue;
            const SDL_Keycode key = ev.key.key;

            if (!failure.empty()) {
                // sub_414340's own key loop, exactly as run_online drives it.
                if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE ||
                    key == SDLK_ESCAPE)
                    return false;
                ctx_.audio.play(20);
                continue;
            }
            if (key == SDLK_ESCAPE) {
                ctx_.audio.play(20);
                return false;  // back to the NETWORK GAME menu, nothing joined
            }
            if (key == SDLK_R || key == SDLK_F5) {
                // Re-ask. Ignored while an answer is still outstanding — the flow
                // would refuse it anyway (browse_public is Idle/Failed-only).
                if (!searching) {
                    asked_rev = flow.public_list_revision();
                    flow.browse_public();
                    searching = true;
                    ctx_.audio.play(20);
                }
                continue;
            }
            if (searching) continue;  // the transient modal has nothing to steer
            if (count == 0) {
                // The answered-but-empty state draws the acknowledge modal, whose
                // dismiss keys close it (the *.BM browser's empty-glob path does
                // the same); every other key blips.
                if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE) return false;
                ctx_.audio.play(20);
                continue;
            }

            // The sub_42DBCC list widget's own navigation model (bmscreen.cpp's
            // HelpBrowser): arrows wrap, PageUp/PageDown/Home/End clamp,
            // Enter/Space picks. No letter-jump here — R is the refresh key.
            switch (key) {
                case SDLK_UP:
                    sel = (sel + count - 1) % count;
                    ctx_.audio.play(20);
                    break;
                case SDLK_DOWN:
                    sel = (sel + 1) % count;
                    ctx_.audio.play(20);
                    break;
                case SDLK_PAGEUP:
                    sel = std::max(0, sel - kBrowseRows);
                    ctx_.audio.play(20);
                    break;
                case SDLK_PAGEDOWN:
                    sel = std::min(count - 1, sel + kBrowseRows);
                    ctx_.audio.play(20);
                    break;
                case SDLK_HOME:
                    sel = 0;
                    ctx_.audio.play(20);
                    break;
                case SDLK_END:
                    sel = count - 1;
                    ctx_.audio.play(20);
                    break;
                case SDLK_RETURN:
                case SDLK_KP_ENTER:
                case SDLK_SPACE:
                    if (!list[static_cast<std::size_t>(sel)].build_ok) {
                        // SFX 40 — the same "you can't do that here" buzz the
                        // waiting room fires at a non-host pressing Enter. The
                        // server would refuse this join with VERSION MISMATCH, so
                        // the row is a dead end by design, not a failure to show.
                        ctx_.audio.play(40);
                        break;
                    }
                    ctx_.audio.play(10);  // accept sting
                    code = list[static_cast<std::size_t>(sel)].code;
                    return true;  // -> the caller's UNCHANGED join path
                default: break;
            }
        }

        flow.step(static_cast<std::int64_t>(SDL_GetTicks()));
        if (flow.phase() == net::LobbyFlow::Phase::Failed && failure.empty()) {
            failure = flow.error().empty() ? std::string("CONNECTION FAILED") : flow.error();
            ctx_.audio.play(20);
        }
        // A refresh can shrink the list under the cursor; re-clamp before drawing.
        count = static_cast<int>(list.size());
        if (sel >= count) sel = std::max(0, count - 1);
        if (sel < top) top = sel;
        if (sel >= top + kBrowseRows) top = sel - kBrowseRows + 1;
        top = std::min(top, std::max(0, count - kBrowseRows));

        ctx_.audio.update_music();
        draw_backdrop();
        if (!failure.empty()) {
            draw_acknowledge_dialog(ctx_.sdl, ctx_.front_font, winz, "NETWORK ERROR", failure,
                                    ok_label, kDialogInkR, kDialogInkG, kDialogInkB);
        } else if (flow.public_list_revision() == asked_rev) {
            draw_acknowledge_dialog(ctx_.sdl, ctx_.front_font, winz, head, "SEARCHING FOR GAMES...",
                                    ok_label, kDialogInkR, kDialogInkG, kDialogInkB);
        } else if (count == 0) {
            const std::string body = "NO PUBLIC GAMES";
            draw_acknowledge_dialog(ctx_.sdl, ctx_.front_font, winz, head, body, ok_label,
                                    kDialogInkR, kDialogInkG, kDialogInkB);
            // sub_414340's width is PINNED to its own two lines, so this hint
            // cannot widen the window the way the list dialogs' can — it wraps
            // to the pinned width instead, which is what keeps it from
            // stretching past both borders.
            const DialogRect win = acknowledge_dialog_rect(ctx_.front_font, head, body);
            const HintBlock hints =
                pack_hint_lines(ctx_.front_font, {"R = REFRESH", "ESC = BACK"}, {}, win.w);
            const float lh = static_cast<float>(ctx_.front_font.line_height());
            for (std::size_t i = 0; i < hints.lines.size(); ++i)
                draw_centred(ctx_.sdl, ctx_.front_font, hints.lines[i],
                             win.y + win.h + 10.0f + static_cast<float>(i) * lh);
        } else {
            draw_browser(ctx_, list, sel, top);
        }
        SDL_RenderPresent(ctx_.sdl);
        frame_clock.pace();
    }
}

#endif  // BOMBER_HAS_LOBBY

}  // namespace bomber::game
