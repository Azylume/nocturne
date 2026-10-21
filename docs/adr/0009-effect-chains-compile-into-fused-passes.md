# Effect chains compile into fused GPU passes

A layer is not rendered as one GPU pass per effect. The engine compiles each layer's source and effect chain into as few passes as possible and blends the last one straight into the composition with fixed-function blending.

On the 2018 Mac mini (Intel UHD 630) the cost of a layer is dominated by full-resolution writes and reads between passes, not by the effect math. Measured there:
- fusing plus fixed-function blending cut GPU time by 35-40%;
- 5 layers of Transform + Colorize + Edge went from 45 fps / 20.4 ms to 60 fps / 13.3 ms.

The naive pass-per-effect design (what the first prototype did, and what Resolume's texture stack implies) does not fit the v1 show machine. Fusing makes effects code-generated rather than hand-written shaders per pass, which is the cost of this decision.
