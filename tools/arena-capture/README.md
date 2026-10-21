# Reference capture: phase 11 (ticket 46) and MovementType option 2 (ticket 101)

`arena_capture.py` drives the reference app's local REST API (`http://127.0.0.1:3500/api/v1`) on a scratch layer it
adds at the end of the open composition and deletes at the end. It never saves the composition: any request path
containing "save" is refused, and the only composition-wide change, the tempo for `tempo-card`, is put back in a
`finally`. Frames land in work directories laid out the way `tools/classic_parity.py` and `tests/classic_test.cpp`
read them.

## Run it

1. Open the reference app with any composition. Turn the webserver on in Preferences > Webserver (port 3500).
   Close other heavy jobs. The capture takes about 10-15 minutes.
2. Check the plan, then the requests, without touching the app:

   ```
   python3 tools/arena-capture/arena_capture.py --list
   python3 tools/arena-capture/arena_capture.py --dry-run
   ```

3. Capture everything, under `nice`, writing to `~/Work/azylume/parity-captures/p11/`:

   ```
   nice -n 15 python3 tools/arena-capture/arena_capture.py --arena-fps 60
   ```

   `--arena-fps` is the FPS the reference app shows in its status bar while capturing. Read it and pass the number you
   see. It is stored in `frames.json` and `capture-log.json`, because the Trails decay is per rendered frame. Use
   `--only NAME` (repeatable) to capture a single work directory again, for example `--only stripper-uv`.
   `--from DIR` points at the phase 6 captures. The test card, `uv.png` and `motion.mp4` are copied from there. The
   Bloom inputs are generated.
