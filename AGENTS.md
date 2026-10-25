# AGENTS.md

Nocturne is an autopilot VJ engine for macOS: a headless C++20 engine that renders with Metal, a separate JUCE UI,
and one WebSocket protocol (port 7400) between them. Your job here: small, tested, reviewable changes that keep a
live show running at 60 fps on a 2018 Intel Mac mini.

Today Nocturne builds and runs on macOS only. A Windows port is planned ([#35](https://github.com/Azylume/nocturne/issues/35)):
keep new Metal or AppKit code behind the existing engine, media and output boundaries instead of spreading it into
portable code (protocol, UI logic, ISF parsing).

Humans start with [CONTRIBUTING.md](CONTRIBUTING.md); it wins where this file disagrees. Subtrees have their own
`AGENTS.md`: read [tests/AGENTS.md](tests/AGENTS.md), [src/engine/AGENTS.md](src/engine/AGENTS.md) or
[third_party/isf/AGENTS.md](third_party/isf/AGENTS.md) before you change files there.

## Commands

macOS 14+, Xcode command line tools, CMake 3.24+, Homebrew `ffmpeg`. CMake uses `ccache` by itself when it is
installed (`brew install ccache`); CI also uses `ninja`.

```sh
tools/make-signing-identity.sh                  # once per machine: the local "Nocturne Dev" signing identity
cmake -S . -B build                             # Universal (arm64 + x86_64) by default
cmake --build build -j8
build/nocturne_tests --test-case="*user ISF*"   # one engine case or a group (doctest wildcards)
build/src/ui/nocturne_ui_tests --test-case="*Cues*"  # UI suite, same filters
(cd build && ctest --output-on-failure)         # the full local suite, real-time cases included
tools/ci-tests.sh engine 0 3                    # what one CI shard runs (shards 0, 1, 2, then: tools/ci-tests.sh ui)
build/nocturne --offscreen                      # a smoke run that shows no window; build/nocturne-ctl watch talks to it
```

- Faster local loop: `cmake -S . -B build -G Ninja -DCMAKE_OSX_ARCHITECTURES=arm64` (what CI configures).
- Run the narrowest test first, then the shard or `ctest` before you call it done.
- The suite renders on the GPU and needs a logged-in macOS session. Some cases are GPU timing reports, not checks.
- Optional builds: `-DNOCTURNE_SANITIZE=ON` (ASan + UBSan), `-DNOCTURNE_TRACY=ON` (profiler), fuzzing per
  `fuzz/README.md` (a new parser gets a harness).
- **In a Linux or cloud sandbox nothing here builds or runs** (Metal, AppKit, AVFoundation). You can still edit code,
  docs, ISF `.fs` files and scripts. Say in the PR that it was **not built or tested**, and what a human should run.

## Map

| Path | What is there |
|---|---|
| `src/engine/` | Render thread, chain compiler, ISF compiler and user ISF library, services |
| `src/effects/` | Effect registry, chain planner (fusion), ISF parsing (`isf.cpp`), built-in presets |
| `src/document/` | The composition, its undoable edits, the JSON file format |
| `src/protocol/`, `src/net/` | Protocol messages, document server, pairing, remote file rules |
| `src/media/`, `src/output/` | Decoders, HAP, Media Library; displays, recording, return video |
| `src/ui/` | JUCE app (`Nocturne UI.app`), only a protocol client (`src/ui/client/`) |
| `src/app/` | Executables: `nocturne`, `nocturne-supervisor`, `nocturne-ctl`, `nocturne-desk`, tools |
| `tests/`, `tests/ui/`, `tests/golden/` | Engine suite, UI suite, reference images |
| `third_party/isf/` | Curated ISF shaders, vendored verbatim with their licences |
| `docs/reference.md`, `docs/adr/`, `CONTEXT.md` | Every feature and `nocturne-ctl` command; decisions; domain words |

## Boundaries

**Always**
- Work on a branch, one topic per PR, with a test that fails without the change.
- Commit with `git commit -s` (DCO; the check fails on any commit without it).
- Keep the render thread free of waiting (below). Hand work to a service; pick up finished results.
- Use the words in `CONTEXT.md`: Composition, Trigger, Effect, Engine, Director; not project, launch, filter,
  server, master.
- Keep manual control optional: Autopilot and Cues run the show, and a control feature costs nothing when unused.
- Update `docs/reference.md` for any user-visible, `nocturne-ctl` or protocol change. A new decision gets an ADR.
- Report exactly what you ran, on which Mac (model, Apple silicon or Intel), and what you did not run.

**Ask first** (stop and ask your human, or open an issue)
- Protocol message changes, a new output or media path, a new control surface, release or CI workflow changes.
- Adding or bumping a dependency (FetchContent pins, `third_party/`). New ones must be AGPL-compatible (ADR 0012).
- Anything that sends data off the machine: telemetry, update checks, crash upload. Users must see what leaves it.
- Touching the trust model in `SECURITY.md`: pairing token, `Origin` refusal, pre-pairing limits, path confinement,
  bundle signature checks.
- A new built-in C++/Metal effect. New effects should be ISF shaders (direction of issue #34).
- Adding a line to `.github/realtime-tests.txt` (needs a reviewer's OK), or changing what an ADR decided.
- Pushing a branch or opening a PR when your human did not ask for it; force-pushing even your own branch (use
  `--force-with-lease`).

**Never**
- Push to `main`, merge, or force-push someone else's branch.
- Skip, loosen or delete a test, or regenerate a golden, to make a failing check pass.
- Describe an unfixed vulnerability in an issue, PR or commit. Report it privately (Security tab, per `SECURITY.md`).
- Commit secrets, tokens, real host names or credentials (`.gitleaks.toml` lists the only allowed fake values).
- Copy code, shaders, assets, presets or parameter values from other VJ software or its files.
- Load or link closed native code into a Nocturne binary (ADR 0014), or link engine internals into the UI.
- Claim a test passed, a shader fuses or a number improved without the output that shows it.
- Commit build output, media under `examples/`, `.claude/` or `.scratch/`; reformat code you did not otherwise change.

## The render thread never waits

The v1 show machine is a 2018 Intel Mac mini with UHD 630 graphics (ADR 0008); its budget decides, not Apple
silicon's. On the render thread: no file or network I/O, no shader compile, no decode, no heap allocation per frame,
and a lock only for a short lookup, never held across work. One thread per output presents (ADR 0003).

```cpp
// Good: any thread queues the work and returns at once; the render thread only looks the result up.
compiler_.prepare(*document);                 // ChainCompiler, src/engine/chain_compiler.hpp
if (auto chain = compiler_.find(key)) { ... } // null until compiled: keep drawing what you had

// Bad: this frame waits for a compile (or a file read, a decode, a socket), and every output stalls with it.
auto chain = compileChainNow(effects);        // never on the render thread
```

Pointwise and remap stages fuse into one pass (ADR 0009); do not add a pass where fusion would do. For a render-path
change, measure before and after and put the numbers in the PR: `tools/bench-mini.sh` on the Mini (gate: render
>= 59.5 fps, <= 0.05 hitches/s per output; see `docs/show-machine.md`), or on your own Mac
`bench/run.sh BIN_DIR MEDIA_DIR OUT_DIR` (usage in its header), and say which. Give the number with its run count
and range; a gap smaller than the run-to-run spread is no difference.

## Code style

No formatter config: match the code around you. 4-space indent, `#pragma once`, `namespace azl`, `PascalCase` types,
`camelCase` functions, `member_` fields, `kConstant` constants, about 120 columns, opening brace of a function on its
own line. Comments are full sentences that say what the code guarantees: which thread, what it never does.

```cpp
// Compiles effect chains on its own thread. The render thread only looks finished chains up, so it never waits for a
// compile, even a slow or failing one.
class ChainCompiler {
public:
    // Null until the chain is compiled.
    std::shared_ptr<const CompiledChain> find(const std::string& key) const;
private:
    mutable std::mutex mutex_;
};
```

Not: comments that narrate the diff ("fixed the bug you mentioned") or restate the code.

Copy the nearest good example, not just any nearby code: `tests/isf_user_test.cpp` for an engine test through the
protocol, `tests/ui/cues_test.cpp` for a UI test, `src/engine/chain_compiler.hpp` for a thread contract.

## Commits and pull requests

```text
Clips keep their speed after a relink

A relinked clip took the new file's default speed; it now keeps its own.
The relink case in tests/prototype_gaps_test.cpp checks it.

Signed-off-by: Your Name <you@users.noreply.github.com>
```

- Subject: a short sentence of what is now true, optionally `Area: ` first (`ISF: ...`, `docs: ...`). No ticket-only
  subjects. The `Signed-off-by:` must match the commit author exactly; fix with `git commit --amend -s --no-edit` or
  `git rebase --signoff main`.
- PR body: what and why, how it was tested, which Mac. CI tests arm64 only. The person who opens the PR has read
  every line, run the tests and signed off, and answers for the change.
- Docs-only changes (`docs/**`, `**/*.md`) skip CI; the DCO check still runs.

## Rules and what checks them

Prose here is the weakest guard; a check that fails is stronger. Know which rules a machine catches:

| Rule | Checked by |
|---|---|
| Every commit signed off | DCO workflow (`.github/workflows/dco.yml`), every PR |
| Builds, engine and UI suites pass on arm64 | CI (`.github/workflows/ci.yml`); skipped for docs-only changes |
| Both slices in every binary | CI `universal` job, on `main` and nightly only, not on PRs |
| A curated ISF effect or generator has a test entry | "every built-in ISF shader loads" (`tests/isf_engine_test.cpp`) |
| An ISF effect's golden is not the plain card | the same file's golden test (2% of pixels must differ) |
| Test file added to CMake, no secrets, render thread never waits | **nothing in CI**: you and the reviewer |
| Fusion claims, licences, goldens looked at, doc commands run as written | **nothing in CI**: you and the reviewer |

When a reviewer corrects the same mistake twice, propose a check (a test, a CI step) that would have failed on it,
and add it to this table; a new line of prose is the last resort.

## Before you say done

1. `git diff --stat origin/main...`: only files on this topic; no stray reformatting, build output or media.
2. `cmake --build build` builds without new warnings in the files you touched.
3. The narrow test, then its shard or `(cd build && ctest --output-on-failure)`, pass. Paste the summary line.
4. Render change: goldens unchanged, or regenerated on purpose and opened and looked at (see `tests/AGENTS.md`).
   Render-path change: bench numbers, or a plain "not measured".
5. Every command you wrote into docs was run as written, from a clean shell.
6. `git log origin/main..HEAD --format='%h %an <%ae> | %(trailers:key=Signed-off-by,valueonly,separator=%x2C )'`
   shows a matching sign-off on every commit.
7. `docs/reference.md`, `CONTEXT.md` or an ADR updated if the change needs it; nothing claimed that was not run.

If a rule here or in an ADR blocks what you were asked to do, stop and say so instead of working around it.
