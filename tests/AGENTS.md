# tests/AGENTS.md

Two doctest binaries: `build/nocturne_tests` (engine, `tests/*_test.cpp` and `*_test.mm`) and
`build/src/ui/nocturne_ui_tests` (`tests/ui/`). Filter with `--test-case="*words*"`, list with `--list-test-cases`.

## Adding tests

- A new engine test file must be added to `add_executable(nocturne_tests ...)` in the root `CMakeLists.txt`; a UI one
  to `nocturne_ui_tests` in `src/ui/CMakeLists.txt`. CI picks shards by file name, so a file that is not compiled
  runs nothing there and still looks green.
- Case names are sentences about behaviour. UI cases start with their area (`panels: `, `cues: `, `midi: `).
  ```cpp
  TEST_CASE("a broken user ISF file reports its error and the engine keeps rendering")   // tests/isf_user_test.cpp
  ```
- Drive the engine through `azl::EngineHarness` (`src/harness/engine_harness.hpp`): it runs in-process on an injected
  clock. `step(frames)` waits for every decode and compile, so pictures depend on the clock only; `runLive` and
  `holdCompiles` exercise the real render thread and a slow compile. Talk to it as a client does, with `Client` and
  `TempDir` from `tests/test_client.hpp`.
  ```cpp
  TempDir root;
  azl::EngineHarness harness({.width = 160, .height = 90}, 60.0, root.path);
  Client client(harness.document());
  harness.step(30);
  checkGolden(harness.readScreen(), "userIsfTint");   // tests/golden.hpp
  ```
- Test clips come from `tests/media/make_media.sh`; picture helpers are in `tests/clips.hpp`.
- UI tests can save the window as evidence: see `shoot()` in `tests/ui/midi_test.cpp`, which writes PNGs to
  `build/ui-shots/` (`NOCTURNE_UI_SHOTS`). Look at them before you claim a UI change works.
- Nothing may steal focus: engines run `--offscreen`, the UI `--hidden`. The test mains set
  `NOCTURNE_NO_AUDIO_INPUT=1`; `NOCTURNE_REAL_AUDIO=1` runs the cases that need a real input.

## Goldens (`tests/golden/`)

- Regenerate only when the new picture is the intended one, and only the cases you mean:
  `NOCTURNE_UPDATE_GOLDEN=1 build/nocturne_tests --test-case="<your case>"`.
- Open every changed PNG and look at it. Say in the PR which goldens changed and why. Never update one to make a
  failing test pass.

## CI and real-time cases

- `tools/ci-tests.sh` skips the wildcard patterns in `.github/realtime-tests.txt` (real GPU, displays, encoder,
  real-time scheduling, supervisor-started engines). They still run in local `ctest`. A new entry needs a reviewer's
  OK, and a new case name must not match an existing pattern by accident.
- On Windows CI also skips `.github/windows-ci-tests.txt` (cases that need the Credential Manager, which a runner
  session lacks); they run in a Windows desktop session. Same rule for a new entry.
- A failed case is rerun once and reported as flaky if it then passes. A flaky case is a bug to report, not noise.
- Cases that print GPU or compile timings are reports, not gates; do not "fix" them by changing thresholds.
