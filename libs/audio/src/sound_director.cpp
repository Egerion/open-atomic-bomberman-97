#include "bomber/audio/sound_director.hpp"

#include "bomber/audio/death_anim.hpp"

namespace bomber::game {

void SoundDirector::reset() {
    pending_.clear();
    pickups_.fill(0);
    wall_slam_id_ = -1;
}

void SoundDirector::drain_pending(const sim::State& s) {
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (s.tick < it->first) {
            ++it;
            continue;
        }
        audio_.play(it->second);
        it = pending_.erase(it);
    }
}

void SoundDirector::on_bomb_placed(const sim::State& s, const sim::Event& ev) {
    const sim::Player* layer =
        (ev.player >= 0 && ev.player < sim::kMaxPlayers) ? &s.players[ev.player] : nullptr;

    // A SPOOGED RUN IS ENTIRELY SILENT, so everything below belongs to the plain
    // drop only: sub_41F29B's spooge loop (0x420AAE-0x420B6E) exits to 0x420CEC,
    // PAST the whole sound block at 0x420C26-0x420CC1. Telling the two apart
    // without a sim change: the plain drop places at the player's OWN tile,
    // while the spooge loop advances `cx += dx` before its first placement and
    // so can never target it.
    if (layer && (layer->tile_x() != ev.x || layer->tile_y() != ev.y)) return;

    // "Fire In The Hole" taunt: the SOUNDLST 1200 group, gated on VALUELST 651
    // ("many" bombs) and rolled 1-in-650 (sub_41F29B ~23360-23368).
    //
    // NOT a clean extraction — the threshold's left-hand register has no visible
    // assignment in sub_41F29B. Read as the cached max_bombs a few lines above
    // it, which matches the VALUELST author's own comment; see
    // docs/re/id-audit.md item 1 before treating this as pinned.
    if (layer && layer->max_bombs >= s.tuning.taunt_many_bombs &&
        layer->bombs_placed == layer->max_bombs && audio_.chance(s.tuning.taunt_many_chance))
        audio_.play(1200);

    // Diarrhea/super drop = "poops" splat (550-554, sub_41F29B's forced-drop
    // branch); a plain drop is 100/101.
    audio_.play(ev.data ? 550 : 100);
}

void SoundDirector::on_powerup_picked(const sim::Event& ev) {
    // sub_41E21E holds ONE id and makes ONE sub_427961 call on it, so the
    // "AWESOME" milestone REPLACES the pickup voice rather than layering over
    // it. (The port once played both, the second on an invented +8-tick delay.)
    const int n = ++pickups_[ev.player];
    // 7th powerup, then every 5th after; the counter wraps to 7 past 50.
    const bool milestone = (n == 7 || (n > 7 && (n - 7) % 5 == 0));
    if (n > 50) pickups_[ev.player] = 7;
    if (milestone) {
        audio_.play(1400);
        return;
    }
    if (ev.data == static_cast<std::int8_t>(sim::PowerupType::Jelly)) {
        audio_.play(135);  // jelly boing, case 0xA
        return;
    }
    audio_.play(400);  // woohoo1, the base of the pickup block
}

void SoundDirector::on_player_died(const sim::State& s, const sim::Event& ev) {
    audio_.play(300);
    // The death-anim overlay: sub_41DCB2 follows the scream group with
    // `sub_4278F2(340 + actor[+4])` at 0x41DDE4 — an exact-slot play, no group
    // walk. Only 9 of the 24 slots are authored, and two of those runs belong to
    // other cues, so most deaths are silent and some play a trampoline or
    // bombhit clip. That is the ORIGINAL's data collision, reproduced
    // deliberately; read docs/re/sound-engine.md §10 before "fixing" it.
    //
    // NEITHER SIDE ROLLS the index. The renderer picks the corpse sprite from
    // the same pure derivation, on values both already share, so the sound
    // cannot describe a different corpse — see bomber/audio/death_anim.hpp.
    const int anim = death_anim_index(s.tick, ev.player);
    audio_.play_exact(death_overlay_sound(anim), s.tick);
    // Post-death taunt from a survivor: VALUELST 95, 1-in-N, group base 700.
    // The group is 700..999, NOT 500..999 — the wider range overlapped the
    // "ploppy poop" splat at 550-554, so a dying player could taunt with a fart
    // (docs/re/id-audit.md).
    if (audio_.chance(s.tuning.taunt_chance)) pending_.push_back({s.tick + 25, 700});
}

