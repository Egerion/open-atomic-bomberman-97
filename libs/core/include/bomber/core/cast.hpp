#pragma once

#include <cstddef>  // std::size_t (idx below) — this header depends on NOTHING else
#include <type_traits>

// Scoped-enum conversion helpers (ADR-0008 libs/core) — a C++20 shim for
// std::to_underlying (C++23) plus a size_t-typed index for container lookups.
// These replace scattered `static_cast<int>(some_enum)` / `static_cast<size_t>`
// at enum boundaries without weakening the scoped-enum type safety.
namespace bomber::core {

template <class E>
constexpr std::underlying_type_t<E> to_underlying(E e) noexcept {
    return static_cast<std::underlying_type_t<E>>(e);
}

// Index form for `array[idx(enum)]` — always std::size_t.
template <class E>
constexpr std::size_t idx(E e) noexcept {
    return static_cast<std::size_t>(static_cast<std::underlying_type_t<E>>(e));
}

}  // namespace bomber::core
