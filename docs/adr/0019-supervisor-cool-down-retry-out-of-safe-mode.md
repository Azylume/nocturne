# The Supervisor may retry the Engine once out of safe mode after a cool-down, off by default

Status: Accepted ([#54](https://github.com/Azylume/nocturne/issues/54)); the Decisions section at the end answers the open questions and says what is implemented.

## The problem, from the code

The Supervisor (`src/app/supervisor.cpp`) counts crashes and hangs in `crashed()`: each one is pushed on `crashes_`, entries older than `--crash-window` (60 s) are dropped, and when `--crash-limit` (3) remain it sets `safeReason_` ("3 crashes within 60 s; last: ..."). A client's "safe mode" request (the Engine exits with 76, `src/supervisor/restart_request.hpp`) sets it too, with the reason "asked for from a client". The main loop in `run()` then calls `runSafeMode()` instead of `runEngine()` for as long as `safeReason_` is set.

`runSafeMode()` starts the Engine with `--safe-mode REASON` (and `--fallback-image` when set). That Engine runs `runSafeMode` in `src/app/safe_mode.hpp`: black or the fallback image on every display with the reason along the bottom, an empty read-only document, and a protocol that serves only the machine status (`"mode": "safe"`, `"reason"`), Restart engine and the Show lock. Nothing that crashed it runs, and there is no autoplay.

There is one way out inside a running Supervisor: the safe-mode Engine exits with 75 (a client pressed Restart engine, which the UI labels "Leave safe mode"), and `runSafeMode()` clears `safeReason_` and `crashes_`. If the safe-mode Engine exits any other way, the Supervisor waits 2 s and starts it in safe mode again. The other way out is restarting the Supervisor itself (`tools/autostart.sh restart`, `docs/reference.md` line 40): `safeReason_` and `crashes_` live in memory only, so a fresh Supervisor starts a normal Engine. What does survive is `skip.json`, the skip list.

So a burst at 23:00 (a driver hiccup, a drive that reconnects) on a machine nobody is watching leaves the fallback image up until a person connects or restarts the agent. That is the case #54 is about.

Related, from `crashed()` and `src/supervisor/crash_guard.cpp` (`culprits()`): only marks held by the crashing thread are skip-listed, and a crash needs a `crash.json` record. A hang kills the Engine from outside, so no record exists and `culprits()` is empty. A show that hangs three times on the same clip therefore goes to safe mode with nothing on the skip list, and any retry would hang on it again.

## Options

1. **Never retry (today).** Safe mode lasts until a person acts. Predictable, and the cost is the dark venue above.
2. **One timed retry.** After a cool-down of N minutes in safe mode caused by crashes, the Supervisor starts the normal Engine again, with a cap on total retries per 24 hours.
3. **Escalating back-off.** Like 2, with the cool-down growing after each retry that fails: N, 2N, 4N, up to the cap.
4. **Retry only a transient cause.** Classify the reason: a GPU device lost, a drive that vanished, a start that failed once, retry; the same crash on the same composition, never. The Supervisor has little to classify with today: a `Crash` is a thread and a signal, the marks are a clip or an effect chain, and `restart_request.hpp` has no exit code for a lost device (`kNoGpu` is a startup failure and stops the Supervisor). It would need the Engine to say why it died, which is protocol and exit-code surface ("Ask first" in AGENTS.md), and a classifier that is wrong in the dangerous direction.

## Proposal

Option 3, kept small, behind a flag that is off by default:

- `nocturne-supervisor --safe-mode-retry MINUTES` (decimal allowed, so a test can use 0.05). Default 0 means off, and today's behaviour is unchanged.
- Fixed schedule, no second flag: the cool-down is MINUTES, then 2x, then 4x, and at most 3 retries in any rolling 24 hours. Constants in the Supervisor, not flags, until a venue needs them changed.
- Only safe mode entered by `crashed()` is retried. Safe mode asked for by a client is a person's decision and is never undone by a timer.
- After a retry the Engine is on probation: one crash or hang before it has run stably for 10 minutes puts it straight back in safe mode (crash limit 1), without three more restarts. This keeps a deterministic crash from showing a picture, dying, restarting and dying again for a minute, and cuts the work each attempt costs. A retry that survives the probation resets the cool-down to MINUTES; the daily count is kept.
- The timer lives in `runSafeMode()`, which already polls every 0.2 s. When it is due, the Supervisor stops the safe-mode Engine as it does on a stop, clears `safeReason_` and takes the same path as leaving safe mode on a client's Restart.
- Hangs: no change in this proposal (see the open questions).

## Risks for a live show

- **A visible restart mid-show.** Safe mode is already black or the fallback image, so the retry replaces a dead picture, not a working one. A resumed show is held black until its clip is decoded, then continues on the beat grid (`tests/supervisor_test.cpp`, "a resumed show is held black..."). The ugly case is a retry that fails: the picture comes back and goes away. Probation limits that to the seconds until the first crash, and the cap limits it to three times a day.
- **Retry storms.** Each attempt starts decoders and pipeline compiles on a machine whose GPU may be the fault. The cap, the doubling and the probation bound it; the cap is rolling over 24 hours, in memory.
- **Masking a bad composition.** A deterministic crash with a culprit mark is skip-listed on the first crash, so the retry then runs without it and may succeed: that is the skip list working. A deterministic crash with no culprit (a crash elsewhere, a hang) repeats until the cap, and the operator sees every attempt in `status.json` and the log, so it is not silent. Safe mode that never ends is loud; one that ends is quiet, so the log lines and fields below must be unmissable.
- **The Show lock.** The lock refuses client requests for restart and safe mode (`src/engine/machine_actions.cpp`, state in `show-lock.json`). A timer is not a client request, and the lock says nothing about a dark show. The proposal does not consult it. The Supervisor does not read the lock, and the safe-mode Engine's document is empty, so honouring it would also mean new plumbing.
- **The Gig window.** Only the Engine knows the composition's Gig windows (used to refuse Update, `src/engine/machine_service.hpp`); the Supervisor and the safe-mode Engine do not. The proposal does not depend on them: a dark screen during a window is the worst case, and outside one nobody minds a retry.
- **A restart of the Supervisor resets the count.** A Supervisor that is itself restarted by launchd repeatedly would also reset the daily cap. Persisting it in the state folder fixes that at the price of a file.

## What to log and report

- `supervisor.log`: "safe mode: retry in N min (attempt K of 3)", "safe mode: retrying the engine", "safe mode: retry failed, back in safe mode, next in M min", "safe mode: retry held for 10 min, cool-down reset", "safe mode: retry cap reached, staying in safe mode".
- `status.json` (`writeStatus()` today writes `mode`, `reason`, `error`, `engine`, `starts`, `crashes`, `supervisor`): add `"retry": null` when off, otherwise `{"attempts": K, "limit": 3, "nextAt": UNIX seconds or null, "last": "survived" | "failed" | null}`, and keep each retry's crash in `crashes` as now.
- Machine status, which the UI Machines tab and the Venue desk read: the safe-mode Engine would need the next retry time. The Supervisor passes it as an argument when it starts the safe-mode Engine (`--safe-mode-retry-at UNIX`), and the Engine adds it to the `"mode": "safe"` status. The UI shows "Retrying at 23:20" beside the reason, and the desk says the same. That is a protocol field, so it needs a human's OK, and the part of #54's acceptance criteria about the desk page depends on it. `docs/reference.md` would change with the implementation.

## Failure modes

- The retried Engine starts and dies before its first frame: counted as a crash, probation puts it back in safe mode.
- The Supervisor is asked to stop during a cool-down: it already stops from `runSafeMode()` within 0.2 s; the timer dies with it.
- The clock jumps: the cool-down uses `steady_clock` like the crash window; `nextAt` is wall time for display only.
- A client presses Restart engine during a cool-down: it leaves safe mode as today and the retry state is cleared.
- The status write fails: logged, as `writeStatus()` does today; the retry goes on.

## Tests

In `tests/supervisor_process_test.cpp`, with the `Supervised` fixture and the fake engine (`tests/fake_engine.cpp`), which needs one new command that crashes its first N runs, counted in a file in the state folder. Cases, with `--safe-mode-retry 0.05`:

- Off by default: three crashes, safe mode, and a minute later the start count has not moved (the existing "three crashes within a minute" case already covers the start; add the wait).
- Retry, then success: crashes 3 times, then runs; the status goes safe, then normal, `retry.attempts` is 1, the log says "retrying".
- Retry, then safe mode again: the fake engine keeps crashing; a probation crash returns to safe mode after one start, the next cool-down is doubled, and after 3 retries the Supervisor stays in safe mode.
- Safe mode asked for by a client (`exit 76`) is never retried.
- A stop during a cool-down exits cleanly.
- `status.json` carries `retry` with the fields above in each state.

The issue's wish for a case with `NOCTURNE_CRASH_ON` is one real-engine case in `tests/supervisor_test.cpp`; it is a real-time case (supervisor-started engine), so it would need the usual `.github/realtime-tests.txt` pattern and a reviewer's OK.

## Recommendation

Do option 3 as described, off by default, because an unattended venue that stays dark all night over a one-minute burst is a worse result than a restart that may show a picture and fail. Not doing it is a legitimate answer: a venue with a person on the desk gets "Leave safe mode" in the UI and the desk, and the cap and probation above are the whole safety case, which a reviewer may find too thin for a feature that restarts a show without anyone asking. Option 4 stays out until the Engine can say why it died.

## Open questions for a human

1. Do we want automatic retry at all, or is a dark screen until someone acts the right failure for a show machine?
2. Schedule: MINUTES, 2x, 4x, cap 3 per rolling 24 hours, 10 minutes of stable running to clear probation. Right numbers, and should any be a flag?
3. Probation (crash limit 1 after a retry) instead of the normal 3 within 60 s: agreed?
4. Should the Show lock stop the timer ("nothing changes while it is on"), or stay as proposed because the lock guards clients and not a dark screen?
5. May the safe-mode status gain a retry time (a protocol change) so the UI and the desk can show it, or should the retry be visible in `status.json` and the log only?
6. Should the retry cap survive a Supervisor restart, in a state file?
7. #54's last criterion: should a repeated hang on the same in-flight mark add a skip? Hangs have no `crash.json`, and a mark held when the Engine was killed may be innocent (a slow decode while the render thread hung for another reason). This proposal leaves hangs alone. If it is wanted, the safest rule is the identical mark set held across two hangs in a row, as its own change and its own decision.
8. Does #54's acceptance wording (`--safe-mode-retry MINUTES` with "backoff and a cap") match option 3 here, or is a plain single retry enough?

## Decisions

The owner delegated the open questions; they are answered as follows. Implemented means in the change that accepted
this ADR.

1. Retry at all: yes, strictly opt-in and off by default, so manual control stays optional. Implemented.
2. Schedule: `--safe-mode-retry MINUTES` sets N; the cool-downs are N, 2N and 4N, at most 3 retries in any rolling
   24 hours, and 10 minutes of stable running end the probation. Only N is a flag. Implemented
   (`src/supervisor/safe_retry.hpp`).
3. Probation: yes, crash limit 1 after a retry, for 10 minutes. Implemented.
4. The Show lock does not stop the timer: the Supervisor knows neither the Show lock nor Gig windows, only the Engine
   does. A retry never happens when safe mode was requested by a client (`machine safemode`), and a client's Restart
   engine clears the retry state. Whoever enables the flag accepts a visible restart out of a dark safe mode.
   Implemented, and said in `docs/reference.md`.
5. The safe-mode machine status gains the optional field `safeRetryAt` (UNIX seconds, `null` when no retry is
   scheduled). The Supervisor passes it to the safe-mode Engine as `--safe-retry-at`. Implemented; showing it in the UI
   Machines tab and the Venue desk is not, and is a follow-up.
6. The cap survives a Supervisor restart: `status.json` already is the state file the Supervisor writes, and its
   `retry` object gains `startedAt`, the start times of the retries within 24 hours, read back on start. A file
   without it counts as no retries. Implemented.
7. Hangs on the same in-flight mark: no skip. Hangs stay as they are (see the open question above). Nothing to
   implement.
8. #54's wording ("`--safe-mode-retry MINUTES` with backoff and a cap") is option 3 here. Implemented.

Differences from the proposal's text: `status.json`'s `retry` is `null` when the flag is off, else `{"attempts",
"limit", "nextAt", "last", "startedAt"}`; `nextAt` and `startedAt` are whole UNIX seconds. The `NOCTURNE_CRASH_ON` case
the issue asks for is not added: it would be a real-time case needing an entry in `.github/realtime-tests.txt`. The
fake engine tests in `tests/supervisor_process_test.cpp` and the unit tests in `tests/safe_retry_test.cpp` cover the
logic on short intervals instead.
