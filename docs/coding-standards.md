# Coding standards

The project's engineering standard. Adopted 2026-07-31 from a general C++20
guidelines document, **reconciled with this codebase's two hard contracts** —
the deterministic sim (`docs/adr/0003`) and the faithful-port rule of the
reverse-engineering workflow. Where a general rule and a contract disagree, the
contract wins and the disagreement is written down here rather than left for a
reader to discover by breaking something.

`CLAUDE.md` stays the short operational summary. This file is the long form.

## Objectives

Modern C++20, clean, SOLID where SOLID applies, testable, maintainable,
extensible, fast, readable, low coupling, high cohesion.

> Code is read far more often than it is written. Optimise for readability,
> maintainability and correctness before cleverness.

---

## 1. Language

**Use**: C++20 and the STL. RAII everywhere. `constexpr` wherever it is
possible. `enum class` over plain `enum`. `std::optional`, `std::variant`,
`std::span`, `std::string_view`, ranges, concepts, `std::filesystem`.

**Do not use**: raw `new`/`delete`, C-style arrays, C macros, C casts, `void*`,
globally mutable state.

Two notes on the general list:

- **`std::expected` is C++23** and is not available to us. Use `std::optional`
  for "maybe", `std::variant` for a closed set of outcomes, and exceptions where
  section 6 allows them.
- **Modules are not adopted.** The build spans MSVC, GCC and Clang across five
  presets and a FetchContent dependency graph; the cost is real and the benefit
  here is not.

---

## 2. Naming — the repo's convention, unchanged

This is the one section of the source document that is **deliberately not
adopted**, by the project owner's decision. Renaming the tree would touch every
file, reflow every diff, and buy nothing. The convention stays:

| kind | style | example |
|---|---|---|
| functions, methods, variables | `lower_snake` | `load_texture`, `player_health` |
| types, classes, structs | `PascalCase` | `Renderer`, `MatchConfig` |
| constants | `kCamelCase` | `kMaxPlayers` |
| members | trailing underscore | `sim_`, `win_count_` |
| enums | `enum class`, `PascalCase` values | `enum class WeaponType` |
| namespaces | `lower_snake`, one root | `bomber::sim`, `bomber::net` |

**No `I` prefix on interfaces.** An abstract base is named for what it is
(`Transport`, not `ITransport`); the abstractness belongs in the header, not in
the identifier. Mixing an `I`-prefixed type into a `lower_snake` codebase would
be worse than either convention alone.

Names still have to be *descriptive* — that half of the rule fully applies.
`load_texture()`, `save_player()`, `calculate_damage()`; never `do_stuff()`,
`handle()`, `process()`.

---

## 3. Size and shape

| | target | hard ceiling |
|---|---|---|
| function length | 5–20 lines | 40 |
| parameters | 3 | 4 |
| class length | 200 lines | — |
| nesting depth | 3 | — |
| inheritance depth | 1–2 | 3 |
| cognitive complexity | 25 | gated, see below |

Past 3 parameters, pass a **parameter object** instead of a longer list —
`create_enemy(config)`, not `create_enemy(x, y, z, hp, speed, armor, name)`.
This codebase already has the pattern (`MatchConfig`, `TurnContext`,
`ScreenContext`); reach for it rather than growing a signature.

Guard clauses over nesting. No `else` after a `return`/`continue`/`break`.

**Complexity is gated in the pre-push hook** (`scripts/complexity.sh`), on
clang-tidy's *cognitive* complexity rather than McCabe cyclomatic — it charges
nesting and charges nothing for a flat `switch`, which is the right bias for a
codebase full of ported dispatch tables. It is a **ratchet**: the 66 functions
already over the threshold are baselined and may stay, none may get worse, and
new code meets the threshold from its first line. See `CLAUDE.md`.

---

## 4. Object orientation, and where it stops

Encapsulation, abstraction, polymorphism, **composition over inheritance**.
Deep inheritance trees are a defect; three levels is the ceiling and two is
already suspicious.

### Where OO applies, and where it must not

This is the most important amendment in this document.

**OO and interfaces live at the BOUNDARIES** — rendering, audio, input, files,
transports. That is where a virtual call costs nothing and buys substitutability
we actually use: `Transport` has four implementations and the session above it
is identical whichever one it gets.

