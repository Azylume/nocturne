# Built-in effects to ISF (issue #34)

Inventory of every native effect in `src/effects/effect_registry.cpp`, how it is built, and what moving it to an ISF
shader in `third_party/isf/` takes. Stage kinds are ADR 0009's: P pointwise, R remap, N neighbourhood, M multi-pass.
"Goldens" are the cases that look at the picture; "none" means no picture is compared today.

## What an ISF port needs from the loader

| Feature the loader lacks | Why a port needs it |
|---|---|
| F1 parameter metadata (**done**) | `ParamDef` has a unit, `wraps`, a group and shortened labels; an ISF input carries none, so a port would show `0.25` for `25%` and stop a hue from wrapping under a modulator. The `NOCTURNE` header's `PARAMS` now carries them (below) |
| F2 the stage's Opacity (**done**) | the Classic wrapper's Add mixer (`addMixer`) reads the stage's `mix`, which an ISF shader cannot see. A built-in shader's `NOCTURNE` header can now ask for `STAGE_OPACITY` (below) |
| F3 driven parameters | `ParamDrive::Phase`, `Beats` and `Audio`: a rate integrated over time, beats counted by the beat clock, an audio feature's level |
| F4 remap stage kind | a shader that reads its input at one other coordinate fuses as `Remap`; the importer makes it a neighbourhood pass of its own |
| F5 CPU-derived values | `EffectDef::derive`: values worked out once a frame on the CPU and read by every pixel |
| F6 multi-pass, persistent 16F targets | `SidePass`: history and feedback |

## Inventory

### Ours (13)

| Type | How | Kind | Parameters | Goldens | Class |
|---|---|---|---|---|---|
| transform | one remap body | R | positionX, positionY, scale, rotation | `test_card_warp`, `transform_colorize`, chain goldens | needs F4 |
| colorize | one pointwise body | P | hue, saturation, brightness, invert, alpha | `colorize_brightness`, `edge_detection`, `transform_colorize`, chain goldens | **ported** |
| edgeDetection | Sobel over 3x3 | N | multiplier | `edge_detection` | **ported** |
| stripper | pointwise, variants `cross` and `live` | P | 22 (number, direction, speed, spread, width, feather, ..., audioBand, evolve) | `stripper`, `stripper_add`, `stripper_spread`, `stripper_step`, `stripper_swing` | needs F3 |
| trails | side pass, persistent 16F history | M | feedback | `trails` | needs F6 |
| reveal | pointwise, hashed cell rank | P | progress, columns, rows, gap, seed, reshuffle, softness, black, evolve | `reveal` | needs F3 |
| slicer | 6 layouts, 10 styles, CPU-derived cells | N | 24 | none | keep native: CPU-derived cell table (F5) shared with the C++ layout code, no goldens |
| thermal | pointwise, shared style globals | P | hue, colour | none | needs F1 |
| grain | pointwise, hashed specks | P | none | none before this PR (`grain`, added) | **ported** |
| letters | glyph grid, samples the picture per cell | N | rows | none | needs F1 |
| dots | halftone, samples per cell | N | rows, hue, tint | none | needs F1 |
| duotone | pointwise | P | hue | none | needs F1 |
| outline | 8-tap threshold edge | N | hue | none | needs F1 |

### Classic (17)

