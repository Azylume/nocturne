# The engine and UI are AGPL-3.0-or-later, with contributions under the DCO

Everything in this repository, the engine and the UI, will be licensed under the GNU Affero General Public License,
version 3 or (at your option) any later version (`AGPL-3.0-or-later`). Anyone may use, change and sell it; nobody may
ever make it closed; running it as a network service counts as distribution, so users of a hosted copy get the source.

- **Contributions under the DCO, no CLA.** Every commit carries `Signed-off-by:` (Developer Certificate of Origin).
  Contributors keep their copyright, so nobody, the original author included, can later relicense the code as closed.
- **The cloud services are separate.** The flyer and composition generator, the media server and conversion, a cloud
  Director, the client pipeline and guest clips are separate, proprietary codebases that talk to the engine only
  through its protocol.
- **Not published yet.** The repository goes public only when the maintainer explicitly says so. Nobody changes its visibility
  or pushes it to a public remote without his explicit go. Until then nothing changes except one rule: every new
  dependency must be AGPL-3.0-compatible (checked against `docs/licence-notes.md`).
- **Why "or later":** JUCE offers its modules under the AGPLv3; our own code under "or later" combines with it as AGPLv3
  today and keeps the door open to a future AGPL version without asking every contributor.

## Current dependencies (2026-10-18)

| Dependency | Where | Licence | Compatible |
|---|---|---|---|
| JUCE 8.0.9 | UI, FetchContent | AGPLv3 / commercial dual licence | yes (under AGPLv3) |
| metal-cpp | third_party/metal-cpp | Apache-2.0 | yes |
| nlohmann/json (also our MessagePack encoding) | third_party/nlohmann | MIT | yes |
| doctest | third_party/doctest, tests only | MIT | yes |
| Tracy v0.14.1 | optional profiler build (`-DNOCTURNE_TRACY=ON`), FetchContent; not in the default build | BSD-3-Clause | yes |
| glslang vulkan-sdk-1.4.363.0 | engine, FetchContent: ISF GLSL to SPIR-V | BSD-3 / BSD-2 / MIT / Apache-2.0 (GPL-3 with the Bison exception covers only Bison output) | yes |
| SPIRV-Cross vulkan-sdk-1.4.363.0 | engine, FetchContent: SPIR-V to MSL | Apache-2.0 | yes |
| Curated ISF shaders (ISF-Files, gl-transitions) | third_party/isf, built into the program | MIT, per shader in `docs/licence-notes.md` | yes |
| AMD FidelityFX Super Resolution 1 (v1.20210629, EASU and RCAS) | shaders ported to Metal in src/engine/fsr_upscaler.cpp, AMD's notice kept in that file | MIT | yes |
| Apple frameworks (Metal, AVFoundation, VideoToolbox, CoreAudio, ...) | system | system libraries | yes (system library exception) |
| stb_dxt.h v1.12 | third_party/stb, the HAP encoder's BC block compressor | MIT / public domain (dual) | yes |
| HAP frame format (Vidvox) and snappy format (Google) | re-implemented from their specifications in src/media/hap.cpp and src/media/snappy.cpp; no code copied | BSD-2-Clause, BSD-3-Clause (the references) | yes |
| ffmpeg, ffprobe (Homebrew, with libx264) | tools/transcode-media.sh and nocturne-upload run them as separate programs; nothing links them | GPL-2.0-or-later build | yes (not linked; GPL-2.0+ allows GPL-3.0 anyway) |

No incompatible dependency was found.

## Planned dependencies

None at the moment.

GPL-3.0 libraries are licence-compatible with AGPL-3.0 (its section 13); GPL-2.0-only libraries are not. The earlier
rule "no GPL code in the engine" was made when a permissive licence was still possible; whether to relax it for
GPL-3.0 libraries (aubio, Essentia) is the maintainer's call.