**`libs/sim` is not an object-oriented subsystem and must not become one.**
`State` is a plain aggregate: hashable, copyable, no virtuals, no heap-owning
members beyond `std::vector`. Three reasons, all binding:

1. `state_hash()` must mix every gameplay field, and a snapshot must be
   copyable by value — rollback re-simulates from copies every frame.
2. Virtual dispatch in the 20 Hz × 9 sub-frame inner loop is a measured cost
   with nothing to show for it; there is exactly one implementation of each
   system.
3. ADR-0003 makes the tick step order part of the contract. An extension point
   that lets a subclass reorder or intercept a step is a determinism bug wearing
   a design pattern.

So: "always depend on an abstraction, never on a concrete type" is a **boundary
rule here, not a universal one**. Systems take `State&` and each other by
reference in the constructor — cheap stack objects, explicit dependencies, no
globals, no singletons, and no interfaces where there is one implementation and
a contract forbidding a second.

---

## 5. SOLID, as it applies here

- **Single responsibility** — fully adopted, and it is the axis this codebase
  most often fails. A class doing several unrelated things gets split.
- **Open/closed** — adopted at the boundaries (see §4). Inside the sim, the
  extension mechanism is a new system class wired into `simulation.cpp`'s tick
  order, not a virtual hook.
- **Liskov** — adopted as written.
- **Interface segregation** — adopted. Many small interfaces over one large one.
- **Dependency inversion** — adopted **at the boundaries**. Constructor
  injection first; never a service locator. Inside the sim, see §4.

---

## 6. Ownership, memory, errors

**Prefer value semantics.** This is a sharpening of "always prefer
`std::unique_ptr`", not a rejection of it: RAII is the goal, and a stack value
achieves it with less indirection than a heap allocation. Use `std::unique_ptr`
when ownership is genuinely dynamic or polymorphic, `std::shared_ptr` only for
genuinely shared ownership (the repo currently has none, and that is a good
sign). Never `delete` in application code.

**Errors**: never ignore one, never signal one with a bare error code that a
caller can silently drop.

- `libs/assets` **throws** (`std::runtime_error` / `std::out_of_range`). Its
  input is 1997 files from an untrusted disk; bounds-check everything through
  `BinaryReader`.
- `libs/sim` **does not throw on the tick path.** An exception mid-tick leaves
  a half-advanced state that no longer hashes to anything either peer expects.
  Validate at construction; make invalid states unrepresentable.
- `libs/net` is I/O-confined: a transport reports failure as a status the
  session must handle, because "the socket died" is an ordinary outcome, not an
  exceptional one.

---

## 7. Const correctness, references, headers

Everything that can be `const` is `const` — methods, locals, references,
parameters. Pass `const T&` rather than copying; move only when ownership
transfers.

Headers declare; `.cpp` files implement. Implementation belongs in a header only
for templates, `constexpr`, and deliberately `inline` header-only components
(`libs/core`, `libs/match`, `libs/platform` are header-only by design and say
so). Prefer forward declarations. Every header includes exactly what it needs —
and `libs/core/include/bomber/core/cast.hpp` once used `std::size_t` through a
transitive include, which is the failure this rule exists to prevent.

---

## 8. Design patterns

**Use a pattern only when it solves a problem you actually have.** An
unnecessary pattern is a defect: it adds a layer a reader must decode before
reaching the logic. The audit register that scores this codebase counts
"appropriate use of design patterns" as *including not using them*.

