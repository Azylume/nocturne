# Release notes

## 0.1.1

Security release. Upgrade every engine and UI; see the advisory GHSA-pxq7-wh9p-6hw8.

Fixed:

- A host on the network could crash the engine or exhaust its memory with malformed protocol messages before pairing.
- A web page open in a browser on the engine machine could control the engine through its loopback trust.
- A paired client could read files outside the show and media folders through media stills.
- A rogue main engine could make a follower node overwrite or delete files, and learn the main's pairing token.
- Crafted show files, HAP movies, user ISF shaders and Resolume XML could crash the engine or exhaust its memory.
- A race in the venue desk server when the engine connection dropped.

Behaviour changes:

- A bare `--node` no longer sends its pairing token to, and so no longer pairs with, a main engine on another machine.
  Use `--follow HOST`. The token change narrows the leak but does not close it; see SECURITY.md.
- Compositions and saved state with a clip hash that is not a SHA-256 content hash are refused.
- Unpaired clients from another machine: messages up to 64 KiB, 10 s to pair, at most 4 waiting per address and 32
  overall. Paired clients: messages up to 16 MiB. Messages nested more than 64 levels deep are refused.
- WebSocket handshakes that carry an `Origin` header are refused.
- MessagePack with strings that are not UTF-8 is refused as undecodable.