void SoundDirector::on_tick(const sim::State& s) {
    drain_pending(s);

    for (const auto& ev : s.events) {
        switch (ev.type) {
            case sim::Event::Type::BombPlaced: on_bomb_placed(s, ev); break;
            case sim::Event::Type::BombKicked: audio_.play(120); break;
            case sim::Event::Type::Explosion: audio_.play(200); break;
            // DELIBERATELY no case for clock-zero: the tie/draw voice belongs to
            // the DRAW result screen (sub_427BFB(1700)), not to the tick where
            // ticks_left hits 0 — sudden death continues past that.
            case sim::Event::Type::Hurry:
                // sub_42A191 ~0x42A2C4 latches dword_464984 and fires this once,
                // just before the "hurry" banner.
                audio_.play(2700);
                break;
            case sim::Event::Type::WallClosed:
                // sub_426818 draws `dword_462244 = rand() % 3` ONCE when the
                // enclosure arms and replays 140+that for every tile that drops
                // — not a fresh pick per drop. Latch it for the round
                // (docs/re/facts.md "Wall-slam SFX").
                if (wall_slam_id_ < 0) wall_slam_id_ = 140 + audio_.roll(3);
                audio_.play_exact(wall_slam_id_, s.tick);
                break;
            case sim::Event::Type::BombPunched:
                // The event fires on every press so the punch pose plays, but
                // sub_427961(150) sits inside sub_424A50's `if (bomb ahead)`:
                // only an actual launch is audible. ev.data is that hit flag.
                if (ev.data) audio_.play(150);
                break;
            case sim::Event::Type::BombBounced: audio_.play(160); break;
            case sim::Event::Type::BombStopped: audio_.play(130); break;   // "bombstop"
            case sim::Event::Type::DropRefused:
                // Drop refused on a warphole (sub_41F29B ~23354). NOT gated to a
                // local player — sub_427961 is a global SFX, so an AI's warphole
                // drop buzzes too; do not add an `ev.player == 0` check.
                audio_.play(40);
                break;
            case sim::Event::Type::JellyBounced:
                // The ONLY debounced sound in the binary: the jelly reversal
                // goes through sub_427ABB(135), so a bomb pinballing between two
                // walls cannot re-trigger it more than once every 3 frames. Its
                // BombStopped sibling is NOT debounced — a bomb stops once.
                audio_.play_debounced(135, s.tick);
                break;
            case sim::Event::Type::BombGrabbed:
                // sub_424AF4 fires sub_427961(170). The group is SIX members,
                // not {grab1, grab2}: SOUNDLST loads 170..175 with no gap, so
                // the four "bmbthrw" clips are in it. facts.md's earlier "the
                // bmbthrw ids are dead assets" was wrong — nothing CALLS 172,
                // but 170's group walk reaches it (docs/re/sound-engine.md).
                audio_.play(170);
                break;
            // THE TWO SILENT BOMB EVENTS — silent for different reasons, and
            // both proven rather than assumed. BombThrown: the release path
            // sub_424987 makes no play call, so only the in-flight 160 is heard.
            // HeadHit: 360-363 is a loaded group NOTHING asks for — an
            // exhaustive scan of all 87 direct calls into the play primitives
            // names it nowhere, and no indirect path exists. Do not re-add a cue
            // here; tests/audio/test_sound_director.cpp pins both silences, and
            // docs/re/sound-engine.md §7-§8 carries the census.
            case sim::Event::Type::BombThrown:
            case sim::Event::Type::HeadHit: break;
            // Stage actors (docs/re/stage-actors.md §7). Group bases like every
            // other call site: trampoline picks across 350..353, warp 1330..1332.
            case sim::Event::Type::TrampolineBounce: audio_.play(350); break;
            case sim::Event::Type::WarpUsed: audio_.play(1330); break;
            case sim::Event::Type::Infected:
                // sub_41DFB6: 1-in-3 the per-disease line, else generic "oh no".
                audio_.play(audio_.chance(3) ? 3000 + 50 * ev.data : 2300);
                break;
            case sim::Event::Type::PowerupPicked: on_powerup_picked(ev); break;
            case sim::Event::Type::PlayerDied: on_player_died(s, ev); break;
            default: break;
        }
    }
}

}  // namespace bomber::game
