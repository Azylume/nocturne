# Contributing to Nocturne

Thanks for helping. Nocturne is AGPL-3.0-or-later (see `LICENSE` and `docs/adr/0012-agpl-licence.md`), and it stays that
way: there is no contributor licence agreement, you keep the copyright of what you write, and nobody can later take
the code closed.

## Where to start

- **Small, well-scoped tasks:** [`good first issue`](https://github.com/Azylume/nocturne/issues?q=is%3Aissue+is%3Aopen+label%3A%22good+first+issue%22).
  Effects, generators, ISF shaders, presets and docs need little context.
- **Bigger features:** [`help wanted`](https://github.com/Azylume/nocturne/issues?q=is%3Aissue+is%3Aopen+label%3A%22help+wanted%22).
- **Where the code lives:** the "For developers" section of the [README](README.md#for-developers) has the architecture
  diagram. In short: `src/engine/` renders (headless, Metal), `src/effects/` holds the effect registry and shaders,
  `src/document/` the composition and its undoable edits, `src/protocol/` and `src/net/` the WebSocket protocol,
  `src/ui/` the JUCE app, `src/app/` the executables (`nocturne`, `nocturne-supervisor`, `nocturne-ctl`), `tests/`
  the engine and UI suites. `CONTEXT.md` defines the domain words; `docs/reference.md` documents every feature.

Comment on an issue before you start, so two people don't build the same thing.

## Open an issue before big work

For anything larger than a bug fix or a self-contained addition (for example MIDI or OSC control, a release
pipeline, a protocol change, a new output or media path), open an issue or comment on the existing one first and
agree on the approach. Nocturne runs itself with Autopilot and Cues; features that add manual control stay optional
and must cost nothing when unused.

## Sign off every commit (DCO)

Every commit carries a `Signed-off-by:` line, which certifies the [Developer Certificate of Origin](https://developercertificate.org/):
that you wrote the change or otherwise have the right to submit it under the project's licence.

```sh
git commit -s -m "Clips keep their speed after a relink"
```

The name and email in the line must match the commit author. A pull request with an unsigned commit fails the DCO
check; fix it with `git rebase --signoff main` and force-push your branch.

## Build and test

macOS 14 or later, Xcode command line tools, CMake 3.24+ and Homebrew `ffmpeg` (for the media tools and test clips).

```sh
tools/make-signing-identity.sh # once per machine: the local code-signing identity
cmake -S . -B build
cmake --build build -j8
(cd build && ctest --output-on-failure)
```

The suite renders on the GPU and needs a logged-in session; some tests are GPU timing reports, not pass/fail checks.
CI runs everything except the real-time and hardware tests listed in `.github/realtime-tests.txt`; run the full
suite locally before a pull request that touches rendering, outputs, recording or the supervisor.

## What a good change looks like

- One topic per pull request, with a test that fails without it. Render changes compare against the goldens in
  `tests/golden/`; regenerate a golden with `NOCTURNE_UPDATE_GOLDEN=1` only when the new picture is the intended one,
  and say why in the pull request.
- Say in the pull request which Mac you built and tested on: the model and Apple silicon or Intel. CI builds
  arm64 only, and the reference show machine is a 2018 Intel Mac mini, so Intel results are especially welcome.
- Follow the code around it: naming, comment density, file layout. `CONTEXT.md` defines the domain words.
- Architecture decisions go in `docs/adr/`. Read the relevant ADR before changing what it decided.
- A new dependency must be AGPL-3.0-compatible: check `docs/licence-notes.md` and add a row to ADR 0012's table.
- Nothing copied from other VJ software or its files. Compatibility (the Resolume importer, the ISF loader) is built
  from documented formats and our own saved files.

## Reporting and questions

Bugs, ideas and questions: [GitHub issues](https://github.com/Azylume/nocturne/issues/new/choose); search the existing
ones first. Security problems: privately, see `SECURITY.md`. Conduct: `CODE_OF_CONDUCT.md`.

## Reviews

Nocturne is maintained by a working VJ, so reviews are best effort: usually within a few days, slower around gigs.
A small pull request with a test gets reviewed fastest. If a week passes without an answer, a friendly ping on the
pull request is welcome.
