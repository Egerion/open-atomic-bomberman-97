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

# The match-frame verdict. A recapture run skips it (and skips the FATAL below)
# but must NOT return here -- the front-end `.BM` rows further down have to be
# recaptured in the same pass, or one invocation would silently refresh half the
# manifest.
if(NOT BOMBER_RECAPTURE)
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
endif()

# ---------------------------------------------------------------------------
# FRONT-END pins: bm_shots.txt, one `--bm-shot` capture per row.
#
# The five rows above all come from ONE scripted match, which is why they share
# a run. A `.BM` text screen has no match behind it at all -- it is one frame of
# the sub_41302D viewer at a given scroll position -- so each row is its own
# invocation. Same hermetic guarantee: --bm-shot is a capture_run(), so
# options.ini's soft_scaling / show_fps / vsync / native_cadence are pinned
# rather than read (game_app.cpp load_config), and HD artwork is off at boot
# and only ever turned on by a keypress.
#
# Row format: `<label> <BM_NAME> <scroll_lines> <sha256>`.
set(bm_file "${BOMBER_SHOTS_DIR}/bm_shots.txt")
# A missing manifest is a broken checkout, not a smaller test. This used to be a
# silent `return()`, which is the "green having verified nothing" shape this repo
# has now been bitten by six times -- an independent read of this script found
# three such paths (this one, an all-comment manifest leaving the count at zero,
# and a mid-loop skip that discarded mismatches already found). All three are
# closed below the same way: nothing exits this file successfully without either
# comparing every pinned frame or printing the SKIP marker ctest looks for.
if(NOT EXISTS "${bm_file}")
  message(FATAL_ERROR "missing ${bm_file}")
endif()

file(STRINGS "${bm_file}" bm_lines)
set(bm_mismatches "")
set(bm_count 0)
set(bm_rows 0)
set(bm_skipped FALSE)
foreach(line IN LISTS bm_lines)
  string(STRIP "${line}" line)
  if(line STREQUAL "" OR line MATCHES "^#")
    continue()
  endif()
  separate_arguments(fields UNIX_COMMAND "${line}")
  list(LENGTH fields nfields)
  if(NOT nfields EQUAL 4)
    message(FATAL_ERROR "${bm_file}: malformed row (want 'label name scroll sha256'): ${line}")
  endif()
  list(GET fields 0 label)
  list(GET fields 1 bm_name)
  list(GET fields 2 scroll)
  list(GET fields 3 expect_hash)
  math(EXPR bm_rows "${bm_rows} + 1")
  set(bmp "${BOMBER_WORK_DIR}/${label}.bmp")
  execute_process(
    COMMAND "${BOMBER_GAME_EXE}" --bm-shot "${bm_name}" "${bmp}" "${scroll}"
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE bm_stdout
    ERROR_VARIABLE bm_stderr
  )
  if(NOT rc EQUAL 0)
    if(rc EQUAL 2)
      # No install: stop capturing, but do NOT jump the verdict -- mismatches
      # already collected this run must still fail below.
      message(STATUS "VISUAL_GOLDEN_SKIP: no install for --bm-shot ${bm_name}")
      set(bm_skipped TRUE)
      break()
    endif()
    message(FATAL_ERROR "bomber_game --bm-shot ${bm_name} exited ${rc}: ${bm_stderr}")
  endif()
  if(NOT EXISTS "${bmp}")
    list(APPEND bm_mismatches "${label}: MISSING -- expected ${bmp}")
    continue()
  endif()
  file(SHA256 "${bmp}" actual_hash)
  math(EXPR bm_count "${bm_count} + 1")
  if(BOMBER_RECAPTURE)
    message(STATUS "RECAPTURE-BM ${label} ${bm_name} ${scroll} ${actual_hash}")
  elseif(NOT actual_hash STREQUAL expect_hash)
    list(APPEND bm_mismatches
         "${label} (${bm_name} +${scroll}): expected ${expect_hash}, got ${actual_hash}")
  endif()
endforeach()

if(bm_rows EQUAL 0)
  message(FATAL_ERROR "${bm_file}: no .BM rows defined -- an all-comment "
                      "manifest would otherwise pass having compared nothing")
