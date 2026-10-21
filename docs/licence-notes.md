# Licence notes

The repository is `AGPL-3.0-or-later` (ADR 0012). Before adding a dependency (FetchContent, vendored code, ported
shaders, copied snippets), check its licence here and add a row to ADR 0012's tables.

- **Compatible:** MIT, BSD-2/3-Clause, ISC, zlib, Apache-2.0, MPL-2.0, LGPL-2.1+/3, GPL-3.0(+), AGPL-3.0(+), public domain.
- **Not compatible:** GPL-2.0-only, proprietary or "non-commercial" licences, licences with field-of-use limits,
  code with no licence.
- **Keep the notice:** ported or vendored code keeps its copyright and licence text next to it.
- **Ported:** AMD FidelityFX Super Resolution 1 (MIT, Copyright (c) 2021 Advanced Micro Devices, Inc.): the EASU and
  RCAS shaders, ported to Metal in `src/engine/fsr_upscaler.cpp`, which carries AMD's copyright and licence text.
- **Commits** carry `Signed-off-by:` (DCO); no CLA.
- **Unsure:** stop and ask the maintainer before adding it.

## ISF import (ticket 95), checked 2026-10-18

| Component | Version | Licence | Use | Compatible |
|---|---|---|---|---|
| glslang | vulkan-sdk-1.4.363.0 | BSD-3-Clause, BSD-2-Clause, MIT, Apache-2.0 (its LICENSE.txt); the "GPL-3 with Bison exception" section covers only Bison skeleton output, which the exception lets us ship under any terms (and Bison is no longer used) | FetchContent, linked into the engine; GLSL to SPIR-V | yes |
| SPIRV-Cross | vulkan-sdk-1.4.363.0 | Apache-2.0 (a few files MIT) | FetchContent, linked into the engine; SPIR-V to MSL | yes |
| ISFMSLKit (mrRay) | 3aa9355 | MIT; its own dependencies are BSD/MIT/Apache | reference reading only, nothing copied | yes (unused) |
| VVMetalKit (mrRay) | 85ff46e | MIT text (README calls it BSD); bundles Apple's AAPLMathUtilities under the Apple sample code licence | reference reading only, nothing copied | yes (unused) |
| ISF-Files (Vidvox) | 395072d | MIT for the repository; shaders ported from elsewhere carry their own terms, and many say nothing | source of the curated shaders, vendored verbatim with the licence next to them | per shader, below |
| gl-transitions | via ISF-Files | MIT ("gl-transitions contributors"); each transition names its author and "License: MIT" | the curated transitions | yes |

Shader rule: only shaders whose terms are known and permissive. A shader credited "by VIDVOX" with no third-party
origin falls under the repository's MIT licence; a gl-transitions conversion must say `License: MIT` (or BSD) with its
author. Excluded: Shadertoy and glslsandbox ports (CC BY-NC-SA by default or unstated), "adapted from" a page with no
licence, anything without a credit.

Curated set (vendored in `third_party/isf/` with both licence files):

| Shader | Kind | Credit | Licence |
|---|---|---|---|
| Kaleidoscope | effect | VIDVOX | MIT (ISF-Files) |
| Twirl | effect | VIDVOX | MIT (ISF-Files) |
| Pixellate | effect | VIDVOX | MIT (ISF-Files) |
| Chromatic Aberration | effect | VIDVOX | MIT (ISF-Files) |
| RGB EQ | effect | VIDVOX | MIT (ISF-Files) |
| Posterize | effect | VIDVOX | MIT (ISF-Files) |
| Mirror | effect | VIDVOX | MIT (ISF-Files) |
| Quad Tile | effect | VIDVOX | MIT (ISF-Files) |
| Solarize | effect | VIDVOX | MIT (ISF-Files) |
| Triangle Mosaic | effect | VIDVOX | MIT (ISF-Files) |
| Spiral | generator | VIDVOX | MIT (ISF-Files) |
| Stripes | generator | VIDVOX | MIT (ISF-Files) |
| Sine Warp Gradient | generator | VIDVOX | MIT (ISF-Files) |
| Graph Paper | generator | VIDVOX | MIT (ISF-Files) |
| Worley Cells | generator | VIDVOX | MIT (ISF-Files) |
| Window Slice | transition | gre (gl-transitions) | MIT |
| Radial | transition | Xaychru, ported by gre (gl-transitions) | MIT |
| Random Squares | transition | gre (gl-transitions) | MIT |

Every other shader in ISF-Files 395072d, and why it is not vendored. The first groups are licence exclusions; the last three are licence-compatible but not picked.

