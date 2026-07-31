#pragma once

#include <cstddef>  // std::size_t (idx below) — this header depends on NOTHING else
#include <type_traits>

// Scoped-enum conversion helpers (ADR-0008 libs/core) — a C++20 shim for
// std::to_underlying (C++23) plus a size_t-typed index for container lookups.
// These replace scattered `static_cast<int>(some_enum)` / `static_cast<size_t>`
// at enum boundaries without weakening the scoped-enum type safety.
//
// Constrained on is_enum rather than left open: an unconstrained E puts
// std::underlying_type_t<E> in the RETURN type, so a non-enum argument fails
// deep inside <type_traits> with the call site nowhere in the message. A helper
// whose whole purpose is to make a cast boundary legible should not hand back a
// worse diagnostic than the static_cast it replaces.
namespace bomber::core {

template <class E>
    requires std::is_enum_v<E>
constexpr std::underlying_type_t<E> to_underlying(E e) noexcept {
    return static_cast<std::underlying_type_t<E>>(e);
}

// Index form for `array[idx(enum)]` — always std::size_t.
template <class E>
    requires std::is_enum_v<E>
constexpr std::size_t idx(E e) noexcept {
    return static_cast<std::size_t>(to_underlying(e));
}

}  // namespace bomber::core
