# Every output presents on its own thread

The composition is rendered once on the render thread. Each display output then has its own present thread, paced by its own display link, that maps the latest finished frame onto its slices.

Resolume swaps all displays on the render thread, so one slow output stalls everything. The prototype showed that one output slowed to 22 fps leaves rendering and the other outputs untouched (prototype branch, NOTES.md).

## Frames in flight on the GPU

When the GPU is near 100 %, rendered frames queue up on it ahead of the outputs' presents, and the presents wait
behind them (measured up to ~57 ms from render start to GPU start with two outputs). The `framesInFlight` machine
setting (`nocturne-ctl settings in-flight ring|1|2`, `EngineConfig::maxInFlight`) bounds how many rendered frames may
be on the GPU at once: a frame due while that many are waits for one to finish, up to a frame's time, so the GPU goes
straight on to the next frame while fewer are ever queued ahead of a present. The default stays the frame ring's
bound until the reference machine says otherwise; the report test "under GPU overload: frames in flight on the GPU"
(`tests/resilience_test.cpp`) prints the render rate, the queueing time and each output's new frames and hitches per
second for the ring, 2 and 1.