Patterns that earn their place here: **factory** (object creation from parsed
config), **strategy** (see the caveat below), **state** (screen and lobby flow),
**observer** (the sim's per-tick `Event` list is exactly this), **command**
(editor undo/redo, input), **composite** (UI trees), **adapter** (third-party
libraries — SDL, IXWebSocket), **facade** (subsystem entry points), **flyweight**
(shared sprite/texture data), **dependency injection** by constructor.

### The one dangerous entry: "strategy replaces large switch statements"

**Not when the switch is a faithful port.** The RE workflow requires that a
ported mechanic *mirror the original's arithmetic rather than paraphrase it*, so
a long `switch` that reproduces `sub_XXXX`'s dispatch stays a `switch`, cites the
address, and is a documented exception rather than a finding. Replacing it with
a strategy hierarchy destroys the property that makes the port checkable against
the binary.

Apply strategy to switches *we* invented. Leave the ones the 1997 binary
invented alone.

---

## 9. Architecture, coupling, cohesion

The layering already exists and is drawn in `CLAUDE.md`'s dependency graph:
apps → game → match → assets, with sim at the bottom and `libs/core` beneath
everything. It maps onto the general Application → Engine → Subsystems →
Platform shape; do not re-layer it, extend it.

Dependencies point one way only. Never reach into another component's internals
— that is what the public headers under `include/bomber/<name>/` are for.
`libs/net` must never leak a socket into `libs/sim`; the sim only ever sees a
fully-assembled `TickInputs`.

High cohesion: a class holds closely related functionality. `libs/audio`'s split
into an SDL-free selection engine and an SDL playback engine is the model — the
seam fell where the dependency did.

---

## 10. Comments

Explain **why**, the constraints, and the algorithm. Never restate the code.

This codebase holds comments to a higher standard than the general rule, because
here they carry the reverse-engineering citations, the measured evidence and the
retracted conclusions — **they are the specification**. A comment that lies is
worse than no comment: it is indistinguishable in tone from the ones that are
right, which teaches a reader to trust none of them. Six such comments were
found and fixed in a single pass on 2026-07-31.

"Prefer expressive names over comments" applies to the *what*. It does not
license deleting a *why*.

---

## 11. Logging

No `std::cout` / `printf` inside library code. Console output belongs behind a
logging seam so it can be routed, silenced, or captured.

Two honest exceptions:

- **`apps/abtool` is a CLI whose product IS its stdout.** Printing there is the
  feature.
- **A diagnostic file written from a destructor** (`netdiag.log`) is a
  deliberate design: it survives any exit path, which is the whole point. It is
  not ad-hoc console noise.

Everything else is a finding. As of 2026-07-31 there are 76 such sites, 74 of
them in `libs/game`.

---

## 12. Testing

**doctest**, one suite per executable under `tests/`, registered in ctest. The
source document names GoogleTest and Catch2; doctest is what is wired in, and
Arrange / Act / Assert is the structure regardless of framework.

Every business rule should be testable, and a test that cannot fail is not a
test: **prove a new suite discriminates** by breaking the rule it covers,
confirming the *right* case goes red, and restoring it. That practice has caught
three separate "green having verified nothing" gates in this repo.

Sim behaviour is additionally pinned by golden hashes and by `build_hash`; see
the determinism contract in `CLAUDE.md`.

---

## 13. Performance

Measure before optimising; avoid premature optimisation. Prefer move semantics,
`emplace_back`, `reserve()`, `string_view`, `span`, `constexpr`, ranges.

One project-specific rule: **`libs/sim` is integer-only.** No floats, no wall
clock, no I/O — not as an optimisation but as a determinism requirement.

---

## 14. Threading

Where threading exists, prefer `std::jthread`, `std::mutex`, `std::scoped_lock`,
`std::atomic`; never manage a thread's lifetime by hand.

**The sim is single-threaded by contract and stays that way.** Parallelising the
tick would make the RNG draw order — which is part of the determinism contract —
depend on the scheduler.

---

## 15. Tooling

`clang-format` (repo `.clang-format`), `clang-tidy` (repo `.clang-tidy`),
warnings as errors under MSVC `/W4` and GCC/Clang `-Wall -Wextra`. All enforced
by the pre-push hook alongside the complexity ratchet.

**Sanitizers are the one tool the source document names that we do not yet
have.** AddressSanitizer and UndefinedBehaviorSanitizer over the headless
suite would be a genuine addition, particularly for the asset parsers, which
read untrusted 1997 files. Tracked as an open item, not yet done.

---

## Review checklist

- [ ] Single responsibility respected; SOLID respected where §4/§5 say it applies
- [ ] No duplicated code
- [ ] No long methods, no long classes, ≤3 parameters
- [ ] Complexity gate green, and nothing new baselined
- [ ] Naming follows §2
- [ ] RAII; no raw ownership; no leaks
- [ ] No unnecessary inheritance; every pattern justified
- [ ] Const correctness
- [ ] Comments explain why, and every one of them is TRUE
- [ ] Tests added, and proven to discriminate
- [ ] `clang-format` and `clang-tidy` clean
- [ ] For sim changes: golden hashes, `state_hash()` coverage, `build_hash` moved
