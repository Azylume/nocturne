# Every output presents on its own thread

The composition is rendered once on the render thread. Each display output then has its own present thread, paced by its own display link, that maps the latest finished frame onto its slices.

Resolume swaps all displays on the render thread, so one slow output stalls everything. The prototype showed that one output slowed to 22 fps leaves rendering and the other outputs untouched (prototype branch, NOTES.md).
