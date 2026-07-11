# Visual golden harness runner (tests/visual/README.md). Invoked by the
# "visual_golden" ctest (apps/game/CMakeLists.txt) via `cmake -P`, so it has
# no dependency on bash/PowerShell being present -- CMake itself is the only
# interpreter, matching every other preset in this repo.
#
# Required -D args:
#   BOMBER_GAME_EXE  -- path to the built bomber_game(.exe)
#   BOMBER_SHOTS_DIR -- tests/visual (finds shots.txt next to this script)
#   BOMBER_WORK_DIR  -- scratch directory for the captured BMPs
# Optional:
#   BOMBER_RECAPTURE -- if set, print fresh `label tick hash` rows instead
#                       of comparing (see README.md "Recapturing").

if(NOT DEFINED BOMBER_GAME_EXE OR NOT EXISTS "${BOMBER_GAME_EXE}")
  message(FATAL_ERROR "BOMBER_GAME_EXE not set or missing: ${BOMBER_GAME_EXE}")
endif()
if(NOT DEFINED BOMBER_SHOTS_DIR)
  message(FATAL_ERROR "BOMBER_SHOTS_DIR not set")
endif()
if(NOT DEFINED BOMBER_WORK_DIR)
  set(BOMBER_WORK_DIR "${CMAKE_CURRENT_BINARY_DIR}/visual_shots")
endif()

set(shots_file "${BOMBER_SHOTS_DIR}/shots.txt")
if(NOT EXISTS "${shots_file}")
  message(FATAL_ERROR "missing ${shots_file}")
endif()

file(STRINGS "${shots_file}" lines)
set(spec "")
set(labels "")
set(ticks "")
set(hashes "")
foreach(line IN LISTS lines)
  string(STRIP "${line}" line)
  if(line STREQUAL "" OR line MATCHES "^#")
    continue()
  endif()
  separate_arguments(fields UNIX_COMMAND "${line}")
  list(LENGTH fields nfields)
  if(NOT nfields EQUAL 3)
    message(FATAL_ERROR "${shots_file}: malformed row (want 'label tick sha256'): ${line}")
  endif()
  list(GET fields 0 label)
  list(GET fields 1 tick)
  list(GET fields 2 expect_hash)
  list(APPEND labels "${label}")
  list(APPEND ticks "${tick}")
  list(APPEND hashes "${expect_hash}")
  if(spec STREQUAL "")
    set(spec "${label}:${tick}")
  else()
    set(spec "${spec},${label}:${tick}")
  endif()
endforeach()

list(LENGTH labels nshots)
if(nshots EQUAL 0)
  message(FATAL_ERROR "${shots_file}: no shots defined")
endif()

file(REMOVE_RECURSE "${BOMBER_WORK_DIR}")
file(MAKE_DIRECTORY "${BOMBER_WORK_DIR}")

# ONE scripted run captures every shot (run_demo, libs/game/src/game_app.cpp)
# -- deterministic per tests/visual/README.md's "Determinism guarantees", so
# repeated invocations of this exact command are byte-for-byte identical.
execute_process(
  COMMAND "${BOMBER_GAME_EXE}" --demo-shots "${spec}" "${BOMBER_WORK_DIR}"
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE demo_stdout
  ERROR_VARIABLE demo_stderr
)
if(NOT demo_stdout STREQUAL "")
  message(STATUS "${demo_stdout}")
endif()
if(NOT demo_stderr STREQUAL "")
  message(STATUS "${demo_stderr}")
endif()

if(NOT rc EQUAL 0)
  # GameApp::run() returns 2 specifically when no game_dir could be resolved
  # (opts_.game_dir stays empty -- BOMBER_GAME_DIR / gamedir.txt / the
  # standard install paths, libs/assets/src/install.cpp, all missed). That
  # is the expected "no install" case on CI; anything else is a real
  # failure of the harness itself (bad args, crash, SDL init failure, ...).
  if(rc EQUAL 2)
    message(STATUS "VISUAL_GOLDEN_SKIP: no BOMBER_GAME_DIR / gamedir.txt / standard install found")
    return()
  endif()
  message(FATAL_ERROR "bomber_game --demo-shots exited ${rc} (not the 'no install' code 2)")
endif()

set(mismatches "")
math(EXPR last "${nshots} - 1")
foreach(i RANGE ${last})
  list(GET labels ${i} label)
  list(GET ticks ${i} tick)
  list(GET hashes ${i} expect_hash)
  set(bmp "${BOMBER_WORK_DIR}/${label}.bmp")
  if(NOT EXISTS "${bmp}")
    list(APPEND mismatches "${label}: MISSING -- expected ${bmp}")
    continue()
  endif()
  file(SHA256 "${bmp}" actual_hash)
  if(BOMBER_RECAPTURE)
    message(STATUS "RECAPTURE ${label} ${tick} ${actual_hash}")
  elseif(NOT actual_hash STREQUAL expect_hash)
    list(APPEND mismatches "${label} (tick ${tick}): expected ${expect_hash}, got ${actual_hash}")
  endif()
endforeach()

if(BOMBER_RECAPTURE)
  message(STATUS "Recapture complete -- paste the RECAPTURE lines above into "
                 "${shots_file} (label/tick/hash columns), after confirming "
                 "the rendered BMPs under ${BOMBER_WORK_DIR} look right.")
  return()
endif()

if(mismatches)
  string(REPLACE ";" "\n  " mismatches_str "${mismatches}")
  message(FATAL_ERROR
    "Visual golden mismatch -- a renderer change altered pixels for a "
    "pinned frame. If this is a DELIBERATE visual change, recapture "
    "(README.md \"Recapturing\") and update ${shots_file} in the same "
    "commit; otherwise this is the presentation regression this harness "
    "exists to catch.\n  ${mismatches_str}")
endif()

message(STATUS "Visual golden: all ${nshots} pinned shots match (${BOMBER_WORK_DIR})")
