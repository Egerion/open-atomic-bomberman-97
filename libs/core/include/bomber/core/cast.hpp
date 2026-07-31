#pragma once

#include <cstddef>  // std::size_t (idx below) — this header depends on NOTHING else
#include <type_traits>

// Scoped-enum conversion helpers (ADR-0008): a C++20 shim for C++23's
// std::to_underlying, plus a size_t index for container lookups.
//
// The `requires` clause is load-bearing, not decoration. Unconstrained, E
// appears in the RETURN type, so a non-enum argument fails deep inside
// <type_traits> with the call site nowhere in the message — a worse diagnostic
// than the static_cast this replaces.
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