| Type | How | Kind | Parameters | Goldens | Class |
|---|---|---|---|---|---|
| classicStripper | pointwise mask, CPU-derived strips, `resample` variant | P | 16 | `classic_stripper*` (7) | needs F1, F2, F3, F5 |
| classicColorize | pointwise | P | 11 | `classic_colorize`, `_add`, `_palette` | needs F2 |
| classicShiftRgb | per-channel remap reads | N | 7 | `classic_shift_rgb`, `_zoom` | needs F2 |
| classicMirror | remap | R | 7 | `classic_mirror` | needs F2, F4 |
| classicKaleidoscope | remap | R | 6 | `classic_kaleidoscope` | needs F2, F4 |
| classicTwitch | 3x3 blur, shifts, per-frame hash | N | 16 | `classic_twitch` | needs F1, F2 |
| classicAutoMask | pointwise | P | channel, contrast, invert | `classic_auto_mask` | **ported** |
| classicBrightnessContrast | pointwise | P | brightness, contrast, blendMode | `classic_brightness_contrast` | **ported** |
| classicLevels | pointwise | P | base, midpoint, ratio, blendMode | `classic_levels` | **ported** |
| classicThreshold | pointwise | P | 12 | `classic_threshold` | **ported** |
| classicHueRotate | pointwise, HSL | P | hueScale, hueRotate, satScale, blendMode | `classic_hue_rotate`, `_add` | **ported** |
| classicMirrorQuad | remap | R | flipX, flipY, blendMode | `classic_mirror_quad` | needs F2, F4 |
| classicPolarKaleido | remap | R | rings, parts, angle, blendMode | `classic_polar_kaleido` | needs F2, F4 |
| classicTrails | side pass, persistent history | M | feedback, mode, blendMode | `classic_trails` | needs F2, F6 |
| classicStrobe | pointwise | P | 8 | `classic_strobe_fade` | needs F2 |
| classicShiftGlitch | remap, CPU-hashed bands | R | 7 | `classic_shift_glitch` | needs F2, F4, F5 |
| classicEdgeDetection | Sobel or min filter | N | 9 | `classic_edge_detection`, `_color` | needs F2 |

### Generators (8)

| Type | How | Parameters | Goldens | Class |
|---|---|---|---|---|
| blank | constant | none | `generator_blank` | **ported** |
| solid | constant colour | red, green, blue | `generator_solid` | **ported** |
| strobe | beat clock duty | rate, duty, colour | `generator_strobe` | needs F1, F3 |
| lines | box-filtered lines | count, width, angle, speed, softness, colour | `generator_lines` | needs F1, F3 |
| checker | rotated checker | count, angle, speed, colour | `generator_checker` | needs F1, F3 |
| rings | box-filtered rings | count, width, speed, softness, x, y, colour | `generator_rings` | needs F1, F3 |
| gradient | mirrored ramp | angle, scale, speed, two colours | `generator_gradient` | needs F1, F3 |
| noise | value noise | scale, speed, contrast, colour | `generator_noise` | needs F1, F3 |

Totals: 38 types, 10 ports, 27 need a loader feature, 1 stays native for now. F1 and F2 are done; thermal, letters, dots, duotone and outline need F1 and nothing else (none has a golden).

## How a ported shader keeps its identity

A built-in shader whose header has `"NOCTURNE": {"TYPE": "grain", "CATEGORY": ""}` registers under that type and
category instead of `isf<FileName>` and `ISF`, so saved compositions, presets, macros and modulators that name the
native effect load unchanged and no alias table is needed. Parameter names, ranges and defaults are the native ones.
The header is ignored in a user shader, so a file in the user folder cannot replace a built-in effect.
Ported shaders live in `third_party/isf/nocturne/`: our own work, AGPL-3.0-or-later like the rest of the repository.
Ported effects register after the remaining native ones, so they list after them in their group.

## Parameter metadata (F1)

A built-in shader's `NOCTURNE` header may carry `"PARAMS": {"<input name>": {...}}` with `UNIT` (`SUFFIX`, `SCALE`,
`DECIMALS`), `WRAPS`, `GROUP` and `FULL_LABEL`; the input's own `LABEL` is the short label. A name that is not an input
is an error. A user shader's `NOCTURNE` header, `PARAMS` included, is ignored. Colorize, Edge Detection and Solid are
ported with it; a test compares their parameter specs to the native ones.

## The stage's Opacity (F2)

A built-in shader's `NOCTURNE` header may carry `"STAGE_OPACITY": true`. The shader then reads `STAGE_OPACITY`, a float
global holding the stage's own Opacity (`mix`), the value the native Add mixer reads. It is not an input, so it adds no
parameter; the compiler passes it after the inputs and a user shader that names it does not translate. Classic
Brightness Contrast, Levels, Threshold and Hue Rotate are ported with it; a test compares their parameter specs to the
native ones.
