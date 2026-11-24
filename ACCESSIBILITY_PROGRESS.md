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

See the "Feature coverage" section of `doc/accessibility/FEATURES.txt` for the
per-feature status (implemented / compiled / automatically tested / manually
tested). Summary of milestones:

| Milestone | State |
|---|---|
| Baseline Windows build of unmodified 1.18.8 (local MinGW docker) | done, compiled |
| CI workflow (Windows package, Windows smoke test, Linux tests) | written, runs on GitHub Actions |
| Speech layer (Universal Speech, worker thread, self test) | implemented, compiled; unit tested on Linux (log backend); Windows self test runs in CI |
| GUI2 generic accessibility | implemented, compiled; end-to-end tests on Linux |
| Map review cursor and commands | implemented, compiled; end-to-end tests on Linux |
| Event announcements (moves, combat, recruit, advance, turns) | implemented, compiled; partly covered by tests |
| Packaging (overlay zip, scripts, docs) | implemented |
| Real JAWS/NVDA testing | NOT DONE — needs the user (see TESTING-CHECKLIST.txt) |

## Build/test log

- Local MinGW baseline build of unmodified source: success (wesnoth.exe PE32+ x86-64).
- Local MinGW build with accessibility layer: success, no new warnings.
- Overlay package built locally: 43.6 MB zip, 70 runtime DLLs, all imports verified.
- Found and fixed: missing `libssp-0.dll` in the package (would have prevented start).
- Linux CI build failed initially because of the debug Boost in the upstream
  image; fixed by removing it like upstream's cmake job does.

## Known problems / next steps

Kept up to date in `doc/accessibility/KNOWN-LIMITATIONS.txt`. Next steps are
listed at the end of that file.

## Conventions

- Never change game rules, RNG use, AI, or visibility. Hooks only read state
  after the engine has computed it.
- Every new engine hook is commented "Wesnoth Access".
- Commit messages end with the session attribution lines.
