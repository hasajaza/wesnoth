# Wesnoth Access — development progress and continuation notes

This file lets any developer (or another Claude session) resume work on
Wesnoth Access. Keep it current with every milestone.

## Project facts

- Repository: https://github.com/hasajaza/wesnoth, branch `wesnoth-access`.
- Base: Battle for Wesnoth 1.18.8, commit `9f54f6a4f65b0d942a70436372e7cf28c4ba14d0`.
- Target: Windows 11 x64, JAWS and NVDA, totally blind player, single-player campaigns first.
- Accessibility version: see `src/accessibility/version.hpp` (`ACCESS_VERSION_STRING`).

## How to build (no local Windows toolchain needed)

Windows x64 builds are cross-compiled with the same docker image upstream uses
for official Windows releases (`wesnoth/wesnoth:mingw-master`, pinned by digest
in `.github/workflows/wesnoth-access.yml`).

- CI: push to `wesnoth-access` (or run the "Wesnoth Access" workflow manually)
  -> artifact `WesnothAccess-win64` contains the overlay zip
  (and the standalone zip when the `standalone` input is set).
  After the Windows smoke test and the Linux tests pass on `wesnoth-access`,
  job `publish-latest` uploads the overlay zip to the release `access-latest`
  under the fixed name `WesnothAccess-win64-overlay.zip` (permanent link:
  https://github.com/hasajaza/wesnoth/releases/download/access-latest/WesnothAccess-win64-overlay.zip).
  The tag is created once and never moved; only the asset and notes change.
- Locally in a Linux container with docker:
  - Windows: `docker run --rm -v $SRC:/wesnoth -v $OUT:/output wesnoth/wesnoth:mingw-master sh -c 'cd /output && scons -j4 arch=x86-64 prefix=/msys64/mingw64 gtkdir=/msys64/mingw64 host=x86_64-w64-mingw32 -Y /wesnoth build=release nls=false wesnoth'`
  - Linux (tests): image `wesnoth/wesnoth:2204-master`; first remove the debug
    Boost in /usr/local (`rm -rf /usr/local/lib/cmake /usr/local/lib/libboost* /usr/local/include/boost && ldconfig`),
    then `scons -Y /wesnoth build=release nls=false wesnoth boost_unit_tests`.
  - Speech DLLs: `packaging/access/build_speech_deps.sh <outdir>` inside the mingw image
    (needs internet access to nvaccess.org for the NVDA controller client).
  - Package: `packaging/access/make_package.py` inside the mingw image.
- Tests: `utils/access/run_access_tests.sh <linux build dir>` (inside the Linux image).

## Architecture (summary; details in doc/accessibility/ARCHITECTURE.txt)

`src/accessibility/`:
- `speech.*` — speech worker thread; Universal Speech (runtime-loaded
  `UniversalSpeech.dll`, JAWS/NVDA/SAPI), `log`/`stdout`/`none` backends,
  spd-say on Linux.
- `accessibility.*` — settings (preferences keys `access_*`), manager (RAII,
  created in `do_gameloop`), speak()/history/dialogue history, global keys,
  self test, settings dialog and command reference (reuse the existing
  `simple_item_selector` dialog, so no data files change).
- `keymap.*` — configurable bindings per context (global/dialog/map).
- `gui_access.*` — generic GUI2 layer, hooked in
  `gui2::event::sdl_event_handler::key_down` and `gui2::window::show`.
- `game_access.*` — map review cursor, info commands, lists, actions through
  `events::mouse_handler` (`access_hover` + `select_or_action`), event
  announcements via read-only hooks.
- `test_driver.*` — `--access-test-script` end-to-end test driver.

Engine files touched (all changes are marked with "Wesnoth Access" comments):
`wesnoth.cpp`, `commandline_options.*`, `controller_base.cpp`, `mouse_events.*`,
`play_controller.cpp`, `playsingle_controller.cpp`, `display.*`,
`display_chat_manager.cpp`, `actions/{attack,move,create,advancement}.cpp`,
`gui/core/event/handler.cpp`, `gui/widgets/window.cpp`,
`gui/widgets/text_box_base.hpp`, `source_lists/*`.

## Status

Per-feature status: `doc/accessibility/FEATURES.txt`. Limitations and next
steps: `doc/accessibility/KNOWN-LIMITATIONS.txt`.

| Milestone | State |
|---|---|
| Baseline Windows build of unmodified 1.18.8 (local MinGW docker) | done |
| CI: Windows package, Windows smoke + end-to-end tests, Linux tests | working (runs 3 and 4 green; test step exit codes fixed after run 4) |
| Speech (Universal Speech, worker thread) | done; Windows runner: library loads, SAPI5 speaks |
| Generic GUI2 dialogs | done; automated tests |
| Legacy help browser reader | done; automated test |
| Map review cursor, info commands, lists | done; automated tests on Linux and Windows |
| Actions (select, move, attack, recruit, advance, undo, end turn) | done; automated tests |
| Event announcements (combat, moves, heal, turns, enemy turn) | done; automated tests |
| Packaging (overlay zip, installer, docs) | done; installer tested on Windows runner |
| Real JAWS/NVDA testing | NOT DONE — needs the user (TESTING-CHECKLIST.txt) |

## Test results (latest local run)

- Unit tests (Boost, test_accessibility): 8/8 pass.
- End-to-end (utils/access/tests): map_basics 66, map_actions 29,
  menus 27, campaign_start 28 checks, all pass (Linux).
- Windows runner (CI run 4): map_basics 56/56 on the real Windows binary,
  speech self test PASS (SAPI5), installer OK.
- Upstream WML unit test suite: passes with accessibility disabled and
  enabled (CI run 4).

## Build/test log

- Local MinGW baseline build of unmodified source: success (wesnoth.exe PE32+ x86-64).
- Local MinGW build with accessibility layer: success, no new warnings.
- Overlay package: about 44 MB zip, 70 runtime DLLs, all imports verified.
- Fixed: missing `libssp-0.dll` in the package; debug Boost of the Linux CI
  image; test step exit code hidden by `tee`; unit tests needed the source
  tree as working directory.
- Bugs found by the end-to-end tests and fixed: previous-location aliasing,
  possible announcement of moves by invisible units, recruit dialog focus,
  repeated loading announcements, label hints with colons, F4 not reading
  all list items, silent undo.

## Known problems / next steps

Kept up to date in `doc/accessibility/KNOWN-LIMITATIONS.txt`. Next steps are
listed at the end of that file.

## Conventions

- Never change game rules, RNG use, AI, or visibility. Hooks only read state
  after the engine has computed it.
- Every new engine hook is commented "Wesnoth Access".
- Commit messages end with the session attribution lines.

## Field notes (tester feedback, November 2026)

- NVDA: Universal Speech's own NVDA detection reported "not running" on the
  tester's PC although `nvdaController_testIfRunning()` from the same
  packaged DLL returned 0. Wesnoth Access now loads the controller client
  itself and speaks through it directly in that case ("NVDA (direct)" in
  the log). Confirmed working by the tester with NVDA.
- Stray mouse clicks reach the map (seen in logs while pressing keys with a
  screen reader running); the map ignores the mouse by default
  (`access_map_mouse`). Before that, a click moved a selected unit.
- After a move the game drops the selection; F7 reselects the unit under
  the cursor.