endif()
if(bm_mismatches)
  string(REPLACE ";" "\n  " bm_str "${bm_mismatches}")
  message(FATAL_ERROR
    "Visual golden mismatch on a front-end .BM screen -- recapture and update "
    "${bm_file} in the same commit if the change is deliberate.\n  ${bm_str}")
endif()
if(NOT BOMBER_RECAPTURE AND NOT bm_skipped)
  message(STATUS "Visual golden: all ${bm_count} pinned .BM frames match")
endif()

# ---------------------------------------------------------------------------
# MENU pin: menu_shot.txt, one `--menu-shot` capture per row.
#
# This is the ONLY capture path that renders through FontTextures::draw_outlined
# (the sub_41696C outliner) -- the in-match frames cannot reach it even in
# principle (libs/render never includes libs/ui), and the .BM rows draw their
# text through the no-outline blit. Until this section existed, --menu-shot had
# no manifest at all, which is how two contradicting ports of sub_41696C
# coexisted for weeks under a green harness. Same hermetic terms as the rows
# above: --menu-shot is a capture_run().
#
# Row format: `<label> <sha256>`.
set(menu_file "${BOMBER_SHOTS_DIR}/menu_shot.txt")
if(NOT EXISTS "${menu_file}")
  message(FATAL_ERROR "missing ${menu_file}")
endif()

file(STRINGS "${menu_file}" menu_lines)
set(menu_mismatches "")
set(menu_count 0)
set(menu_rows 0)
set(menu_skipped FALSE)
foreach(line IN LISTS menu_lines)
  string(STRIP "${line}" line)
  if(line STREQUAL "" OR line MATCHES "^#")
    continue()
  endif()
  separate_arguments(fields UNIX_COMMAND "${line}")
  list(LENGTH fields nfields)
  if(NOT nfields EQUAL 2)
    message(FATAL_ERROR "${menu_file}: malformed row (want 'label sha256'): ${line}")
  endif()
  list(GET fields 0 label)
  list(GET fields 1 expect_hash)
  math(EXPR menu_rows "${menu_rows} + 1")
  set(bmp "${BOMBER_WORK_DIR}/${label}.bmp")
  execute_process(
    COMMAND "${BOMBER_GAME_EXE}" --menu-shot "${bmp}"
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE menu_stdout
    ERROR_VARIABLE menu_stderr
  )
  if(NOT rc EQUAL 0)
    if(rc EQUAL 2)
      message(STATUS "VISUAL_GOLDEN_SKIP: no install for --menu-shot")
      set(menu_skipped TRUE)
      break()
    endif()
    message(FATAL_ERROR "bomber_game --menu-shot exited ${rc}: ${menu_stderr}")
  endif()
  if(NOT EXISTS "${bmp}")
    list(APPEND menu_mismatches "${label}: MISSING -- expected ${bmp}")
    continue()
  endif()
  file(SHA256 "${bmp}" actual_hash)
  math(EXPR menu_count "${menu_count} + 1")
  if(BOMBER_RECAPTURE)
    message(STATUS "RECAPTURE-MENU ${label} ${actual_hash}")
  elseif(NOT actual_hash STREQUAL expect_hash)
    list(APPEND menu_mismatches "${label}: expected ${expect_hash}, got ${actual_hash}")
  endif()
endforeach()

if(menu_rows EQUAL 0)
  message(FATAL_ERROR "${menu_file}: no rows defined")
endif()
if(menu_mismatches)
  string(REPLACE ";" "\n  " menu_str "${menu_mismatches}")
  message(FATAL_ERROR
    "Visual golden mismatch on the menu frame -- this is the one pin through "
    "the sub_41696C outliner; recapture and update ${menu_file} in the same "
    "commit if the change is deliberate.\n  ${menu_str}")
endif()

if(BOMBER_RECAPTURE)
  message(STATUS "Recapture complete -- paste the RECAPTURE lines into "
                 "${shots_file}, the RECAPTURE-BM lines into ${bm_file}, and "
                 "the RECAPTURE-MENU lines into ${menu_file}, after confirming "
                 "the rendered BMPs under ${BOMBER_WORK_DIR} look right.")
  return()
endif()
if(NOT menu_skipped)
  message(STATUS "Visual golden: menu frame matches (${menu_count} pin)")
endif()
