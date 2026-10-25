# src/engine/AGENTS.md

The headless renderer. Read `engine.hpp`, `chain_compiler.hpp` and `render_thread.hpp` before changing how a frame
is made.

## Threads

| Thread | Owner | May |
|---|---|---|
| Render thread | `RenderThread` drives `Engine` at a fixed rate | look up finished results, encode GPU work; nothing that waits |
| Compile thread | `ChainCompiler` | compile Metal pipelines, read and write the pipeline cache |
| Present thread, one per output | `src/output/` (ADR 0003) | map the latest finished frame onto its slices |
| Decode thread, one per clip | `src/media/` | read and decode ahead into the clip's frame cache |

## Contracts in comments

Every public method says where it may be called and whether it blocks. Keep that, and write it for new code:

```cpp
// Any thread, returns at once: wants only this one chain, for a compiler of its own such as a preview's.
void prepare(const Composition& composition, ChainKind kind, const EffectList& effects);
// Nothing is queued or compiling. Never blocks.
bool idle() const;
```

Members say who owns them (`// Compile thread only.`, `// Under mutex_.`). A new member touched from two threads
without such a note is a review blocker.

## Rules

- A live action from a client lands as data the render thread picks up at its next frame ("never blocks the render
  thread", see the comments in `engine.hpp`), not as a call into the frame.
- Per-frame resources come from the render thread's pools; no allocation per frame on the hot path.
- A pipeline compiled while a show plays is a hitch: new chains are prepared ahead (`ChainCompiler::prepare`), and the
  benchmark gate fails when a pipeline compiles while it measures.
- Stalls show in the stats: `nocturne --stats-json FILE` records `maxGapMs`, the longest gap between two frame starts.
- Test the real render thread with `EngineHarness::runLive` / `holdCompiles`, not only `step()`, when your change is
  about timing (see `tests/AGENTS.md`).
