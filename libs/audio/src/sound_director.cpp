#include "bomber/audio/sound_director.hpp"

namespace bomber::game {

namespace {

// The death-anim overlay's two constants (see the PlayerDied case).
// `340 + anim`, where anim is 1-based: SOUNDLST 341 is `burnedup`.
constexpr int kDeathOverlayBase = 340;  // the literal added at 0x41DDDF
// VALUELST id 105, authored 24 — the death-animation count the original's roll
// takes its modulo from (sub_41DE63 @0x41DEFD). Not read live from VALUELST:
// SoundDirector talks only to SoundSink and has no values handle, so a modified
// VALUELST would not move this. Authored-24 is the shipped install's value.
constexpr int kDeathAnimCount = 24;

}  // namespace

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
                // THE SPOOGER RUN IS SILENT — the whole block below belongs to
                // the PLAIN drop only. sub_41F29B's spooge loop (0x420AAE-
                // 0x420B6E) walks one tile at a time in the facing direction and
                // its ONLY call is the bomb constructor sub_41EB13 (@0x420B69);
                // every one of its four exits (occupied tile / powerup / blocked /
                // allotment spent) jumps to 0x420B73, which jumps to 0x420CEC —
                // PAST the entire sound block at 0x420C26-0x420CC1 (the 40 buzz,
                // the 1200 taunt, the 550 splat and the 100 drop). So a spooged
                // string of bombs makes no noise at all, however long it is. The
                // port used to fire a drop SFX per bomb in the string, which under
                // the 5-voice cap also swallowed everything else in that frame.
                //
                // Telling the two apart WITHOUT a sim change: the original's own
                // arithmetic separates them. The plain drop places at the player's
                // OWN tile (sub_41F29B's drop branch reads the player's cell);
                // the spooge loop advances `cx += dx` BEFORE its first placement,
                // so it can never target the tile the player stands on. Compare
                // the event's tile against the player's and take the run as silent.
                if (ev.player >= 0 && ev.player < sim::kMaxPlayers) {
                    const sim::Player& layer = s.players[ev.player];
                    if (layer.tile_x() != ev.x || layer.tile_y() != ev.y) break;
                }
                // "Fire In The Hole" taunt (docs/re/id-audit.md item 1; VALUELST
                // 650/651, SOUNDLST 1200 group; sub_41F29B pseudo.c ~23360-23368,
                // the plain single-bomb-drop path — the file's own comment reads
                // "after laying out a HUGE string of bombs"). The original's
                // shape: it loads the "many bombs" threshold from getvalue(651),
                // requires an unnamed register value to be >= that threshold AND
                // the player's bomb count minus one to equal the number of bombs
                // already placed, and only then rolls 1-in-max(1, getvalue(650))
                // before playing SOUNDLST 1200 via sub_427961.
                // The register holding the left-hand side of the threshold test
                // has no visible assignment anywhere in sub_41F29B, so its
                // provenance can't be pinned from the recovered source alone. The
                // best-supported reading, matching the VALUELST author's own
                // comment ("what constitutes 'many' dropped bombs") and the
                // adjacent code (which already holds player+86 = max_bombs in a
                // register a few lines up for the drop-eligibility gate), is that
                // it is that same cached max_bombs read. Ported on that basis: a
                // player whose bomb-count
                // powerup level is >= id 651 ("many") who has just placed the
                // LAST bomb of their current allotment (bombs_placed, already
                // incremented by BombSystem::place before this event, equals
                // max_bombs) rolls a 1-in-id-650 chance. Layering: the counter
                // reads straight off the already-hashed sim state the event
                // carries (no new State field), and the roll uses the sink's
                // RNG, never State::rng (determinism contract rule 6) — purely
                // cosmetic, no golden impact. The spooge run can no longer reach
                // it: the plain-drop gate above returns first, matching the
                // original's jump PAST this whole block (0x420B73 -> 0x420CEC).
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
                // sub_41F29B's forced-drop branch); a normal drop is 100/101.
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
            // THE TWO SILENT BOMB EVENTS. Grouped because they behave
            // identically (nothing), but they are silent for different reasons.
            //
            // BombThrown: the carried-bomb RELEASE in sub_41F29B's +37 block
            // (~0x41F3E5) launches the held bomb via sub_424987 with NO
            // sub_427961 call, so throwing makes no sound at the release
            // instant. (No call site names 171-175, but they are NOT dead
            // assets: they sit inside the 170 "grab" group's contiguous run, so
            // the grab pick reaches them — see BombGrabbed above.) The audible
            // part of a throw is the in-flight/settle "bmdrop3" (160), emitted
            // as BombBounced from the flight code — verified by an exhaustive
            // sub_42331C census: its only calls are 160 (fly, case 2), 130 (kick
            // stop, NOT flying), 200 (explode), 120 (kick). A thrown bomb
            // therefore never plays 130.
            //
            // HeadHit: SETTLED 2026-07-28, closing the contested item. SOUNDLST
            // 360-363 (bombhit1..4) is a real, loaded group that NOTHING ever
            // asks for. An exhaustive byte-level scan of every `call rel32` in
            // BEGTEXT finds 87 direct calls into the play primitives (70 x
            // sub_427961, 4 x sub_427BFB, 2 x sub_4278F2, 1 x sub_427ABB, plus
            // the internals) and not one names 360-363. The five sites whose id
            // is not a literal are all accounted for: 3000+50*disease
            // (0x41E03B), the powerup local (0x41E549 — its only four writers
            // store -1, 400, 135, 1400), sub_427ABB forwarding its own argument
            // (0x427B13), the death overlay 340+field (0x41DDE4) and the wall
            // slam 140+rand%3 (0x426A55). No indirect path exists either: the
            // literal address of sub_427961, or of any sibling primitive,
            // appears NOWHERE in the image, code or data, so there is no
            // function-table dispatch the scan could have stepped over. And
            // sub_421F7E, the head-hit handler itself, makes no audio call at
            // all. The 2026-07-27 note that "widened" this group from 360-362 to
            // 360-363 answered the wrong question — the range was never the
            // issue, the CALL was. Do not re-add a cue here;
            // tests/audio/test_sound_director.cpp pins both silences.
            case sim::Event::Type::BombThrown:
            case sim::Event::Type::HeadHit: break;
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
                // sub_41E21E (batch_0x41DAA7.cpp:495-503): the sound id it will
                // play defaults to the pickup voice (400, or 135 for jelly,
                // case 0xA), but the "You are now AWESOME" MILESTONE OVERWRITES
                // it with 1400 — then there is a SINGLE sub_427961 call on that
                // one id. So on a milestone the pickup voice is
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
                // The death-ANIM overlay, PINNED 2026-07-28 (docs/re/
                // sound-engine.md §10 — task #22, previously open because the
                // field could not be identified). The death handler sub_41DCB2
                // follows its scream group with `sub_4278F2(340 + actor[+4])` at
                // 0x41DDE4, an EXACT-slot play with no group walk. `actor[+4]`
                // is the DEATH ANIMATION INDEX: its caller sub_41DE63 rolls
                // `rand() % getvalue(105) + 1` and stores it at 0x41DF04, and
                // VALUELST id 105 is authored 24 with the comment "how many
                // different death animations do we have? (die 1 through die
                // 24)". Two further locks on the reading: sub_41F29B special-
                // cases `actor[+4] == 9` to float the body upwards by
                // getvalue(106) ("death anim #9 ... the angel"), and the index is
                // replicated to peers as its own network field.
                //
                // WHY MOST DEATHS ARE STILL SILENT, and why some of them are not
                // silent in the way you would expect: 340+v spans 341..364, and
                // SOUNDLST only occupies NINE of those slots. 341 is `burnedup`,
                // the clip the overlay was written for. But 350-353 and 360-363
                // are the `trampo`/`bombhit` blocks, authored for entirely
                // different cues INSIDE the range this overlay reserved — and
                // because sub_4278F2 addresses a slot directly, a death that
                // rolls anim 10-13 or 20-23 plays one of those. That is the
                // ORIGINAL's data collision, reproduced here deliberately; see
                // the doc before "fixing" it. The other 15 values hit empty slots
                // and sub_4278F2 returns without a sound, which SoundBank's own
                // null-name early-out already reproduces.
                //
                // The draw is on the PRESENTATION rng (root CLAUDE.md rule 6).
                // In the original this index is a real gameplay value — it picks
                // the death sprite and travels over the wire — so peers hear the
                // same overlay; here they need not, which is a cosmetic-only
                // divergence and the price of keeping State::rng untouched.
                audio_.play_exact(kDeathOverlayBase + audio_.roll(kDeathAnimCount) + 1);
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
