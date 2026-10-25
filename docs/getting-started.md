# Getting started

From a fresh clone to a show that plays itself, in about an hour. Every feature has more detail in
[`reference.md`](reference.md); the domain words (deck, column, layer, Autopilot, Section, Cue...) are in
[`CONTEXT.md`](../CONTEXT.md).

## 1. Build

You need a Mac on macOS 14 or later, the Xcode command line tools, CMake 3.24 or later and Homebrew `ffmpeg`.

```sh
xcode-select --install          # once, if you have no command line tools
brew install cmake ffmpeg
git clone https://github.com/Azylume/nocturne.git && cd nocturne
tools/make-signing-identity.sh  # once per Mac: a local "Nocturne Dev" signing identity
cmake -S . -B build
cmake --build build -j8         # the first build fetches JUCE and takes a while
```

The signing identity keeps macOS permissions (microphone, Documents) attached to the app across rebuilds. Without
it, macOS asks again after every build.

## 2. Get the example show's clips

The repo ships no footage. Save these free clips into `examples/first-show/` under exactly these names:

| file | source |
|---|---|
| `serpentine.mp4` | [Danse serpentine](https://archive.org/details/danse-serpentine-par-mme-bob-walter) (public domain) |
| `diagonale.mp4` | [Symphonie Diagonale](https://archive.org/details/SymphonieDiagonale) (public domain) |
| `opus3.mp4` | [Lichtspiel Opus III](https://archive.org/details/LichtspielOpusIII) (public domain) |
| `moon.mp4` | [NASA SVS 14992](https://commons.wikimedia.org/wiki/File:Lunar_Near_and_Far_Side_Phases_(SVS14992_-_Waxing_4K).webm) (public domain) |
| `beeple_scream.mp4`, `beeple_jahara.mp4`, `beeple_narrow_gultch.mp4` | [Beeple VJ loops](https://www.beeple-crap.com/vjloops) (CC BY, credit "Beeple") |

Nocturne plays H.264, HEVC, ProRes and HAP. The NASA clip is a `.webm`; convert it first:

```sh
ffmpeg -i Lunar_Near_and_Far_Side_Phases*.webm -c:v libx264 -pix_fmt yuv420p -an examples/first-show/moon.mp4
```

A show finds its media by path, then by name, then by content, so another clip saved under one of these names
plays in its place. A missing one leaves its cell empty.

## 3. Start the engine and the UI

The engine plays the show; the UI is only a window into it. Start the engine in a terminal:

```sh
build/nocturne --windowed --composition examples/first-show/show.json
```

`--windowed` shows the output in a normal window instead of taking over a display fullscreen. The example's
screen names a display this Mac has never seen, so it moves to one that is attached. The show starts
playing at once: the example has a column Autopilot that moves to the next column every 8 bars.

Then open the UI:

```sh
open "build/Nocturne UI.app"
```

It connects to the engine on this Mac by itself (port 7400). If the engine is not running yet, the status area
says so and the UI keeps retrying every second. Quitting or crashing the UI never stops the engine.

## 4. Fire clips and columns

The **Perform** tab shows the composition monitor (top left), the clip grid, and the effect chain and panes below.

- **Click a clip's thumbnail** to trigger it. **Click a clip's name** to select it without triggering.
- **Click a column header** (`Column 1`...) to trigger the whole column, every layer at once.
- Triggers land on the beat clock: **Q:** in the transport bar sets the quantisation (Bar, a beat, N bars, or off).
  A queued clip blinks until it lands; click it again to take the trigger back, or press Esc for all of them.
- The tempo is in the transport bar: type a BPM, use **TAP**, or **−**/**+** to nudge and **Resync** to put the
  downbeat on now.

## 5. Autopilot

Autopilot plays the show when nobody is at the controls.

- **Column Autopilot** (the **Autopilot** tab in the middle pane): steps through the deck's columns. The example
  show has it on, every 8 bars.
- **Layer Autopilot**: click the `auto` label on a layer's control (left of the grid) and pick **Next clip** or
  **Random clip** and the number of bars. The layer then triggers its own clips.
- To take over by hand, trigger anything. To stop it, pick **Autopilot off**.

Leave both on, walk away, and the show keeps moving on the beat.

## 6. Add an effect

- Open the **Effects** tab in the browser (bottom right). Our own effects are under **Ours**, the Classic pack
  under **Classic**, ISF shaders under **ISF**.
- **Drag an effect** onto a layer's control, a clip, the **Composition** header or the effect chain. A
  double-click adds it to the chain on show.
- The chain shows below the grid as cards. Drag a parameter row to change it, double-click its value to type
  one. Every card has dry/wet last, a bypass and a remove button.
- Open an effect in the Effects tab to see its presets under **Built-in**, **Imported** and **Mine**. Drag one
  onto a clip or a layer, or click it on a card's Presets box.
- Right-click a parameter and pick **Animate** to move it with a beat LFO or the kick, snare, hat or bass.

Every edit is one undo step: Cmd+Z and Cmd+Shift+Z, or Undo and Redo in the transport bar.

## 7. A screen on a second display

The engine started with one screen showing the whole composition on the first display. To add a second one:

1. List the displays the engine sees: `build/nocturne --displays`.
2. In the UI, open the **Map** tab and click **+ Screen** under the list of screens.
3. Select the new screen and pick the display in the inspector's **Output** menu.
4. **Test card** in the inspector helps you line up a projector; drag a slice's corners to corner-pin it.

Without `--windowed` the engine runs each display fullscreen, which is what you want at a show. Without a second
display, try `build/nocturne --virtual 2 --composition examples/first-show/show.json`: two simulated displays,
each a window on your screen.

## 8. Record a minute

Click **REC** in the transport bar. It lights while recording; click it again to stop. The movie goes to
`~/Movies/Nocturne` (HEVC by default; the file is in REC's tooltip). `build/nocturne-ctl record state` shows what
the next recording uses, and `record codec prores` or `record target screen:N` change it.

## Next

- [`cues.md`](cues.md): mark the Sections of a track live and let Cues fire columns on the drop.
- [`resolume-import.md`](resolume-import.md): bring a Resolume composition and its Advanced Output mapping.
- [`show-machine.md`](show-machine.md): run a Mac mini on its own for the night (supervisor, autostart, pairing).
- `build/nocturne-ctl` drives everything from a terminal; `build/nocturne-ctl watch` prints every change.
