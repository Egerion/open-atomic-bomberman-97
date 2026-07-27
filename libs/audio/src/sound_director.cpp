#include "bomber/audio/sound_director.hpp"

namespace bomber::game {

void SoundDirector::reset() {
    pending_.clear();
    pickups_.fill(0);
    wall_slam_id_ = -1;
}

void SoundDirector::on_tick(const sim::State& s) {
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (s.tick >= it->first) {
            audio_.play(it->second);
            it = pending_.erase(it);
        } else {
            ++it;
        }
    }

    for (const auto& ev : s.events) {
        switch (ev.type) {
            case sim::Event::Type::BombPlaced: {
                // "Fire In The Hole" taunt (docs/re/id-audit.md item 1; VALUELST
                // 650/651, SOUNDLST 1200 group; sub_41F29B pseudo.c ~23360-23368,
                // the plain single-bomb-drop path — the file's own comment reads
                // "after laying out a HUGE string of bombs"). Literal decompile:
                //   v60 = getvalue(651);
                //   if (v61 >= v60 && player.bombCount - 1 == bombs_placed_before)
                //       if (!(rand() % max(1, getvalue(650)))) sub_427961(1200);
                // `v61` is read from a register the decompiler itself flags
                // "possibly undefined" at this call site (no visible assignment
                // anywhere in sub_41F29B) — its provenance can't be pinned from
                // the text decompile alone. The best-supported reading, matching
                // the VALUELST author's own comment ("what constitutes 'many'
                // dropped bombs") and the adjacent code (which already holds
                // player+86 = max_bombs in a register a few lines up for the
                // drop-eligibility gate), is that v61 is that same cached
                // max_bombs read. Ported on that basis: a player whose bomb-count
                // powerup level is >= id 651 ("many") who has just placed the
                // LAST bomb of their current allotment (bombs_placed, already
                // incremented by BombSystem::place before this event, equals
                // max_bombs) rolls a 1-in-id-650 chance. Layering: the counter
                // reads straight off the already-hashed sim state the event
                // carries (no new State field), and the roll uses AudioEngine's
                // RNG, never State::rng (determinism contract rule 6) — purely
                // cosmetic, no golden impact. Not gated to the single-drop-only
                // path the original uses (spooge's multi-drop loop shares
                // BombSystem::place and could also complete the cap) — an
                // unobservable, cosmetic-only widening, not worth a new event
                // field to disambiguate.
                //
                // SOUNDLST correction: id-audit.md described 1200-1203 ("clear/
                // fireinh/lookout/litemup") as the taunt and 1204+ as an
                // unrelated "runaway/DMB/ZAE/JMB" pool. The raw SOUNDLST.RES
                // shows NO gap or comment between 1203 and 1204 — the block
                // is contiguous through id 1279, and the file's own closing
                // comment reads `;1299 is last "huge string of bombs" sound
                // value`. So the whole 1200-1299 span is this ONE taunt group
                // (mirroring the 700-999 death-taunt range fix already in this
                // file), not just the first four slots.
                if (ev.player >= 0 && ev.player < sim::kMaxPlayers) {
                    const sim::Player& pl = s.players[ev.player];
                    if (pl.max_bombs >= s.tuning.taunt_many_bombs &&
                        pl.bombs_placed == pl.max_bombs &&
                        audio_.chance(s.tuning.taunt_many_chance))
                        audio_.play(1200);
                }
                // Diarrhea/super drop = random "poops" splat (SOUNDLST 550-554,
                // sub_41F29B v112 branch); a normal drop is 100/101.
                if (ev.data)
                    audio_.play(550);
                else
                    audio_.play(100);
                break;
            }
            case sim::Event::Type::BombKicked: audio_.play(120); break;
            case sim::Event::Type::Explosion: audio_.play(200); break;
            // No sound at clock-zero: the original plays the tie/draw voice
            // (1700-1999) only on the DRAW result screen, via the blocking
            // sub_427BFB(1700) (batch_0x4293E5.cpp:1097) — NOT mid-round when
            // ticks_left hits 0 (sudden death continues after that). The draw
            // voice is already handled on the result screen (game_app.cpp).
            // Playing it here fired it prematurely mid-round AND duplicated the
            // result-screen sting. Removed 2026-07-22 (oracle audio audit).
            case sim::Event::Type::Hurry:
                // "HURRY!" voice callout the frame the walls start closing. The
                // per-frame game loop (sub_42A191 ~0x42A2C4) latches on
                // dword_464984 and fires sub_427961(2700) exactly once, right
                // before it flashes the "hurry" banner (aHurry). SOUNDLST labels
                // 2700 "hurry" with the comment "2799 is last hurry up! sound".
                // 39 clips are authored there; the load-time cull keeps a random
                // FIVE of them per session (sub_42814B: cull(2700, 2799, 5)), and
                // play() then walks that surviving run.
                audio_.play(2700);
                break;
            case sim::Event::Type::WallClosed:
                // sub_426818 (docs/re/facts.md "Wall-slam SFX", `sub_4278F2`
                // call site) draws `dword_462244 = rand() % 3` ONCE when the
                // enclosure arms and replays SOUNDLST 140+dword_462244 for
                // every tile that drops in the sequence — not a fresh pick
                // per drop. Latch the roll on the first WallClosed since the
                // last reset() (one per round, matching the original's
                // per-arm draw) and reuse it thereafter.
                if (wall_slam_id_ < 0) wall_slam_id_ = 140 + audio_.roll(3);
                audio_.play_exact(wall_slam_id_);
                break;
            case sim::Event::Type::BombPunched:
                // The glove swings on every press (the event fires regardless so
                // the punch pose plays), but the original only plays the "kbomb"
                // hit SFX when a bomb is actually launched: sub_427961(150) sits
                // inside sub_424A50's `if (bomb ahead)`. ev.data carries that hit
                // flag. SOUNDLST 150/151 are "punching a bomb"; sub_427961(150)
                // random-picks across the contiguously loaded 150,151 slots.
                if (ev.data) audio_.play(150);
                break;
            case sim::Event::Type::BombBounced: audio_.play(160); break;
            case sim::Event::Type::BombStopped: audio_.play(130); break;   // "bombstop"
            case sim::Event::Type::DropRefused:
                // Drop attempted on a warphole (sub_41F29B ~23354): the
                // placement is skipped and sub_427961(40) plays — SOUNDLST
                // 40/41 "enrt1"/"enrt2" load contiguously, so the original
                // random-picks across both. The sim already suppresses the
                // event for disease-forced auto-drops (silent in the binary).
                // NOT gated to a human/local player: sub_427961 is a global
                // SFX and the event carries any owner, so an AI's warphole drop
                // buzzes too — do not add an `ev.player == 0` check here.
                audio_.play(40);
                break;
            case sim::Event::Type::JellyBounced:
                // The ONLY debounced sound in the binary: `sub_423776` reaches
                // the jelly reversal through `sub_427ABB(135)`, not the usual
                // `sub_427961`, so a jelly bomb pinballing between two walls
                // cannot re-trigger "bombboun" more than once every 3 frames.
                // Its non-jelly sibling one branch down (`sub_427961(130)`,
                // BombStopped) is NOT debounced — a bomb only stops once.
                audio_.play_debounced(135, s.tick);
                break;
            case sim::Event::Type::BombGrabbed:
                // Pickup "grab1". sub_41F29B's +92 grab branch (~0x41F4CA) calls
                // sub_424AF4(bomb, player), which cross-links the pair and fires
                // sub_427961(170). CORRECTION (2026-07-27): the 170 group is not
                // {grab1, grab2} — SOUNDLST loads 170..175 with no gap, so the
                // group is SIX members and the four "bmbthrw" clips are IN it.
                // facts.md's earlier "the bmbthrw ids are dead assets, never
                // played" was wrong: nothing calls sub_427961(172), but 170's
                // group walk reaches them. See docs/re/sound-engine.md.
                audio_.play(170);
                break;
            case sim::Event::Type::BombThrown:
                // Silent by design. The carried-bomb RELEASE in sub_41F29B's +37
                // block (~0x41F3E5) launches the held bomb via sub_424987 with NO
                // sub_427961 call — throwing plays no sound at the release instant.
                // (No call site names 171-175, but they are NOT dead assets: they
                // sit inside the 170 "grab" group's contiguous run, so the grab
                // pick reaches them. See BombGrabbed below.)
                // The audible part of a throw is the in-flight/settle "bmdrop3"
                // (160), emitted as BombBounced from the flight code — verified by
                // an exhaustive sub_42331C sound census: its only calls are
                // 160 (fly, case 2), 130 (kick stop, NOT flying), 200 (explode),
                // 120 (kick). A thrown bomb therefore never plays 130.
                break;
            // 360 is a FOUR-member group (bombhit1..4) — the old 360..362 range
            // silently dropped bombhit4.
            case sim::Event::Type::HeadHit: audio_.play(360); break;
            // Stage actors (docs/re/stage-actors.md §7): sub_427961(350) /
            // sub_427961(1330) — group bases like every other call site, so the
            // trampoline picks across 350..353 and the warp across 1330..1332.
            case sim::Event::Type::TrampolineBounce: audio_.play(350); break;
            case sim::Event::Type::WarpUsed: audio_.play(1330); break;
            case sim::Event::Type::Infected: {
                // Skull voice: 1-in-3 the per-disease line (the 3000+50*idx
                // block), else the generic "oh no" (2300) — as sub_41DFB6.
                int base = 3000 + 50 * ev.data;
                if (audio_.chance(3))
                    audio_.play(base);
                else
                    audio_.play(2300);
                break;
            }
            case sim::Event::Type::PowerupPicked: {
                // sub_41E21E (batch_0x41DAA7.cpp:495-503): v8 defaults to the
                // pickup voice (400, or 135 for jelly, case 0xA), but the
                // "You are now AWESOME" MILESTONE OVERWRITES it to 1400 — then a
                // SINGLE sub_427961(v8). So on a milestone the pickup voice is
                // REPLACED by 1400, not layered, and plays immediately. The port
                // used to play BOTH (pickup voice now + 1400 on an invented
                // +8-tick delay) — two sounds where the original plays one.
                const int n = ++pickups_[ev.player];
                // 7th powerup, then every 5th after; the counter wraps to 7 past
                // 50 (sub_41E21E tail).
                const bool milestone = (n == 7 || (n > 7 && (n - 7) % 5 == 0));
                if (n > 50) pickups_[ev.player] = 7;
                if (milestone)
                    audio_.play(1400);  // replaces the pickup voice
                else if (ev.data == static_cast<std::int8_t>(sim::PowerupType::Jelly))
                    audio_.play(135);  // jelly boing
                else
                    // 400 (woohoo1) starts the pickup block (was 401, dropping it).
                    audio_.play(400);
                break;
            }
            case sim::Event::Type::PlayerDied: {
                audio_.play(300);
                // Post-death taunt from a survivor (VALUELST id 95: 1-in-N,
                // sub_427961(700) call site). FIXED (docs/re/id-audit.md):
                // the taunt group is SOUNDLST 700..999 ("after a player
                // death", the file's own comment block starts at 701 and
                // ends "999 is the last possible death taunt"), NOT
                // 500..999 — the old range wrongly overlapped the unrelated
                // "ploppy poop" splat group at 550-554 (the diarrhea-bomb
                // drop sound, played elsewhere via BombPlaced), so a dying
                // player had a small chance of "taunting" with a fart splat.
                if (audio_.chance(s.tuning.taunt_chance))
                    pending_.push_back({s.tick + 25, 700});
                break;
            }
            default: break;
        }
    }
}

}  // namespace bomber::game
