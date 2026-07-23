#pragma once

// Slot space + nominal tick rate — SDL-free scalars shared across the port
// (ADR-0008 libs/core). 10 player slots; 20 Hz nominal gameplay rate.
namespace bomber::core {

inline constexpr int kMaxPlayers = 10;
inline constexpr int kTicksPerSecond = 20;

}  // namespace bomber::core
