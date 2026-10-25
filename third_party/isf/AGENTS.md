# third_party/isf/AGENTS.md

Curated ISF shaders, built into the engine. CMake embeds every `third_party/isf/*/*.fs` at build time
(`src/effects/isf_sources.cpp.in`); users' own shaders are read at runtime from `<state>/isf-user/` instead
(`nocturne-ctl isf list`, `isf rescan`) and never belong here.

## Rules

- Files are vendored **verbatim** next to their source's licence (`vidvox/LICENSE`, `gl-transitions/LICENSE` and
  `LICENSE-ISF-Files`). Never edit a `.fs` here; a fix goes into the importer, `src/effects/isf.cpp`.
- Only shaders with known, permissive terms and a credit: follow the "Shader rule" in `docs/licence-notes.md`.
  No Shadertoy or glslsandbox ports, nothing "adapted from" a page without a licence. Unsure: leave it out and ask.
- The importer cannot run multi-pass shaders, persistent buffers, imported images, audio inputs or a custom vertex
  shader. Multi-pass shaders, imported images and unsupported input types (audio among them) are refused with an
  error. **A shader that ships a `.vs` vertex shader is not
  refused: it renders wrong** (its coordinates never arrive), so check the source folder for a `.vs` file first.
- A new transition needs a new `TransitionMode` (`src/document/composition.hpp`) and a file-format name: that changes
  the composition format, so ask first.

## Fusion: check it, do not assume it

An effect fuses into the pass before it (ADR 0009) only when every read of `inputImage` is one of
`IMG_THIS_PIXEL(inputImage)`, `IMG_THIS_NORM_PIXEL(inputImage)` or `IMG_NORM_PIXEL(inputImage, isf_FragNormCoord)`
(`effectPointwise` in `src/effects/isf.cpp`). Any other coordinate makes it a pass of its own.

```glsl
gl_FragColor = IMG_THIS_PIXEL(inputImage) * tint;              // fuses
gl_FragColor = IMG_NORM_PIXEL(inputImage, warped(isf_FragNormCoord)); // own pass
```

Say in the PR which of your shaders fuse and which do not, from that check.

## Adding a shader

1. Copy the file unchanged into `third_party/isf/<source>/`.
2. Move it in `docs/licence-notes.md` from the left-out list to the curated table. The number in the group's
   parentheses is kept by hand: count the names after your edit.
3. Effect or generator: add a `Look` with fixed parameters to `kEffects` or `kGenerators` in
   `tests/isf_engine_test.cpp`. "every built-in ISF shader loads" fails until you do.
4. Make its golden and look at it:
   `NOCTURNE_UPDATE_GOLDEN=1 build/nocturne_tests --test-case="golden frames of every built-in ISF*"`, then open
   `tests/golden/isf<Name>.png`. Nearly black, flat or the plain test card means the shader is broken here.
   `git status tests/golden` must show only your new file.
5. `build/nocturne_tests --test-case="*ISF*"` passes without the update variable.