- non-commercial ShareAlike (CC BY-NC-SA) (1): Boxinator.
- no credit, author and terms unknown (10): Audio Waveform Shape, Circle Trails, Layer Position, Poly Star, Power Warp, Quad Mask, Replicate Random, Replicate, Sliding Strips, Zooming Feedback.
- ported or adapted from another source with no licence stated (40): ASCII Art, Brick Pattern, Color Organ Polyphonic, Color Scales, Comet Tails, Convergence, Digital Clock, Dither-Bayer, Exposure Adjust, Frosted Glass, God Rays, Heart, Lens Flare, Line Group, Median, Neon, Optical Flow Distort, Optical Flow Generator, Random Characters, Saturation Bleed, Sepia Tone, Shape Morph Feedback Mask, Shape Morph Wrap, Smoke Screen, Star, Stylize Glitch, Thermal Camera, Tiny Date Time Overlay, Toon, Triangle, Trio Tone, Truchet Tile, VHS Glitch, Video Bricks, Y-C Time Blur, v002 Bleach Bypass, v002 Crosshatch, v002 Glitch Analog, v002 Technicolor, v002 Vignette.
- credited to someone other than Vidvox, no licence in the file (57): 3d Rotate, Auto Levels, Bright, Bump Distortion, CMYK Halftone-Lookaround, CMYK Halftone, Channel Slide, Chroma Zoom, Color Controls, Color Invert, Color Monochrome, Color Posterize, Crop and Feather, Cubic Warp, Deinterlace, Dirty Lens, Displace, Dual Side Scroller And Flip, Duotone, Edge Blur, False Color, Fast Blur, Gamma Correction, HSVtoRGB, HorizVertHold, Interlace Mirror, Interlace, Key Frame Artifacts, Layer Mask, Linear Gradient, Luminance Posterize, Maximum Component, Meta Image, Minimum Component, Multi Gradient, Multi Pass Gaussian Blur, RE RGB Gradient Generator, RGB Halftone-lookaround, RGB Halftone, RGB Trails 3.0, RGBtoHSV, Radial Gradient, Radial Spectrogram, Ripples, Side Scroller And Flip, Smudged Lens, Solid Color, Test Pattern Generator, VVMotionBlur 3.0, Vibrance, Waveform Displace, White Point Adjust, v002 Dilate, v002 Erode, v002 Light Leak, v002-CRT-Displacement, v002-CRT-Mask.
- gl-transitions, MIT with its author: licence fine, not picked (three transitions were enough; many sample away from their pixel) (63): Angular, Bounce, Bow Tie Horizontal, Bow Tie Vertical, Burn, Butterfly Wave Scrawler, Circle Crop, Circle Open, Circle, Color Phase, Colour Distance, Crazy Parametric Fun, CrossZoom, Crosshatch, Crosswarp, Directional Warp, Directional Wipe, Directional, Displacement, Doom Screen Transition, Doorway, Dreamy Zoom, Dreamy, Fade Color, Fade Gray Scale, Fade, Film Burn, Fly Eye, Glitch Displace, Glitch Memories, Grid Flip, Heart Transition, Hexagonalize, Kaleidoscope Transition, Linear Blur, Luma Transition, Luminance Melt, Morph, Mosaic, Multiply Blend, Perlin Transition, Pinwheel, Pixelize, Polar Function, Polka Dots Curtain, Ripple Transition, Rotate Scale Fade, Simple Zoom Transition, Squares Wire, Squeeze, Swap Transition, Swirl, TV Static, Undulating Burn Out, Water Drop, Wind, Window Blinds, Wipe Down, Wipe Left, Wipe Right, Wipe Up, Zoom In Circles, cube.
- gl-transitions, BSD with its author: licence fine, not picked (three transitions were enough; many sample away from their pixel) (2): Inverted Page Curl, Stereo Viewer.
- credited to Vidvox: left out for what the importer cannot run yet (multi-pass, persistent buffers, custom vertex shaders, audio, extra image inputs) or as not needed; each gets its own header check before it is added (136): Apply Alpha, Auto Color Tone, Auto Colors Histogram, Bad TV, Basic Shape, Bloom, Bordered Box, Broken LCD, Checkerboard, Chroma Desaturation Mask, Chroma Mask, Circle Splash Distortion, Circle Warp, Circle Wrap Distortion, Circular Feedback Mask, Circular Screen, City Lights, Collage, Color Bars, Color Blowout, Color History, Color Levels, Color Relookup, Color Replacement, Color Schemes, Color Test Grid, ConstantAlpha, Corner Color Tint, Corner Colors, Cursor Overlay, Cursor, Diagonal Blur, Diagonalize, Dilate-Fast, Dilate, Doodler Overlay, Doodler, Dot Screen, Double Vision, Duotone From Histogram, Echo Trace, Edge Blowout, Edge Distort, Edge Trace, Edges, Emboss, Erode-Fast, Erode, Etch-a-Sketch, FFT Color Lines, FFT Filled Waveform, FFT Spectrogram, FastMosh, Flip H, Flip V, Flipbook, Freeze Frame, Ghosting, Glitch Shifter, Gloom, Glow-Fast, Glow, Grid Warp, Hatch Blur, Highlighter Overlay, Histogram Viewer, Hyperspace, JPEG Block Corruption, Kaleidoscope Tile, LGG, Life, Line Screen, Lines, Long Exposure, Micro Buffer RGB, Micro Buffer, Mirror Edge, Motion Heat Map, Motion Mask, Multi Hue Shift, Multi-Pixellate, MultiFrame 2x2, MultiFrame 3x3, Night Vision, Noise Adapt, Noise Displace, Noise Pixellate, Noise, Pattern Glitch, Pixel Shifter, Poly Glitch, RGB Invert, RGB Strobe, RGBA Swap, Radial Replicate, Random Checkerboard, Random Freeze, Random Lines, Random Shape Blast, Random Shape, Random Squares Mask, Random Stripes, Resize Glitch, Ridgelines, Rotate, Set Alpha, Shake, Shape Mask, Sharpen Luminance, Sharpen RGB, Shockwave Pulse, Shockwave, Show Alpha, Simplex Noise, Sine Warp Tile, Sketch, Slit Scan Mask, Slit Scan, Soft Blur, Soft Flip, Sorting Smear, Sphere Map, Strobe, Time Glitch RGB, Trail Mask, Trapezoid Distortion, Triangle Warp, Triangles, Triple Rotate, Unsharp Mask, VU Meter, Vertex Manipulator, Vertical Tearing, Video Snake, XYZoom, Zoom.

