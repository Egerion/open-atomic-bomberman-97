#include "bomber/game/sound_director.hpp"

namespace bomber::game {

void SoundDirector::reset() {
    pending_.clear();
    pickups_.fill(0);
}

void SoundDirector::on_tick(const sim::State& s) {
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (s.tick >= it->first) {
            audio_.play_random_in_range(it->second.first, it->second.second);
            it = pending_.erase(it);
        } else {
            ++it;
        }
    }

    for (const auto& ev : s.events) {
        switch (ev.type) {
            case sim::Event::Type::BombPlaced: audio_.play_one_of({100, 101}); break;
            case sim::Event::Type::BombKicked: audio_.play_random_in_range(120, 123); break;
            case sim::Event::Type::Explosion: audio_.play_random_in_range(200, 299); break;
            case sim::Event::Type::TimeUp: audio_.play_random_in_range(1700, 1999); break;
            case sim::Event::Type::WallClosed: audio_.play_one_of({140, 141, 142}); break;
            case sim::Event::Type::BombPunched: audio_.play_one_of({150, 151}); break;
            case sim::Event::Type::BombBounced: audio_.play(160); break;
            case sim::Event::Type::BombStopped: audio_.play(130); break;   // "bombstop"
            case sim::Event::Type::JellyBounced: audio_.play(135); break;  // "bombboun"
            case sim::Event::Type::BombGrabbed: audio_.play(170); break;
            case sim::Event::Type::BombThrown: audio_.play_one_of({150, 151}); break;
            case sim::Event::Type::HeadHit: audio_.play_random_in_range(360, 362); break;
            case sim::Event::Type::Infected: {
                // Skull voice: 1-in-3 the per-disease line (the 3000+50*idx
                // block), else the generic "oh no" (2300) — as sub_41DFB6.
                int base = 3000 + 50 * ev.data;
                if (audio_.chance(3)) audio_.play_random_in_range(base, base + 49);
                else audio_.play(2300);
                break;
            }
            case sim::Event::Type::PowerupPicked: {
                audio_.play_random_in_range(401, 499);
                int n = ++pickups_[ev.player];
                // "You are now AWESOME": 7th powerup, then every 3rd after.
                if (n == 7 || (n > 7 && (n - 7) % 3 == 0))
                    pending_.push_back({s.tick + 8, {1400, 1699}});
                break;
            }
            case sim::Event::Type::PlayerDied: {
                audio_.play_random_in_range(300, 309);
                // Post-death taunt from a survivor (VALUELST id 95: 1-in-N).
                if (audio_.chance(s.tuning.taunt_chance))
                    pending_.push_back({s.tick + 25, {500, 999}});
                break;
            }
            default: break;
        }
    }
}

}  // namespace bomber::game
