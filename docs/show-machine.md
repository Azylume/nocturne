# Running a show machine

Setting up a Mac mini as an unattended show machine, reaching it remotely, and the benchmark, profiling and resilience tools around it.

## Working rules for show builds

- **Commit every working step.** A step is done when it builds and its check passes; then commit before starting the next step. Never commit mid-edit, never leave working code uncommitted.
- **Tag known-good versions** with `tools/tag-good.sh <what>` after a milestone has been tried on real hardware, so there is always a version to go back to. It creates `good/<date>-<what>` only when the Mac mini benchmark passes (see below).
- **Experiments go on their own branch**; `main` only receives what worked.
- **Never rewrite history** that has been tagged or shared (no force push, no rebase of `main`).

## Mac mini show setup (one time)

The Mini plays from power-on with no keyboard. In its desktop session (Screen Sharing or a keyboard):

1. From the laptop: `tools/deploy-mini.sh` installs `~/Nocturne/Nocturne.app`, `~/Nocturne/tools` and `~/Nocturne/tests`.
2. Microphone, once: `~/Nocturne/Nocturne.app/Contents/MacOS/nocturne --mic-check` in Terminal (or `open -W ~/Nocturne/Nocturne.app --args --mic-check` over SSH) and click Allow. Later builds keep the grant. The engine is still its own responsible process when the supervisor starts it, so the permission carries over; check that the `audio` lines in `engine.log` show a live level and not `digital silence`.
3. Automatic login: System Settings > Users & Groups > Automatically log in as: this user (needs FileVault off; `fdesetup status`).
4. No sleep, and back on after a power cut or a freeze: `sudo pmset -a sleep 0 displaysleep 0 disksleep 0 powernap 0 autorestart 1` and `sudo systemsetup -setrestartfreeze on`.
5. No screen saver, lock or notices over the picture: System Settings > Lock Screen: screen saver and display off Never, password Never; `defaults -currentHost write com.apple.screensaver idleTime 0`; crash dialogs off with `defaults write com.apple.CrashReporter DialogType none`; a Focus that silences notifications; automatic macOS updates off. A solid black desktop picture hides the desktop during the 0.2 s of a restart. Hot corners off: System Settings > Desktop & Dock > Hot Corners, all set to "-" (`defaults write com.apple.dock wvous-br-corner -int 1` and the same for `tl`, `tr`, `bl`, then `killall Dock`). Spotlight: add `~/Nocturne/media` under System Settings > Spotlight > Search Privacy, so macOS does not analyse copied video while the show plays.
6. Autostart: `~/Nocturne/tools/autostart.sh install -- --audio-input Focusrite` (add any other engine options there); it runs the supervisor from `~/Nocturne/Nocturne.app`, and `tools/deploy-mini.sh` (or Update in the Machines tab, `nocturne-ctl machine update BUNDLE`) restarts it on each new build, rolling back a build that fails its health check.
7. Play an authored show: on the laptop, `tools/sync-show.sh show.json`, then on the Mini `~/Nocturne/tools/autostart.sh install -- --audio-input Focusrite --composition ~/Nocturne/shows/show.json --media-root ~/Nocturne/media`. A changed file synced later is loaded with `nocturne-ctl load`; a restart keeps the live state. `tools/show-to-mini.sh [--dry-run] [--reload] [--remap-displays] [--node | --follow MAIN] show.json [MEDIA_ROOT] [-- ENGINE OPTIONS]` does all of it from the laptop: build, deploy, sync, autostart on the show (keeping the Mini's live state of it unless `--reload`), and a check over the LAN that the beat and the clips advance.
8. Or prepare the show with `nocturne-ctl` against the running engine; it is saved as it is edited and comes back after every restart.
9. Prove it: `sudo shutdown -r now`, keyboard unplugged; the picture must come back by itself. `tools/autostart.sh status` and `~/Library/Logs/Nocturne/` tell what happened.
10. Check it: `tools/mini-setup-check.sh` on the laptop reads every item above over SSH without changing anything and prints PASS, FAIL with the exact fix, or MANUAL for what needs sudo or System Settings.

## Reaching the Mini by name (Tailscale)

At a venue the Mini's address changes (a SIM router's lease, or a 169.254.x link-local address on a direct cable), so
the laptop reaches it by one name over Tailscale. Once, on both machines: install Tailscale (the Mac App Store app or
`brew install --cask tailscale`), log in to the same tailnet, and on the Mini turn on "Run at startup" (or "Launch at
login") so it is up before anyone logs in to look. In the Tailscale admin console keep MagicDNS on and turn off key
expiry for the Mini. Its MagicDNS name (`mini`, or the full `mini.TAILNET.ts.net`) is then the one host name for
everything:

- The tools: `tools/mini-host.sh` is the one place every Mini tool (deploy, lock, sync, show-to-mini, watched-show,
  bench, soak, chaos, odd-media, setup check, demo) takes the Mini from: `MINI` for one command, else `NOCTURNE_MINI`,
  else the first line of `~/.config/nocturne/mini`, else `mini.local`. Set it once:
  `mkdir -p ~/.config/nocturne && echo mini > ~/.config/nocturne/mini`. Use `user@host` when the Mini user differs from the laptop's;
  `tools/mini-host.sh` prints the destination and where it came from without connecting, and `tests/mini_host_test.sh`
  checks the selection. ssh needs the Mini's Remote Login on, as on the LAN.
- `nocturne-ctl --host mini` and `open "build/Nocturne UI.app" --args --host mini` (the UI keeps it for next time).
- The engine listens on every interface (`WebSocketServer`), so the Tailscale interface reaches it like the LAN does;
  only machines in the tailnet can.

Bonjour (`--node`, `nocturne-ctl nodes`) stays for the LAN; Tailscale does not carry it, so a node follows over
Tailscale with `--follow mini:7400`. When Tailscale is down (no internet at the venue: Tailscale needs its coordination
server to connect, and a direct cable has no internet), fall back to the LAN: `MINI=mini.local` (Bonjour name,
over the cable's link-local addresses) or the address the router hands out, e.g. `MINI=169.254.12.34
tools/deploy-mini.sh` and `--host 169.254.12.34`.

## Benchmark on the Mac mini

`tools/bench-mini.sh` (on the laptop, with the Mini routed over `en0`) builds Universal at low priority, deploys it with `tools/deploy-mini.sh` to `~/Nocturne/Nocturne.app`, copies `bench/run.sh` and the benchmark clips to `~/Nocturne/bench` on the Mini, runs the scenarios there, fetches the samples, deletes `~/Nocturne/bench` and writes `bench/results/<date>-<commit>.json`. It then compares the result with the newest result in the last `good/*` tag that has one (else `bench/baseline.json`), prints a PASS/FAIL table and exits 1 on FAIL. `tools/bench-mini.sh typical` runs only the gated scenario (about 2 minutes); `--verdict FILE` judges an existing result.

- Clips: `bench/make_media.sh` generates synthetic 1080p H.264 clips (30 and 60 fps, a keyframe every second or every 4-8 s, B-frames) into `~/Library/Caches/Nocturne/bench-media/<script hash>`; they are not committed.
- *typical* (gated): 5 layers with 2-3 effects each (12 in all: Transform, Colorize, Edge Detection, Classic Trails, Trails, Stripper), layers 4 and 5 in a group blended with screen, layers 2 and 3 blended with screen and add; 4 columns of BPM-synced clips at 128 BPM. The bottom layer plays the one long clip (60 s 1080p60, keyframe every 5 s) with Random Jump every 8 beats by up to 2; the other layers play 8 s 30 fps loops. A quantised column trigger every 4 bars. Three outputs with the three mappings: one keystoned slice, two tilted halves, the four quadrants as cube faces. 15 s warm-up, then 75 s measured.
- *heavy* (informational, `tools/bench-mini.sh heavy`): the same composition with Random Jump every beat by up to 8 beats and 1080p60 loops in a quarter of the other cells.
- *stress* (informational): levels of layers x effects (`2x3 3x3 4x3 5x3 6x3 8x3`; the long clip with Random Jump on the bottom layer, 30 fps loops above, effects from the same cycle) on the same three outputs, 20 s measured each. It reports where 60 fps breaks.
- Outputs default to `--virtual 3` (three 1920x1080 windows on the one display). For real displays: `ENGINE_ARGS= DISPLAYS="id1 id2 id3" tools/bench-mini.sh`, with the ids from `nocturne --displays`; `ENGINE_ARGS` also carries debug options. Also `SEED`, `DURATION`, `WARMUP`, `SWITCH_BARS`, `JUMP`, `HEAVY_JUMP`, `LEVELS`, `LEVEL_SECONDS` and `MINI=user@host`. `RECORD=hevc|prores` records the composition through *typical* and *heavy* (results get a `-record-CODEC` suffix and never become a baseline); compare such a run with one without. Every run holds the Mini's exclusive lock (`tools/mini-lock.sh`), and warns when the laptop's load average is above 2.
- Gate (*typical*): render avg >= 59.5 fps; every output (as many as `DISPLAYS` names; one real display gives the F1 setup with all three mappings on it) avg >= 59.5 new frames/s; skipped renders, video misses and hitches per output each <= 0.05/s; no pipeline compiled while measuring; no decode errors; average GPU ms per frame at most 10% above the baseline's.
- Metrics per run: render fps (avg, min), GPU ms per frame (avg, p95 of the per-second averages, max of single frames), video misses in all and by layer, engine CPU %, passes and intermediates; per output presents, shown and new frames per second, hitches (a new frame shown more than half a refresh later than its render start predicts) and the longest present interval; decoded frames/s and decode busy %; system GPU utilisation (`ioreg`), WindowServer CPU and the CPU speed limit from `pmset -g therm` (temperature needs sudo and is not recorded).
- `nocturne --stats-json FILE` appends the per-second stats as one JSON object per line, with the wall-clock time (`unix`), the longest gap between two render frame starts (`maxGapMs`, a stalled render thread shows here) and each output's dropped frames.
- The system samples also record the CPU of `mediaanalysisd` (macOS analysing newly copied video) and Spotlight (`mds`, `mdworker`), which skew a run while busy.

## Profiling with Tracy

The engine can be built for the [Tracy](https://github.com/wolfpld/tracy) profiler (v0.14.1, BSD-3-Clause). The default
build has none of it: the macros in `src/platform/profile.hpp` are empty, and no profiler thread or socket exists.

```sh
cmake -S . -B build-tracy -DNOCTURNE_TRACY=ON   # fetches Tracy at configure time
cmake --build build-tracy -j6
build-tracy/nocturne --offscreen --virtual 3
```

- Tracy UI: it must be the same release as the client, v0.14.1 (`brew info tracy` shows Homebrew's), or build it from
  the fetched source: `cmake -S build-tracy/_deps/tracy-src/profiler -B build-tracy/tracy-ui && cmake --build
  build-tracy/tracy-ui -j6`. The engine listens on TCP port 8086 (the next free one up to 8105 when taken, e.g. by the
  UI app, which links the engine's previews) and announces itself on the local network, so it shows in the UI's list;
  otherwise connect to `localhost` or the machine's address.
- Recording is on demand: nothing is kept until the UI connects, and the GPU timestamp buffers are made only then, so
  the engine can run for hours with the profiler built in.
- What it shows: threads `main`, `render`, `present <display>` (one per output), `decode <clip>`, `clip decoders`,
  `compile` and `previews` with their zones; the render frames as the main frame set and a frame set per output
  (`present <display>`, one frame per refresh); GPU zones per encoder on the render queue (passes, copies, recording),
  each output's queue and the previews' queue. GPU zones need timestamps at stage boundaries (Apple GPUs); the Intel
  Mini shows CPU zones and frame marks only. In a Tracy build `--gpu-profile` samples into attachment 2, so both work
  together.
- On the Mini: `BUILD=build-tracy tools/deploy-mini.sh`, run the engine there as usual (supervisor or autostart), and
  connect the laptop's Tracy UI to the Mini's address (port 8086) over the show network. Save the capture on the
  laptop; captures (`*.tracy`) are never committed.
- `--frame-trace` stays: Tracy replaces reading its JSON to see where a frame's CPU time went and which thread woke
  late, but only while a UI is connected and without the presented (on-glass) times, which the frame trace records for
  unattended bench and soak runs.

## Show resilience

- Fault injection: `tests/resilience_test.cpp` injects each failure of the spec's resilience table into the harness (clips truncated, deleted or half-copied while playing, a media drive removed and back, also as a real disk image, an unplugged audio device, a projector power-cycled, hung and crashed clients, GPU overload, a full disk, odd composition files). Decoder errors, display unplug and engine crashes are in the media, screens and supervisor tests.
- Odd-media corpus: `tests/make_odd_media.sh` generates broken and unusual files (corrupt, truncated, empty, VFR, rotated, phone-style HEVC portrait, 10-bit, odd and non-multiple-of-4 sizes, 4K, 1 and 120 fps, interlaced, VP9, AV1, MPEG-2, audio only; all synthetic) into `~/Library/Caches/Nocturne/odd-media/<script hash>` and prints the folder; `NOCTURNE_ODD_MEDIA_DIR=FOLDER build/nocturne_tests -tc="*odd-media*"` plays each live and checks it plays or is refused as its `manifest.json` says, with no render-thread stall. `tools/odd-media-mini.sh` does the same on the Mini.
- Soak: `tools/soak-mini.sh start` waits for the Mini lock and holds it for the whole run, deploys, copies the benchmark clips, waits up to 30 minutes for `mediaanalysisd` to go quiet and starts `bench/soak.sh` detached on the Mini: 30 minutes (`MINUTES`) on three outputs with the three mappings (three virtual windows by default; real displays with `DISPLAYS="id1 id2 id3" ENGINE_ARGS=""`) under the supervisor with Autopilot on four layers, transitions of every type, LFO and audio modulators, Random Jump and a column switch every 16 bars. `status` shows progress; `fetch` copies the raw data to `bench/soak/` (not committed), writes `bench/results/soak-<id>.json` and prints the verdict of `bench/soak_report.py`. After a 5-minute warm-up it judges slopes extrapolated to an hour and 10-minute windows: footprint and RSS under 10 MB/h, render fps at least 59 in every window with no falling slope, every output's new frames/s and hitches per minute not trending worse (real outputs also at least 59 new frames/s; virtual windows share one display, so only their trend counts), misses per minute, engine CPU and decode busy not rising (under 10/h, 5/h and 5/h), GPU ms within 10 % of its mean per hour, the CPU speed limit (the temperature proxy; temperature needs sudo) within 5 points, and no crash. `bench/soak_report.py RAW COMMIT MINUTES` judges the first MINUTES of a longer run.
- Chaos rehearsal: `tools/chaos-mini.sh` scripts what runs without hands (clients stalled and killed, engine killed and hung, state disk full, media drive detached and back) and writes `bench/results/chaos-<date>-<commit>.json`; the manual steps (displays, projector, audio interface, network, power) are done by hand while it plays.