4. Optional, ticket 101 at the display rate: add `--record-pause`. Before each of the 4 Stripper motion cases the
   script stops and waits for Enter. In the reference app's Record panel, set Source to the scratch layer (the last
   layer) and start recording. Press Enter, and stop recording after 6 s. Recordings go to the folder set under
   Preferences > Recording (default `Documents/Resolume Arena/Recorded`), as QuickTime DXV3
   ([Recording manual](https://resolume.com/support/en/7/recording)). The REST API has no recording endpoint, so this
   is the only way to get every output frame at 60 fps.

## What it captures

| Work directory | Input | Cases | Kind |
|---|---|---|---|
| `glitch-card` | test card (`input.png`) | Shift Glitch default; all at 1 (Frequency, Vertical, Horizontal, Size 1, Black BG on) | 4 s sequences, 5 fresh instances, up to 20/s |
| `glitch-uv` | `uv.png` | the same two | 4 s, 1 instance |
| `tempo-card` | test card, tempo **90 BPM** | Shift Glitch Frequency 0, 0.5, 0.75; Strobe Frequency 0.5, 0.75, 0.875 | 6 s, 1 instance |
| `trails-motion` | `motion.mp4` | fresh instance each: Luma 0.5, Luma 0.9, Feedback 1 (Alpha), Lighten 0.9, defaults | 8 s at up to 5/s, so the reference keeps its frame rate |
| `bloom-flat` | grey squares 0.5 (600 px), 0.3, 0.7, 1.0 (300 px) | Amount x5, Iterations 1-5 at Amount 0.1 / 0.25, Knee x5 at Threshold 0.9 / 0.6 and Amount 0.1 / 1, Threshold 0.1-0.9 at Knee 0 / 0.1 | 53 stills |
| `bloom-ramp` | luma ramp, 64 px black above and below | Knee and Threshold as above | 38 stills |
| `bloom-dots` | 1 px and 2x2 white dots, 400 px apart | Amount, Iterations, Size x6 and Gaussian x5 at Iterations 1 / 2 (Amount 0.1) | 37 stills |
| `bloom-lines1` | 1 px white lines, 400 px apart | as dots | 37 stills |
| `bloom-lines4` | 4 px white lines | Amount x5 | 5 stills |
| `bloom-alphaflat` | the 0.5 square at alpha 0.5 on transparent | defaults; Threshold 0.3 | 2 stills |
| `stripper-uv` | `uv.png` | Number 0.1, GlobalWidth 0.1, Mode 0, Direction 0, Feather 0, Speed 0 as the base: MovementType 0.70-0.80 by 0.01 (stills); Feather 0, 0.25, 0.5, 0.75, 1 (stills); MovementType 1 at Speed 0, 0.02, 0.158 and the Both Ways preset (Add mixer) | stills; 5 s sequences as fast as snapshots come |

All values not listed are the effect's defaults, sent explicitly (see `--list`). Every output is 1920x1080 RGBA, the
layer monitor's snapshot. Static cases are `WORK/ref/<case>.png`. Sequences are `WORK/seq/<case>.<instance>/NNN.png`
with `frames.json` (ask and arrival time per frame, tempo, `arenaFps`, marker t = 0 when the effect leaves bypass). Every
work directory also gets `cases.json`, `ref/input.png` (the bare input) and `capture-log.json`. That file lists the
stills that never settled: for the MovementType sweep, that list *is* the option boundary.

## Check by eye

- After the run, the composition has its original layers and the tempo shows its old value. Do not save it.
- `ref/input.png` in each directory shows the input, not black and not the logo overlay.
- `bloom-*/ref/*amount.0.png` shows a faint glow. Threshold 0.1 on `bloom-ramp` lights most of the ramp.
- `stripper-uv/capture-log.json`: `unsettled` should start somewhere between `mt.0.75` and `mt.0.80`.
- `trails-motion/seq/*feedback.1.0/` frames all look the same. `tempo-card` Strobe flashes are slower than in phase 6.
- Recordings (if made) show strips jumping. Note the file name per case.

## Import

1. Per sequence directory, run the timeline (needs Pillow and numpy), then the Trails clip phase:

   ```
   for w in glitch-card glitch-uv tempo-card trails-motion stripper-uv; do
     python3 tools/classic_parity.py timeline ~/Work/azylume/parity-captures/p11/$w; done
   python3 tools/classic_parity.py trails-phase ~/Work/azylume/parity-captures/p11/trails-motion
   ```

2. Score with the parity test cases, one work directory at a time:
   `NOCTURNE_PARITY_DIR=<work dir>` runs `parity frames of Classic effects` (stills) and
   `parity sequences of Classic effects` (sequences) in `tests/classic_test.cpp`.
   - `glitch-*`, `tempo-card`, `trails-motion` and `stripper-uv` work as they are: the test card, `uv.png` and
     `motion.mp4` are inputs the tests already draw.
   - The six Bloom inputs are not test inputs yet. Before scoring them, add each one to `kProbes` in
     `tests/classic_test.cpp` (the same way as `uv.png` and `alpha.png`). Without that, the test draws the test card
     and the scores are meaningless.
   - `tempo-card` is at 90 BPM. Our harness runs at 120, so compare the measured periods by hand, or set the tempo in
     the test, before you trust a score.
3. Recordings for ticket 101: convert each one to numbered PNGs next to its sequence, for example
   `ffmpeg -i rec.mov stripper-uv/seq/classicStripper.mt2.speed.0.rec/%04d.png`. The first frame with strips is
   t = 0.
4. Raw frames stay outside the repo, as in phase 6. Only scores and findings go into tickets 46 and 101 and
   `docs/research/classic-parity.json`.

## REST endpoints used

Public reference: [Arena & Avenue REST API](https://resolume.com/docs/restapi/)
([swagger.yaml](https://resolume.com/docs/restapi/swagger.yaml)).

- `GET /product`, `GET /composition` (layers, `tempocontroller.tempo`)
- `POST /composition/layers/add`, `GET` and `DELETE /composition/layers/by-id/{layer-id}`
- `POST /composition/layers/{layer-index}/clips/{clip-index}/open` (body `file:///...`) and `.../connect`
- `POST /composition/layers/by-id/{layer-id}/effects/video/add` (body `effect:///video/<name>`),
  `DELETE /composition/layers/by-id/{layer-id}/effects/video/{offset}`
- `PUT /parameter/by-id/{parameter-id}` with `{"value": ...}`
- `GET /composition/monitors` and `GET /composition/monitors/{id}/snapshot.png`: the public swagger lists a
  `monitors` tag ("List available render targets and capture snapshots") but not these paths. They are taken from
  `tools/classic_parity.py`, which used them on this machine in phases 3-6. The public clip `thumbnail` endpoint shows
  the source clip without effects, so it is not a substitute.
