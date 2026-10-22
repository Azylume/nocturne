# Security

Report vulnerabilities privately through GitHub: on this repository, open the Security tab and choose "Report a
vulnerability". Please do not open a public issue for a security problem.

Include what you found, how to reproduce it, and the commit or release you tested. You will get an answer within a
week. Fixes land on `main`; there are no older release branches to backport to yet.

Scope: the engine, the UI, the venue desk page and the command-line tools in this repository. Nocturne listens on the
local network (the protocol port and the desk page), so anything that lets a machine outside the LAN or the tailnet
control it, or reads files outside the show's folders, is in scope.

## Network trust

Nocturne is built to run on a trusted LAN or a tailnet. What a machine exposes:

- **The protocol port, 7400 (`--port`), on every interface.** It is the whole control surface: edits, show files,
  media, recording, settings and pushing a new engine bundle (`nocturne-ctl machine update`). A client on another
  machine must present the machine's pairing token in its first message, the protocol `hello`; without it the engine
  answers with an error and closes the connection. The token is 32 random bytes the engine makes once per machine and
  keeps as `token` in its state folder (default `~/Library/Application Support/Nocturne/State`), readable by its user
  alone; `nocturne-ctl token` on that machine prints it. Clients from a loopback address (127.0.0.1, ::1) need no token,
  so the local UI, the local `nocturne-ctl`, the venue desk and anything reaching the engine through an ssh tunnel get
  in without setup; anyone who can run a process on the machine is trusted. A WebSocket handshake that carries an
  `Origin` header, as every web page's does, is refused, so a page open in a browser cannot borrow that trust. A
  client from another machine that has not paired within 10 seconds is closed, and a message over 16 MiB closes the
  connection. A paired client from another machine
  names files only inside the shows folder (`~/Nocturne/shows`), the media folders (`~/Nocturne/media`, `--media-root`),
  the Media Library folder, the recording folder and the state folder: composition save and load, the shows list,
  library add and relink, the recording folder and its browser, and preset import refuse any other path, a `..` part
  and a symbolic link that leads out. A pushed bundle is installed only when it is signed by the identity that signed
  the running bundle; an engine signed ad hoc refuses pushed bundles.
- **The venue desk page (`nocturne-desk`, its own port).** Read-only: it answers `GET` with tonight's lineup and the
  live deck to clients on the LAN, and refuses other sources. It reaches the engine over loopback and asks it nothing
  but the desk status, which the engine answers to loopback clients alone; it cannot send protocol commands.
- **Bonjour and the UDP clock port.** The engine advertises itself as `_nocturne._tcp` and answers clock pings on UDP
  next to the protocol port; neither reads nor changes anything else.

The token travels in the clear, as the protocol does: on a network you do not trust, reach the machine over Tailscale
or an ssh tunnel, not across the open network. Treat the token like a password; replace it by deleting the file and
restarting the engine (every client then needs the new one).
