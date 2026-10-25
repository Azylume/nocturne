# Bringing your Resolume show

Nocturne opens a Resolume Arena composition (`.avc`) and its Advanced Output mapping, and writes a report of
everything it converted, approximated or left out. It is a translation, not a copy: read the report before the
gig. The details are in [`reference.md`](reference.md), under Resolume import.

You need a built Nocturne ([`getting-started.md`](getting-started.md)) and Homebrew `ffmpeg`. Resolume itself
is not needed; the import reads its files.

## 1. Prepare the media

Nocturne plays H.264, HEVC, ProRes and HAP, up to what the Mac decodes. It does not play DXV.
`tools/transcode-media.sh` prepares everything a composition uses, once, into one folder:

```sh
tools/transcode-media.sh "$HOME/Documents/Resolume Arena/Compositions/Friday.avc" ~/Nocturne/media/friday
```

- DXV, other codecs macOS cannot decode, and anything above 1080p become H.264 1080p (`STEM.mp4`) with a keyframe
  every second, so loops, Random Jump and reverse stay smooth.
- Still images become a 4 s H.264 loop over black (`STEM.mp4`); H.264 has no alpha, so transparency turns black.
- H.264, HEVC, ProRes and HAP files up to 1080p are copied as they are, as APFS clones that take no extra space.
- Originals are never touched. It runs ffmpeg at nice 15, one file at a time, and a rerun only does what is
  missing, so you can stop it and start it again.
- `HAP=hap` (or `hap-alpha`, `hap-q`, `hap-q-alpha`) makes HAP movies (`STEM.mov`) instead, with our own encoder.
  They are several times larger but need no hardware decoder, and the alpha variants keep a still's transparency.
  `CRF` (default 18) and `PRESET` (default `slow`) tune the H.264 encode.

## 2. Import the composition

```sh
build/nocturne-import "$HOME/Documents/Resolume Arena/Compositions/Friday.avc" ~/Nocturne/shows/friday.json \
    --media ~/Nocturne/media/friday \
    --mapping "path/to/your Advanced Output preset.xml"
```

- `--media DIR` is where step 1 put the media. Each clip is found there by its file name, else by its stem with
  `.mp4` or `.mov`. DIR also becomes the show's media root, so the show and its folder move together.
- `--mapping FILE.xml` brings the screens and slices: a preset saved from Resolume's Advanced Output, or its
  `AdvancedOutput.xml`. Without it the show gets one screen with the whole composition.
- `--displays ID,ID,...` puts the mapping's screens on these displays, in order (`build/nocturne --displays` lists
  them). By default each screen keeps the output device Resolume had, and the engine moves a screen whose display
  this Mac has never seen to a free one.
- `--start-column N` is the column every layer plays when the show loads (default 1, 0 plays nothing).
- `--keep-clip-chains` keeps an effect chain that every clip of a layer carries alike on each clip. By default it
  runs once on the layer instead, which compiles once instead of once per clip.

Open the result in the UI (**Open** in the transport bar), or start the engine on it:
`build/nocturne --composition ~/Nocturne/shows/friday.json`.

## 3. Read the report

Next to the show the import writes `friday.report.md` (`--report FILE` puts it elsewhere). It has these sections:

| section | what it lists |
|---|---|
| **Imported** | What came across: layers, decks, columns, clips, effects, screens. |
| **Approximated** | Imported with a conversion that may not look exactly as in Resolume, each with the conversion. |
| **Not imported** | Left out of the Nocturne composition, each with where it was (deck, layer, column, effect...). |
| **Ignored** | Elements that do not change the picture. |
| **Media** | How many media files the composition references, and how many were found in `--media`. |

A conversion that every clip of a layer shares is listed once, with the number of clips. Go through
**Approximated** and **Not imported**, and fix what matters in the UI. A clip not found in the media folder is
listed under **Not imported** and its cell keeps the original path: put the file in the media folder and import
again, or drag the right file onto the cell.

## What comes across

**The composition.** Layers, decks, columns, clips (by name in the media folder), blend modes, opacity,
transport, Random Jump, BPM and Beat Snap. FFT-driven opacity becomes an audio modulator on the layer's opacity.

**Effects.** Transform, Colorize, Edge Detection, Stripper and Trails map with their parameters onto Nocturne's
own effects. Clip effects land on their clips, the composition's on the composition. Bloom and other effects
without a counterpart are reported as not imported. The Classic effect pack in the Effects tab covers more of the
familiar effects, and can import that app's presets from the same Mac.

**The mapping.** Screens and slices: input rects, Bezier warp grids (point mode and subdivisions), the homography
as the corner pin, soft edges (the setup's power, luminance and gamma; widths from where soft-edged slices' input
rects overlap), screen and slice colour, and each screen's output device as its display.

**Approximated in the mapping.** Nocturne's input rects cannot turn. A turned one becomes its bounding box, with
the warp grid turned (quarter turns), or with an inverted slice mask and a re-solved corner pin (flat slices),
and soft edge off.

**Left out.** DMX screens, elements Nocturne does not know, and parameters off their defaults that it cannot
convert, each reported with where it was.

## A mapping on its own

To bring only an Advanced Output setup, for example to reuse a venue's projector mapping with a show built in
Nocturne:

```sh
build/nocturne-import "venue A.xml" "venue B.xml" ~/Nocturne/mappings
```

Each `.xml` (or each one in a folder you name) becomes `NAME.json`, a composition file holding only its screens,
with `NAME.report.md` beside it.

## Before the night

- Play the show through on the machine that will run it, with its real displays: `build/nocturne --composition
  ~/Nocturne/shows/friday.json`, then the UI's **Map** tab to check each screen against a projector (its **Test
  card** helps).
- [`show-machine.md`](show-machine.md) sets up a Mac mini to run on its own for the night.