### User ISF shaders (ticket 116)

The engine also reads ISF shaders from its machine's user folder (`<state>/isf-user/`). Those files are the user's own:
they are never vendored into this repository, never uploaded or synced by us (the media sync and the media upload do
not see the folder), and only go to the engine's own clients so a UI can show their parameters. Their licence is the
user's responsibility.

## Pro DJ Link references (tickets 89 and 112, checked 2026-10-18)

| Project | Licence | Compatible | Used how |
|---|---|---|---|
| Deep Symmetry dysentery and its protocol analysis (djl-analysis.deepsymmetry.org) | EPL-1.0 | no (EPL-1.0 is not GPL-compatible) | documentation only: packet layouts are re-described in our own code (`src/djlink/`, `tools/djlink-sim/djlink_packets.py`); no code copied |
| python-prodj-link (github.com/flesniak/python-prodj-link) | Apache-2.0 | yes | not used; no code read or copied |
| prolink-connect (github.com/EvanPurkhiser/prolink-connect) | MIT | yes | not used; no code read or copied |

The DJ link receiver and the simulator (`tools/djlink-sim`) are written from the documented layouts; nothing from these
projects is vendored, so ADR 0012's dependency tables do not change.

## HAP playback and transcode (ticket 94, checked 2026-10-18)

| Project | Licence | Compatible | Used how |
|---|---|---|---|
| Vidvox hap: the HAP frame specification and reference code (github.com/Vidvox/hap) | BSD-2-Clause | yes | the frame layout is re-implemented from the specification (`documentation/HapVideoDRAFT.md`) in `src/media/hap.cpp`; no reference code copied |
| Google snappy (github.com/google/snappy) | BSD-3-Clause | yes | the format is re-implemented from `format_description.txt` in `src/media/snappy.cpp` (decompressor and a simple compressor); no snappy code copied or linked |
| stb_dxt.h v1.12 (github.com/nothings/stb, commit 2c980bb) | MIT or public domain (dual) | yes | vendored in `third_party/stb/stb_dxt.h` with its licence text; the HAP encoder's BC1, BC3 and BC4 block compressor |
| Scaled YCoCg DXT5 (van Waveren and Castano, "Real-Time YCoCg-DXT Compression", NVIDIA 2007) | paper, no code | yes | the HAP Q encoder and the shader's conversion follow the paper's method; our own code |
| ffmpeg (Homebrew build, GPL-2.0-or-later with libx264) | GPL-2.0-or-later | yes, as a separate program | `tools/transcode-media.sh` and `nocturne-upload` run it to decode sources to raw RGBA for our own HAP encoder, and `tests/media/make_hap_goldens.sh` runs its HAP decoder for the reference pictures; nothing links it. Homebrew's ffmpeg has no HAP encoder (it needs libsnappy), so the encoder is ours |

No GPL code is linked or vendored for HAP.
