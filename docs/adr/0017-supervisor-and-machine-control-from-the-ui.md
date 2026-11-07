# The Machines tab controls the Supervisor in two phases: through the Engine first, then through one small Supervisor listener

Status: Proposed. Nothing here is implemented; [#72](https://github.com/Azylume/nocturne/issues/72) asks for it and
builds on [#66](https://github.com/Azylume/nocturne/issues/66). Open questions are listed at the end.

An operator should stop, start, restart and set up a show machine, and read why it failed, from the Machines tab without
a shell. #72 lists five needs. The protocol cannot serve them all today, and one of them (Start after Stop) needs a
listener that does not exist. This ADR picks the smallest surface for each need and splits the work so that phase 1 adds
no listener.

## What exists today

- `machine stop`, `restart`, `safemode` and `lock` are served by the Engine (`src/engine/machine_actions.cpp`). `restart`
  and `safemode` make the Engine exit with a status the Supervisor reads (`src/supervisor/restart_request.hpp`: 75, 76,
  77 for stop, 78 for no usable GPU). The Show lock (a file the Engine keeps) refuses all three, Stop included, from
  every client, loopback ones too. `stop` makes the Supervisor exit without starting the Engine again, so after it
  nothing listens on port 7400 (`src/app/supervisor.cpp`, `runEngine`).
- The Machines tab has Restart engine and Safe mode, both hold-to-act, and shows the Show lock (`src/ui/app/machines_view.cpp`).
  It has no Stop button. The status it gets carries `mode` and the safe-mode reason only (`src/ui/client/engine_client.hpp`,
  `safeReason`).
- The Supervisor writes `status.json` in its state folder: `mode`, `reason`, `error`, `engine` pid, `starts`, `crashes`
  (the last 20) and `supervisor` pid (`writeStatus`). It has no start time and no autostart state. The file is readable
  on the machine only; no protocol message returns it.
- The Supervisor has no listener. The venue desk is its child and read-only (SECURITY.md: it "cannot send protocol
  commands"). The protocol port, the pairing token, the `Origin` refusal and the pre-pairing limits belong to the Engine
  (SECURITY.md, "Network trust"). Paired clients from another machine may name files only in the shows, media, Media
  Library, recording and state folders (`src/protocol/remote_files.hpp`, `pathRefusal`).
- Autostart is `tools/autostart.sh` (a LaunchAgent, `ai.azylume.nocturne`) and `installer/autostart.ps1` (the scheduled
  task `Nocturne`, registered with run level Limited). Both take engine options once, at install. The LaunchAgent is
  written with `KeepAlive` true; `docs/reference.md` says a stopped Supervisor stays down until `autostart.sh restart`.
  Whether launchd honours that after exit 0 has not been checked for this ADR.
- Outputs are already a saved machine setting: `nocturne-ctl screendisplay SCREEN DISPLAY|none` is "kept on the
  engine machine for every show's screen of that name" (`docs/reference.md`, `src/app/ctl.cpp`).

## Decision

### 1. Status: the Engine returns the Supervisor's status (phase 1)

`machine status` gains a `supervisor` object read from `status.json` in the Engine's own `--state` folder, which the
Supervisor passes to it. The Supervisor adds `startedAt` (wall-clock time of the current engine start) to the file; uptime
is computed by the client. Fields: running (the `supervisor` pid is alive), `mode`, `reason`, `error`, `enginePid`,
`startedAt`, `starts`, `crashes`, `autostart` (`on`, `off` or `unsupported`, see 2). An Engine started without a
Supervisor (`NOCTURNE_SUPERVISED` unset) reports `supervisor: null`, and the tab hides the panel.

- **Who:** any client that may already ask `machine status`: loopback, or paired. Read-only, so the Show lock does not
  refuse it.
- **Failure modes:** a missing, half-written or stale file reports `supervisor: null` or `running: false`; the Engine
  never blocks on it (it is read on the Machine service thread, never on the render thread). When the Engine is down
  nobody answers, and the tab shows the machine as unreachable until phase 2.
- **Considered:** the Supervisor answering status itself. It needs the listener of 3 for no gain while the Engine is up.

### 2. Autostart on/off: a fixed write op through the platform boundary (phase 1)

`machine autostart on|off` (reply `{"type": "machine", "op": "autostart", "on": ...}`) is served by the Engine. It calls
one new platform interface (next to `src/platform/process.hpp`) with two implementations chosen at compile time, as
ADR 0016 does for the rest:

- macOS: `launchctl enable` or `disable` of `gui/<uid>/ai.azylume.nocturne`; status from `launchctl print-disabled`.
- Windows: `Enable-ScheduledTask` or `Disable-ScheduledTask` of the task `Nocturne`.
- Unknown platform, or the job is not installed: status `unsupported`, the op is refused with the reason.

The op toggles an installed job; it never writes a plist or registers a task. Installing and removing belong to the
setup scripts of #66. It runs fixed program names with fixed arguments and no client string, through no shell. Turning
autostart off does not stop the running show: it only decides what happens at the next login or reboot.

- **Who:** a paired client or loopback, like `machine restart`. The Show lock refuses it, because off weakens a locked
  venue.
- **Failure modes:** the command fails or lacks the right to run: the reply is an `error` with the command's message and
  the status is unchanged. The Engine runs as the user in the desktop session, so the macOS agent needs no elevation.
  `installer/autostart.ps1` says to run `install` as admin and the task runs at level Limited; whether a Limited process
  may enable or disable its own task has to be tested on Windows before phase 1 is accepted for Windows (open question).
- **Considered and rejected:** `bootout` and `rm` of the plist as "off". `bootout` alone returns at the next login, and
  deleting the plist makes "on" a reinstall that the UI must not do. The acceptance criterion "off survives a reboot" is
  tested per platform; it is not assumed from the commands.

### 3. Start after Stop: phase 1 offers none, phase 2 adds one Supervisor listener

Nothing can start an Engine when the Supervisor has exited, and nothing can answer a message when it is down. Options:

- **A. No new listener (phase 1).** The tab gets Restart and Leave safe mode (already there) and Stop stays a
  `nocturne-ctl` command with its documented cost. The tab offers no Stop button, so it never strands a remote
  machine. This does not meet the "start" part of #72.
- **B. The Supervisor stays up after Stop** ("idle": no Engine, supervisor alive). Still needs a listener to be told to
  start. Only worth doing with C.
- **C. A Supervisor listener on its own port (phase 2, recommended).** One thread in `nocturne-supervisor`, always on,
  that answers only `hello`, `status` (the same `supervisor` object as in 1) and `start`. Everything else stays served by
  the Engine while it runs, so no op exists in two places. With C, `stop` can leave the Supervisor idle (B) so that
  `start` has something to talk to; `nocturne-ctl machine stop` keeps working.
- **D. The Supervisor takes over port 7400 while the Engine is down.** One port for the UI to know, but a hand-over race
  between two processes on one port, and a second implementation of the Engine's pairing path in the Supervisor. Rejected.
- **E. Reuse the venue desk.** Rejected: the desk is a child that dies with the Supervisor, and its read-only, LAN-open
  contract (SECURITY.md) must not gain a write.

The trust rules for C are the Engine's, not new ones, and are part of the decision:

- The same pairing token (`token` in the state folder, `src/net/pairing.hpp`), presented in the first message, `hello`.
  Loopback needs none. A WebSocket handshake with an `Origin` header is refused. A client that has not paired in 10 s is
  closed; 32 may wait, 4 per address; 64 KiB before pairing. The listener shares the Engine's code for this, not a copy.
- Messages are few and fixed: no path, no command line and no file name travels in `start`.
- `start` is refused while the Show lock is on, read from the same lock file as the Engine reads it. A lock file that
  cannot be read counts as on, as `readLock` does today.
- **Port:** a second port (default to be fixed in review). It must be added to the Windows installer's firewall rules for
  `nocturne-supervisor.exe` on the Private profile only (`installer/nocturne.iss`), and to SECURITY.md in the same change
  that opens it. It is advertised nowhere: the UI derives it from the machine it already pairs with.
- **Failure modes:** the port is taken (the Supervisor logs it and runs without the listener; the tab falls back to A);
  `start` while an Engine runs is a no-op that replies the current status; `start` after a no-GPU stop (status 78)
  starts the Engine, which exits 78 again and shows the same `error`.
- **Windows:** the Supervisor is a Windows-subsystem program under a job object that ends its children with it
  (`killStale` comment in `supervisor.cpp`); idle-after-Stop keeps that job alive and changes nothing else. The firewall
  rule is the only installer change.

### 4. Saved machine settings: a file the Supervisor reads at each Engine start (phase 1)

Settings are `machine-settings.json` in the state folder, with `composition` (show file), `mediaRoot` and `audioInput`.
The Engine validates and writes it atomically (`writeFileAtomically`, as for the lock file); the Supervisor reads it
before every Engine start and turns it into `--composition`, `--media-root` and `--audio-input`
(`src/app/engine_config.cpp`). They apply on the next start, never to the running Engine, and the reply says so. The
Supervisor logs each applied value to `supervisor.log`.

- **Outputs need nothing new.** `screendisplay` already saves screen-to-display on the machine; the tab shows and edits
  it through the existing messages.
- **Path confinement (SECURITY.md):** from a client on another machine, `composition` must be inside the shows folder
  and `mediaRoot` inside the media folders, checked with `pathRefusal` as composition save and load are, symbolic links
  resolved. A loopback client is trusted as it is today. `audioInput` is a device name, not a path: it must be empty or
  match a device the Engine lists. The Supervisor checks again at read time; an invalid or unreadable file is ignored
  with a log line and the Engine starts with the flags of the launch definition, so a bad setting never keeps a show off.
- **Who:** paired or loopback. Refused under the Show lock and inside a Gig window, as `machine update` is
  (`updateRefusal` in `src/engine/machine_service.cpp`). The Show lock decision for settings is open question 4 below.
- **Precedence:** the file wins over flags in the LaunchAgent or task, because otherwise a saved setting would silently
  do nothing. The tab shows the effective values and says where each came from.
- **Considered and rejected:** the tab rewriting the plist or task arguments. That makes the network edit the launch
  definition of a program, differs per platform (the Windows task passes no engine options), and needs a reload to apply.

### 5. Log tails: fixed files, capped, served by the Engine now and the listener later

`machine logs FILE [LINES]` with `FILE` one of `engine`, `supervisor` (never a path or file name from the client),
`LINES` at most 200 and the reply at most 64 KiB, cut at a line start. It reads the newest bytes of one file in the logs
folder (`appLogsFolder`, `--logs`). The Supervisor passes that folder to the Engine as a new flag in phase 1.

- **Who:** paired or loopback. Read-only, so the Show lock does not refuse it. A paired client therefore reads file
  contents: the logs hold media and show paths, display and audio device names and crash causes. They are the machine's own
  diagnostics on a network SECURITY.md already trusts with every show file and media. The token is not logged
  (`engine_main.cpp` prints the token file's path, not the token); a test must keep it that way.
- **Failure modes:** a missing file is an empty reply; a file rotating under the read (`rotating_log.cpp`) gives a short
  or odd first line, which is fine for a tail. While the Engine is down, `supervisor` and `engine` logs are not
  readable until phase 2, which is when they matter most; in phase 2 the listener of 3 also answers `logs`. This is the
  one message that would then exist in two places, served by one shared function.
- **Not exposed:** `audio.jsonl`, `stats.jsonl`, `launchd.log`, crash dumps and any other file.

## Phases

1. **Phase 1, no new listener:** 1 (status), 2 (autostart), 4 (settings), 5 (logs while the Engine is up). The tab gets a
   panel with those and no Stop button. `docs/reference.md`, `nocturne-ctl` equivalents (`machine autostart`, `machine
   settings`, `machine logs`) and SECURITY.md (the new reads and writes) change with it.
2. **Phase 2, one listener:** 3 (C) with `start`, `status` and `logs` while no Engine runs, and the Stop button. Opening a
   port is a protocol and trust change and is decided here, not in the pull request that implements it.

## What we reject, and why

- **A shell or `ssh` from the UI.** The UI is a protocol client and the Engine takes input only through the protocol
  (ADR 0002, ADR 0014); an `ssh` dependency is a credentials story of its own.
- **An unauthenticated or loopback-only Supervisor listener.** Unauthenticated puts a start command on the LAN. Loopback
  only is unreachable from a remote Director, which is the case that needs it.
- **A general "run this" or "tail this file" op.** Every message names a fixed action or a fixed file.
- **Replacing the Engine's `machine` ops with Supervisor ones.** It splits the surface for no gain and breaks existing
  `nocturne-ctl` and UI clients.

## Cost

Phase 1 leaves a stopped remote machine unreachable until someone restarts it by hand, which the missing Stop button
avoids. Phase 2 adds a second listener to review, port and firewall rule to document, and idle-after-Stop changes what
`machine stop` means to anyone who relies on the Supervisor exiting. Settings in a file beside the launch definition mean
two places can name the same flag; the precedence rule and the tab's "effective value" display are what keep that
legible.

## Open questions

1. Is Phase 2's separate port acceptable, or should the Supervisor take over 7400 (option D) despite the hand-over?
2. Port number and whether to advertise it (Bonjour) or only derive it.
3. Should Stop remain a `nocturne-ctl`-only command in the UI even after phase 2, or get a Stop button with a hold-to-act?
4. Settings under the Show lock and in a Gig window: refused (as proposed) or allowed because they apply only on the next start?
5. May a Windows Limited process enable or disable its own scheduled task? To be tested on Windows before phase 1 is
   accepted for Windows; if not, the task needs another run level or this op is macOS-only at first.
6. Does the LaunchAgent's `KeepAlive` true restart the Supervisor after `machine stop`? It decides how much of A and the
   idle state (B) is needed on macOS, and has to be tested on a Mac.
7. Settings precedence: file over flags (proposed) or flags over file.
