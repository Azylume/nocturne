#include "audio/opus_codec.hpp"
#include "document/composition_file.hpp"
#include "document/lineup.hpp"
#include "document/modulation_edits.hpp"
#include "document/palette.hpp"
#include "document/text_edits.hpp"
#include "media/keep_local.hpp"
#include "media/media_library.hpp"
#include "net/lan.hpp"
#include "net/pairing.hpp"
#include "net/websocket.hpp"
#include "output/return_video.hpp"
#include "platform/app_paths.hpp"
#include "protocol/bundle_push.hpp"
#include "protocol/return_stream.hpp"
#include "sync/discovery.hpp"
#include "venues/gig.hpp"
#include "upload/media_fetch.hpp"
#include "upload/media_upload.hpp"
#include "venues/venue_monitor.hpp"
#include "protocol/document_mirror.hpp"
#include "protocol/prepare.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {

constexpr const char* kUsage = R"(usage: nocturne-ctl [options] [command]
  --host H      engine host name or address, e.g. the Mini's Tailscale name (default 127.0.0.1)
  --port N      engine port (default 7400)
  --token T     the engine machine's pairing token, needed when the engine is on another machine (default:
                NOCTURNE_TOKEN in the environment, else this machine's own token when --host is this machine)
  --json        talk JSON instead of MessagePack
commands:
  watch                  print the snapshot and every change until interrupted (default)
  watch OUTPUT FILE [SECONDS]       write an output's return video (its display id or screen id) to FILE as raw
                                    H.264 from its first key frame, for SECONDS (default 10), and print its bit rate
  snapshot               print the snapshot and exit
  opacity LAYER VALUE    set a layer's opacity (0..1); LAYER 0 is the composition's master opacity
  undo | redo
  add PATH               add a file to the engine machine's media library (absolute path on that machine)
  library                list the media library
  clip LAYER HASH NAME   put the library file with that hash on a layer
  play LAYER PATH        add, then put it on the layer
  clear LAYER            remove a layer's clip
  rename KIND ID NAME    rename a deck, layer, column, screen, slice, mask or macro (undoable); NAME may hold
                         spaces, quoted or not
  addlayer [PARENT [INDEX]]         add a layer on top of the composition, or of group PARENT, or at INDEX
  addgroup [INDEX]                  add an empty group
  rmlayer LAYER                     remove a layer or a group with its layers
  movelayer LAYER PARENT INDEX      move a layer to INDEX within group PARENT (0: the composition)
  blend LAYER MODE       normal add screen multiply difference subtract exclusion lighten darken overlay
                         hardLight softLight dodge burn
  bypass LAYER on|off
  solo LAYER on|off
  fullres LAYER on|off              at a render scale below 100 %, the layer and those above it draw at output size
  bpm VALUE              set the composition tempo (20..500)
  sync LAYER MODE [COLUMN]          clip sync mode: timeline | bpm
  playmode LAYER MODE [COLUMN]      clip play mode: loop | bounce | reverse | randomJump | onceClear | onceHold
  jump LAYER INTERVAL DISTANCE [COLUMN]
                         Random Jump every INTERVAL beats (1/8..8, e.g. 0.25) by up to DISTANCE beats
  beats LAYER N|auto [COLUMN]       clip length in beats when BPM-synced
  speed LAYER SPEED [COLUMN]        how fast the clip plays (0..4, 1 as is)
  inout LAYER IN OUT [COLUMN]       the part of the clip it plays (0..1 of its frames, IN before OUT)
                         (these act on the clip the layer plays, or on the one in COLUMN)
  cliptrigger LAYER COLUMN off|beat|bar|NBARS|BEATS|default
                         where the clip's triggers land, or the composition's quantisation
  tap [COUNT SECONDS]    tap tempo once, or COUNT times SECONDS apart
  nudge PERCENT          bend the tempo by PERCENT (-50..50) until nudge 0
  resync                 the downbeat is now
  scrub LAYER POSITION [TO SECONDS]
                         move a clip to POSITION (0..1), or sweep it to TO over SECONDS at 60 Hz
  clock                  print the beat clock and clip positions
  clippreview LAYER COLUMN SECONDS  watch the moving preview of the clip in that cell, with its effects, as a UI on
                                    this machine does for its selected clip, and print the pictures
                                    received each 10 s; fails while clip previews are off
  clippreviews [on|off]             turn live clip previews on or off (machine-local; they cost the engine machine
                                    keyframe decodes and some GPU), or print whether they are on
  fx LAYER TYPE [NAME=VALUE ...]    add an effect at the end of a layer's chain (transform, colorize,
                                    edgeDetection, stripper, trails, ...); LAYER 0 is the composition
  clipfx LAYER COLUMN TYPE [NAME=VALUE ...]
                                    add an effect at the end of the chain of the clip in that cell
  fxrm EFFECT                       remove an effect by id, from whichever chain holds it
  fxmove EFFECT INDEX               move an effect to a position in its chain
  fxbypass EFFECT on|off            bypass an effect (dry/wet forced to 0, side passes skipped; nothing recompiles)
  fxpilot EFFECT next|random|off [BARS]
                                    the effect's Autopilot through its steps every BARS bars (1..64), on the bar
  fxhold EFFECT BARS                a step trigger holds the effect's Autopilot for BARS bars (0: one step)
  fxtransition EFFECT BEATS         the glide into a step (0..64 beats, 0 cuts; choices switch halfway)
  fxstep EFFECT N                   live: move the effect to its step N (1-based), on the composition's quantisation
  fxsteps EFFECT                    list the effect's steps with the parameters each sets
  fxsteps EFFECT add [NAME]         add a step of the effect's current values
  fxsteps EFFECT rm N | move N TO   remove step N, or move it to position TO
  sensitivity SOURCE VALUE          how hard kick, snare, hat, bass, lowBand, midBand, highBand or lfo drives what it
                                    modulates (0..4, 1 as is)
  bands LOWMID MIDHIGH              the edges in Hz of the lowBand, midBand and highBand audio features (40..12000,
                                    default 150 2000); the levels glide to them
  param EFFECT NAME VALUE           set an effect parameter
  animate EFFECT NAME FROM TO SECONDS
                         sweep a parameter at 60 Hz, one edit per step
  token [STATE]          print this machine's pairing token, from STATE/token (default ~/Library/Application
                         Support/Nocturne/State), made when missing; clients on other machines present it
  nodes                  list the Nocturne engines Bonjour finds on the network, with their role
  venues                 list the venues the UI's menu shows (Tailscale devices with the venue tag, and Bonjour), each
                         with its dot (ok, warning, fault, offline), build and any build mismatch with this binary
  node status|promote|demote|adopt
                         Node Sync status; promote makes a node the main engine, demote makes a main engine a node,
                         adopt makes a node that kept its show take the main engine's empty composition after all
  machine                the engine machine's health, as the UI's Machines tab shows it: render fps, GPU ms, frame
                         guard, CPU speed limit and temperature, version, next Cue, every output's state, audio, clock
  machine testcard on|off           the test card on every screen
  machine identify [SECONDS]        every output shows its screen's name and display large (default 3 s)
  machine unmirror                  end mirroring between displays that carry different screens now
  machine stop                      the supervisor stops the engine and exits without restarting it (SIGTERM does
                                    the same); a show machine then needs tools/autostart.sh restart to run again
  machine restart|safemode          the supervisor starts the engine again, or in safe mode (from safe mode, restart
                                    leaves it); refused while the Show lock is on
  machine lock on|off               the venue's Show lock: while on, every client is refused restart, safe mode and
                                    update; kept across engine restarts
  machine update BUNDLE             send Nocturne.app (a path on this machine) to the engine machine, which installs
                                    it with tools/update-bundle.sh: restart, health check, rollback when unhealthy;
                                    prints the result. Refused under the Show lock and inside a Gig window
  displays               list the engine machine's displays (with their ids), outputs and "roles": the display it
                         keeps for each screen name
  main DISPLAY           make that display's output set the render clock's phase
  unmirror [on|off]      turn mirroring off whenever macOS mirrors displays that carry different screens (machine-local,
                         on by default), or print whether it does
  assign GROUP DISPLAY   a new screen on DISPLAY with one full slice of the group
  addscreen DISPLAY|none a new screen with one full slice of the composition
  rmscreen SCREEN
  screendisplay SCREEN DISPLAY|none move the screen; kept on the engine machine for every show's screen of that name,
                                    not in the show (no undo step)
  testcard SCREEN|all on|off        the composition-space test card through the screen's slices, or on every screen
  screen move SCREEN X Y|grid       the screen's frame on the Map canvas (top-left, board units), or back in the grid
  screen hide SCREEN on|off         leave the screen off the Map canvas; its output runs on
  screen order SCREEN INDEX         move the screen to INDEX (from 0) in the screens list
  outputonly GROUP on|off           the group shows only on slices that take it, not in the composition
  addslice SCREEN [INPUT [X Y W H]] add a full slice of INPUT (0: the composition, else a layer or group), optionally
                                    of the input rect X Y W H (0..1)
  rmslice SLICE
  corners SLICE TLX TLY TRX TRY BRX BRY BLX BLY     corner pin, in the screen's 0..1 space, y down
  sliceinput SLICE INPUT [X Y W H]  the slice's input and input rect
  warp SLICE linear|bezier [PX PY]  the warp's point mode, and PX x PY patches (1..16; subdividing keeps the shape)
  warppoints SLICE [X Y ...]        every control point of the warp grid, row-major in the slice's unit square before
                                    its corner pin ((3 PX + 1) x (3 PY + 1) of them), or none for the flat grid
  softedge SLICE L R T B [POWER [LUMINANCE [GAMMA]]]
                                    fade widths (fractions of the slice) and curve (default 2, 0.5, 2.2)
  softedge SLICE auto               fade the slice and every slice of the same input across their overlaps
  slicecolor SLICE B C R G BL       brightness, contrast, red, green, blue (-1..1), added to the screen's
  screencolor SCREEN B C R G BL
  mask add SCREEN SLICE|0 normal|invert X Y X Y X Y ...
                                    a polygon on the screen (0; screen 0..1 space) or the slice (its input's UV) that
                                    blacks out inside, or outside when inverted
  mask rm MASK | mask points MASK X Y ... | mask invert MASK on|off
  guides x=POS|y=POS ... | none     guide lines in composition UV
  showguides on|off                 draw the guides through the slices on every output (machine-local)
  target DISPLAY X Y | off          a corner target through X Y (0..1) on that display's output (machine-local)
  preset save EFFECT NAME           save an effect's parameters as a preset of its type (on the engine machine)
  preset list [TYPE]                list presets, of every type or one
  preset apply EFFECT NAME          set an effect's parameters from a preset, as one undoable edit
  preset default TYPE NAME|none     new effects of TYPE start from the preset NAME (none: from the type's defaults)
  preset savechain NAME LAYER [COLUMN]
                                    save the layer's chain (0: the composition's) or its clip's as a chain preset
  preset rmchain NAME               delete a chain preset
  preset export FILE                write this machine's own presets, defaults and chain presets to FILE
  preset import FILE                add the presets of a Presets.json FILE to the engine machine's
  isf list                          the ISF shaders of the engine machine's user folder, with any error
  isf rescan                        read the user folder again: new, changed and removed shaders take effect
clip grid (undoable edits):
  cell LAYER COLUMN PATH            add a file to the library and put it into a cell
  cellclear LAYER COLUMN            empty a cell
  addcolumn DECK [INDEX]            add a column to a deck
  rmcolumn COLUMN                   remove a column with its cells
  adddeck [COLUMNS]                 add a deck with COLUMNS columns (default 4)
  rmdeck DECK                       remove a deck with its columns
  movedeck DECK INDEX               move a deck to INDEX (0-based) in the deck order
  quantise off|beat|bar|NBARS|BEATS triggers land on the next beat, bar, N bars (e.g. 2bars) or BEATS beats
  autopilot LAYER next|random|off [BARS]
                                    step the layer to the next or a random clip of its deck every BARS bars (1..64)
  hold LAYER BARS                   a trigger while Autopilot plays the layer holds its clip BARS bars (0..64)
  step LAYER COLUMN BARS|layer      the clip in the cell plays BARS bars under Autopilot, or the layer's step
  guard on|off                      the frame budget guard that draws layers at half resolution when frames overrun
  pace 0.5|1|2|4|8                  every Autopilot step and hold of the composition lasts this many times longer
  columnpilot next|random|off [BARS]
                                    trigger the next or a random column of the shown deck every BARS bars (1..64)
  columnhold BARS                   a trigger while the column Autopilot runs holds it BARS bars (0..64)
  transition LAYER [alpha|wipe|dissolve|push|windowSlice|radial|randomSquares] [BEATS|cut]
                                    how the layer's clips change, over BEATS beats (0..64, fractions too) or a cut;
                                    a user ISF transition by its type (isf list)
  transitionparam LAYER MODE NAME VALUE|default
                                    a parameter of the layer's ISF transition MODE (e.g. windowSlice count 20)
  cliptransition LAYER COLUMN BEATS|layer
                                    the beats of the transition into the clip in the cell, or the layer's
  textcell LAYER COLUMN TEXT [KEY=VALUE ...]
                                    put a text clip into a cell; \n in TEXT breaks the line
  text LAYER COLUMN KEY=VALUE ...   change the text clip in a cell, one undoable edit. KEYs: text, font (a name as in
                                    Font Book; a missing font falls back to the system font), size (0.005..1 of the
                                    composition height), color and outlinecolor (#RRGGBB[AA] or R,G,B[,A] in 0..1),
                                    align (left, center, right), x and y (the anchor, 0..1, y down), spacing (line
                                    spacing, 0.5..4), outline (width, 0..0.5 of the size)
triggers (live, not undoable):
  trigger LAYER COLUMN              play a layer's cell from the next quantised beat
  column COLUMN                     play a column on every layer; layers with an empty cell there clear
  deck DECK                         show a deck; what plays keeps playing
  arm LAYER COLUMN [off]            preload a cell besides the active and next column
  cancel [LAYER | column COLUMN]    take back the triggers still waiting for their beat (all, a layer's or a column's)
  stop LAYER                        the layer plays nothing until its next trigger
  relink LAYER COLUMN [PATH]        find the cell's missing media again: by hash in the library's folders, or as the
                                    file PATH; other content at PATH replaces it in every cell (one undoable step)
  identify [DISPLAY|all [SECONDS]]  each output (or one) shows its screen's name and output large (default 3 s)
prepare (edits go to a copy kept in a file on this machine per engine; triggers and the clock still go live):
  prepare start                     edit a copy of the venue's composition: from now on every undoable edit, undo and
                                    redo of nocturne-ctl changes the copy, and the venue is unchanged
  prepare status                    the copy's edits, and those the venue's own changes leave out now
  prepare send now|at-switch        apply the copy's edits on the venue as one undo step, now or at the next deck switch
                                    (a Cue on the venue, also with no Director); edits whose ground the venue changed
                                    meanwhile are left out and listed
  prepare discard                   drop the copy; the venue never saw it
sections and cues:
  section intro|build|drop|breakdown|outro [BAR]
                                    live: the Section starts on the next bar, or on bar BAR of the beat clock (bar 1
                                    starts on beat 0); it lasts until the next one
  cue add NAME WHEN [CHANGE ...]    a Cue (undoable): WHEN is SECTION (its first bar), SECTION@BAR (bar BAR of every
                                    such Section) or @BAR (bar BAR of the beat clock); CHANGE is
                                    autopilot:LAYER=MODE[/BARS], autopilot:LAYER=/BARS or macro:MACRO=VALUE[/BARS]
                                    (ramping over BARS bars), effect:EFFECT=MODE[/BARS] (an effect's Autopilot),
                                    step:EFFECT=N (moves the effect to its step N), palette:N (switches to
                                    palette N), palettepilot:MODE[/BARS] (the palette Autopilot),
                                    held until the Section it fired in ends, then the
                                    composition's own values return; column:COLUMN fires that column (its deck's),
                                    column:#N the Nth column (1-based) of whichever deck is shown when it fires
  cue at HH:MM NAME [date:YYYY-MM-DD] [deck:DECK] [column:COLUMN|column:#N]
                                    a Cue at a time of day on the venue's clock, daily or on that date (undoable): it
                                    shows the deck and fires the column on the next bar after the time
  cue rm CUE
  cue on|off CUE                   switch a Cue on or off (undoable); a Cue switched off never fires
  cue every CUE BARS|once           the Cue fires again every BARS bars from its first firing, within its Section or
                                    on the beat clock (undoable); once fires it once
  section cancel [BAR]              live: drop the scheduled Sections, or the one on bar BAR
  cue cancel CUE                    live: skip the Cue's next firing
  cue clear                         live: an empty timeline, for a new track
  cue state                         the Section playing, the Cues holding and those to come
gigs (files on this machine under ~/Library/Application Support/Nocturne/Gigs/HOST/DATE.json; times are the venue's):
  gig show [DATE]                   the Gig of that date, or the next one that has not ended
  gig set DATE HH:MM-HH:MM [DJ@HH:MM=DECK ...]
                                    write the Gig (window and lineup) and apply it to the venue
  gig apply [DATE]                  apply the Gig to the venue in one undo step: its window, and a deck switch Cue per
                                    slot in place of the ones it made before
  gig shift SLOT MINUTES [following] [DATE]
                                    move a slot (1 is the first) by MINUTES, with following the later ones too, and
                                    apply it
  switches [DATE]                   the night's deck switches (default the night now): time, deck, DJ, cause and
                                    where each lands in the recording
this machine's media (choices in ~/Library/Application Support/Nocturne/keep-local.json; the UI fetches what is kept
and deletes fetched copies no longer kept; nothing changes in the bucket or on a venue):
  media keep HASH|show              keep the clip's media on this machine, or every clip of the venue's open show
  media drop HASH|show              don't keep it here; a clip's choice wins over its shows'
  media follow HASH                 the clip follows its shows again
  media local                       the choices: clips kept or not, shows kept with how many clips they use
  media fetch HASH                  fetch the media from the bucket into ~/Library/Application Support/Nocturne/Fetched
                                    now (R2 settings as nocturne-upload's)
  media upload FILE|DIR...          convert into the playback format and upload under its hash, as nocturne-upload
                                    does; each file's state goes to ~/Library/Application Support/Nocturne/Uploads/
                                    states.json
  media status [HASH...]            upload state: local only, uploading with its progress, uploaded, or failed with
                                    why; without HASH every clip in this machine's library and every upload
palette (undoable edits of the active palette; SLOT is a colour's 1-based number or its name, PALETTE a palette's):
  palette list                      the palettes, the active one marked *: each colour's number, name, #RRGGBB[AA]
                                    and what follows it, and the palette Autopilot
  palette new NAME                  add a palette, a copy of the active one's colours (palettes match by slot)
  palette delete PALETTE            remove a palette; colours linked to it follow the active palette's slots
  palette select PALETTE            live: make the palette active; linked colours crossfade to it from the next
                                    quantised beat over the palette transition
  palette next                      live: select the palette after the one the show is on
  palette pilot next|random|off [BARS]
                                    step through the palettes every BARS bars (1..64), on the bar
  palette hold BARS                 a palette selection holds the palette Autopilot for BARS bars (0: one step)
  palette transition BEATS          the crossfade into a palette (0..64 beats, 0 cuts)
  palette deck DECK PALETTE|none    showing the deck selects the palette
  palette add NAME HEX              add a colour (#RGB, #RRGGBB or #RRGGBBAA); the first one creates the palette
  palette set SLOT HEX              recolour a palette colour, and every colour linked to it with it
  palette link SLOT EFFECT GROUP    the effect's colour group (its first parameter, e.g. colorRed) follows SLOT
  palette unlink EFFECT GROUP       stop following; the colour stays
  palette name SLOT NAME            rename a palette colour
  palette remove SLOT               remove a palette colour
recording (machine-local, live, not undoable; with --state kept across restarts but the target and a .mov path):
  record start [KEY=VALUE ...]      start recording; KEYs as below set the settings first
  record stop                       stop, once the movie is written
  record state                      the settings, and the recording's file, frames recorded and dropped; "audio" is
                                    on, off or unavailable, with "audioSource" (device, channels) and "audioReason"
  record target composition|screen:N
  record codec hevc|prores          HEVC uses the hardware encoder where the machine has one
  record path FILE.mov|FOLDER       on the engine machine (default ~/Movies/Nocturne, a file per recording); a
                                    missing or read-only folder refuses the start
  record size WxH                   0x0 for the composition's size
  record reserve MB|N%              the reserve's size, or its share of the disk: a recording stops, and none starts,
                                    when less than the larger stays free (default the engine's, 20480 MB and 10%)
  record max-size MB                past MB the recording goes on in the next numbered file (0: never, the default)
  record max-length SECONDS         past SECONDS the recording goes on in the next numbered file (0: never)
  record max-total SECONDS|off      past SECONDS in all its files the recording finishes (default 28800, 8 h)
  record gig-grace SECONDS          a recording started inside a Gig window finishes SECONDS after its end
                                    (default 900)
  record audio on|off               an audio track of the audio input, when one runs (default on)
  record precise on|off             HEVC Main10 and ProRes from 16 bits instead of 8-bit input (default off)
audio (machine-local, live):
  audio devices                     list the engine machine's input devices
  audio apps                        list the processes with audio, for app taps
  audio input                       show the input in use and the microphone permission
  audio select DEVICE|default|system|app:NAME|off [CHANNELS]
                                    capture a device by UID, name or part of a name (the default input while it is
                                    missing), mixing the 1-based CHANNELS (e.g. 2 or 1,2; default all); system: all
                                    system audio; app:NAME: one app's audio by part of its bundle ID or name, or pid
  audio state                       print the analysis once
  audio watch [HZ]                  show the live analysis (default 30 Hz) until interrupted
  audio gain auto|DB                auto gain, or a fixed gain in dB
  audio latency MS                  delay audio reactions by MS (0..1000)
  audio envelope FEATURE ATTACK RELEASE
                                    attack and release in ms of level, sub, bass, lowMid, mid, highMid, high, kick,
                                    snare, hat, lowBand, midBand or highBand
  listen [SECONDS] [FILE]           hear the audio input as the venue streams it back (Opus, about 0.5 s behind),
                                    decoded into a WAV file, or to stdout without FILE or with -, for SECONDS or until
                                    interrupted; kbit/s and lost packets on stderr each second
settings (machine-local, live; with --state kept across restarts, engine flags override them on each start):
  settings                          the profile, protect transitions, cache sizes with what they hold now, and the
                                    fields a flag set
  settings profile smooth|sharp     smooth: the frame guard lowers resolution as the GPU time nears the budget;
                                    sharp: only once frames overrun it
  settings protect on|off           during a transition the guard lowers resolution before a frame would drop
  settings bake on|off              draw a chain's static leading passes once and reuse them (same picture)
  settings render-scale 100|83|67   render the composition at that percent of the output size, scaled up once; text
                                    layers and what is above them stay at the output size
  settings in-flight ring|1|2       rendered frames on the GPU at once: as many as the frame ring holds, or 1 or 2,
                                    which under GPU overload lets the outputs' presents through sooner at some render
                                    rate
  settings clip-cache MB            the frames each playing clip holds (64..65536)
  settings preload MB               the frames of clips preloaded for the next trigger (0..65536)
  settings kept-previews N          clip previews kept besides the watched ones (0..64)
DJ link (machine-local, receive only: the engine never sends onto the DJ network):
  djlink on [INTERFACE]             listen to Pro DJ Link on INTERFACE (e.g. en7; default the saved one), remembered
  djlink off                        stop listening, remembered
  djlink status                     devices, players, the on-air player, its BPM and beat
venue media (the venue pulls the media its composition uses from R2 with its read-only token, outside its Gig windows):
  media sync status                 ready, or how many files are missing by deck; the download in progress, any error
                                    and the alert 2 h before a Gig while not ready
  media sync allow|block|auto       force downloads on even inside a Gig window, or off (stops a running one), until
                                    set back to auto by hand; kept on the venue
macros and modulators (undoable edits; a driven value is its base plus offsets, times factors, within its range):
  macro add [NAME [GROUP]]          a macro on the composition, or on group GROUP (8 on each)
  macro rm MACRO
  macro set MACRO VALUE             the macro's value (0..1)
  macro target MACRO PARAM MIN MAX [CURVE]
                                    the macro adds MIN..MAX to PARAM through CURVE as it goes from 0 to 1
  macro untarget MACRO INDEX
  mod lfo TARGET SHAPE BEATS [MIN MAX [CURVE]] [phase=P] [combine=add|multiply] [points=POINTS]
                                    a beat LFO (sine, saw, square, ramp, random, custom; peaks on the beat) over BEATS beats
                                    (1/16..64); POINTS make it custom: up to 16 PHASE:VALUE (0..1, phase below 1, in
                                    order), comma separated, "s" after one eases it into the next (e.g. 0:1s,0.5:0s)
  mod audio TARGET FEATURE [MIN MAX [CURVE]] [combine=add|multiply] [autorange=on|off]
                                    an audio feature (level, sub, bass, lowMid, mid, highMid, high, kick, snare, hat,
                                    or lowBand, midBand, highBand split at the edges `bands` sets); with autorange (on
                                    for new ones) it maps the feature's running range over the last 30 s, not raw 0..1,
                                    so it swings the same at any input level
  mod rm MODULATOR
  mod set MODULATOR KEY=VALUE ...   change shape, points, beats, phase, feature, attack, hold, release (ms), min, max,
                                    curve, combine or autorange (on|off); attack, hold or release make the envelope
                                    custom
  modenv MODULATOR|all PRESET       an audio modulator's envelope, or every one's: meter (new ones start on it), punchy,
                                    smooth, breathe, gate (instant on, fixed decay) or custom
  mod state                         the live values of macros, modulators (with the range an autorange one maps) and
                                    driven parameters
  macroset save NAME | load NAME | list
                                    Macro Sets on the engine machine: macro names, values and modulators by
                                    slot; loading keeps each macro's targets (undoable)
  PARAM is LAYER.opacity, EFFECT.NAME or a text parameter of the clip the layer plays: LAYER.textSize, textX,
  textY, textRed, textGreen, textBlue or textAlpha; TARGET is a PARAM or m:MACRO. A modulator maps its 0..1 through CURVE
  onto MIN..MAX (default 0..1), added to or multiplying its target. CURVE is linear (default), easeIn, easeOut,
  easeInOut or X1,Y1,X2,Y2 (cubic Bezier).
composition files (paths on the engine machine; MEDIA_ROOT defaults to the file's folder):
  save FILE [MEDIA_ROOT]            save the whole composition, media by path relative to MEDIA_ROOT, name and hash
  load FILE [MEDIA_ROOT] [--remap-displays]
                                    replace the composition with the file's (one undoable edit); media are found by
                                    path, then name, then content, missing ones leave their cell empty; a screen whose
                                    display is not attached moves to a free one only when this machine never saw that
                                    display, or with --remap-displays
  showmedia FILE [MEDIA_ROOT]       offline: print each media file the file names, its path on this machine and its
                                    path under the media root, tab-separated (MEDIA_ROOT: the one it was saved with)
)";

[[noreturn]] void usage(int code)
{
    std::fputs(kUsage, code ? stderr : stdout);
    std::exit(code);
}

struct Options {
    std::string host = "127.0.0.1";
    uint16_t port = azl::kDefaultPort;
    std::string token;
    azl::Encoding encoding = azl::Encoding::MessagePack;
    std::vector<std::string> command;
};

// Effect commands once named the layer before the effect id; it is dropped.
void dropEffectLayer(std::vector<std::string>& c)
{
    static const std::map<std::string, size_t> sizes{
        {"fxrm", 2}, {"fxmove", 3}, {"fxbypass", 3}, {"param", 4}, {"animate", 6}};
    if (auto it = sizes.find(c[0]); it != sizes.end() && c.size() == it->second + 1)
        c.erase(c.begin() + 1);
    if (c[0] == "preset" && c.size() == 5 && (c[1] == "save" || c[1] == "apply"))
        c.erase(c.begin() + 2);
}

Options parse(int argc, char** argv)
{
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&] {
            if (i + 1 >= argc)
                usage(2);
            return std::string(argv[++i]);
        };
        if (arg == "--host") o.host = value();
        else if (arg == "--port") o.port = uint16_t(std::stoi(value()));
        else if (arg == "--token") o.token = value();
        else if (arg == "--json") o.encoding = azl::Encoding::Json;
        else if (arg == "--help" || arg == "-h") usage(0);
        else o.command.push_back(arg);
    }
    if (o.command.empty())
        o.command.push_back("watch");
    dropEffectLayer(o.command);
    if (const char* token = std::getenv("NOCTURNE_TOKEN"); o.token.empty() && token)
        o.token = token;
    if (o.token.empty() && !azl::loopbackAddress(o.host) && o.host != "localhost" && azl::onThisMachine(o.host))
        o.token = azl::readToken(azl::tokenFile(azl::defaultStateFolder()));
    return o;
}

// fx LAYER TYPE [NAME=VALUE ...], or with a column clipfx LAYER COLUMN TYPE [NAME=VALUE ...].
azl::AddEffect addEffect(const std::vector<std::string>& c, bool clip)
{
    const size_t type = clip ? 3 : 2;
    azl::AddEffect edit{std::stoull(c[1]), std::nullopt, {0, c[type], {}}};
    if (clip)
        edit.column = std::stoull(c[2]);
    for (size_t i = type + 1; i < c.size(); ++i) {
        const size_t eq = c[i].find('=');
        if (eq == std::string::npos)
            usage(2);
        edit.effect.params[c[i].substr(0, eq)] = std::stod(c[i].substr(eq + 1));
    }
    return edit;
}

std::optional<azl::Message> speedMessage(const std::vector<std::string>& c)
{
    azl::Message transport{{"op", "setClipTransport"}};
    size_t column = 0;
    if (c[0] == "speed" && (c.size() == 3 || c.size() == 4)) {
        transport["speed"] = std::stod(c[2]);
        column = 3;
    } else if (c[0] == "inout" && (c.size() == 4 || c.size() == 5)) {
        transport["in"] = std::stod(c[2]);
        transport["out"] = std::stod(c[3]);
        column = 4;
    } else {
        return std::nullopt;
    }
    transport["layer"] = std::stoull(c[1]);
    if (c.size() > column)
        transport["column"] = std::stoull(c[column]);
    return azl::msg::command(azl::editFromJson(transport));
}

std::optional<azl::Message> transportMessage(const std::vector<std::string>& c)
{
    if ((c.size() != 3 && c.size() != 4) || (c[0] != "sync" && c[0] != "playmode" && c[0] != "beats"))
        return std::nullopt;
    azl::Message transport{{"op", "setClipTransport"}, {"layer", std::stoull(c[1])}};
    if (c.size() == 4)
        transport["column"] = std::stoull(c[3]);
    if (c[0] == "sync")
        transport["sync"] = c[2];
    else if (c[0] == "playmode")
        transport["playMode"] = c[2];
    else
        transport["beats"] = c[2] == "auto" ? 0 : std::stoi(c[2]);
    return azl::msg::command(azl::editFromJson(transport));
}

double beatValue(const std::string& text)
{
    const size_t slash = text.find('/');
    return slash == std::string::npos ? std::stod(text) : std::stod(text.substr(0, slash)) / std::stod(text.substr(slash + 1));
}

bool onOff(const std::string& s)
{
    if (s != "on" && s != "off")
        usage(2);
    return s == "on";
}

std::optional<azl::Message> layerMessage(const std::vector<std::string>& c)
{
    if (c[0] == "addlayer" && c.size() <= 3)
        return azl::msg::command(azl::AddLayer{c.size() > 1 ? std::stoull(c[1]) : 0,
                                               c.size() > 2 ? std::optional<size_t>(std::stoull(c[2])) : std::nullopt,
                                               {}});
    if (c[0] == "addgroup" && c.size() <= 2)
        return azl::msg::command(azl::AddLayer{
            0, c.size() > 1 ? std::optional<size_t>(std::stoull(c[1])) : std::nullopt, {.group = true}});
    if (c[0] == "rmlayer" && c.size() == 2)
        return azl::msg::command(azl::RemoveLayer{std::stoull(c[1])});
    if (c[0] == "movelayer" && c.size() == 4)
        return azl::msg::command(azl::MoveLayer{std::stoull(c[1]), std::stoull(c[2]), std::stoull(c[3])});
    if (c[0] == "blend" && c.size() == 3)
        return azl::msg::command(azl::SetLayerBlend{std::stoull(c[1]), azl::blendModeFromName(c[2])});
    if (c[0] == "bypass" && c.size() == 3)
        return azl::msg::command(azl::SetLayerBypass{std::stoull(c[1]), onOff(c[2])});
    if (c[0] == "solo" && c.size() == 3)
        return azl::msg::command(azl::SetLayerSolo{std::stoull(c[1]), onOff(c[2])});
    if (c[0] == "fullres" && c.size() == 3)
        return azl::msg::command(azl::SetLayerFullResolution{std::stoull(c[1]), onOff(c[2])});
    return std::nullopt;
}

std::string displayArg(const std::string& s)
{
    return s == "none" ? std::string() : s;
}

std::array<double, 4> rectArgs(const std::vector<std::string>& c, size_t from)
{
    return {std::stod(c[from]), std::stod(c[from + 1]), std::stod(c[from + 2]), std::stod(c[from + 3])};
}

std::optional<azl::Message> sliceMessage(const std::vector<std::string>& c)
{
    if (c[0] == "addslice" && (c.size() == 2 || c.size() == 3 || c.size() == 7)) {
        azl::Slice slice{.input = c.size() > 2 ? std::stoull(c[2]) : 0};
        if (c.size() == 7)
            slice.inputRect = rectArgs(c, 3);
        return azl::msg::command(azl::AddSlice{std::stoull(c[1]), std::nullopt, slice});
    }
    if (c[0] == "rmslice" && c.size() == 2)
        return azl::msg::command(azl::RemoveSlice{std::stoull(c[1])});
    if (c[0] == "corners" && c.size() == 10) {
        std::array<double, 8> corners;
        for (size_t i = 0; i < 8; ++i)
            corners[i] = std::stod(c[i + 2]);
        return azl::msg::command(azl::SetSliceCorners{std::stoull(c[1]), corners});
    }
    if (c[0] == "sliceinput" && (c.size() == 3 || c.size() == 7))
        return azl::msg::command(azl::SetSliceInput{
            std::stoull(c[1]), std::stoull(c[2]),
            c.size() == 7 ? std::optional<std::array<double, 4>>(rectArgs(c, 3)) : std::nullopt});
    return std::nullopt;
}

std::vector<azl::Point> pointArgs(const std::vector<std::string>& c, size_t from)
{
    if ((c.size() - from) % 2)
        usage(2);
    std::vector<azl::Point> points;
    for (size_t i = from; i < c.size(); i += 2)
        points.push_back({std::stod(c[i]), std::stod(c[i + 1])});
    return points;
}

azl::ColorAdjust colorArgs(const std::vector<std::string>& c)
{
    return {std::stod(c[2]), std::stod(c[3]), std::stod(c[4]), std::stod(c[5]), std::stod(c[6])};
}

std::optional<azl::Message> warpMessage(const std::vector<std::string>& c)
{
    if (c[0] == "warp" && (c.size() == 3 || c.size() == 5) && (c[2] == "linear" || c[2] == "bezier"))
        return azl::msg::command(azl::SetSliceWarp{
            std::stoull(c[1]), c[2] == "bezier",
            c.size() == 5 ? std::optional<std::array<int, 2>>({std::stoi(c[3]), std::stoi(c[4])}) : std::nullopt,
            std::nullopt});
    if (c[0] == "warppoints" && c.size() >= 2)
        return azl::msg::command(azl::SetSliceWarp{std::stoull(c[1]), std::nullopt, std::nullopt, pointArgs(c, 2)});
    if (c[0] == "softedge" && c.size() == 3 && c[2] == "auto")
        return azl::msg::command(azl::SetSoftEdges{{}, std::stoull(c[1])});
    if (c[0] == "softedge" && c.size() >= 6 && c.size() <= 9) {
        azl::SoftEdge soft{std::stod(c[2]), std::stod(c[3]), std::stod(c[4]), std::stod(c[5])};
        if (c.size() > 6)
            soft.power = std::stod(c[6]);
        if (c.size() > 7)
            soft.luminance = std::stod(c[7]);
        if (c.size() > 8)
            soft.gamma = std::stod(c[8]);
        return azl::msg::command(azl::SetSoftEdges{{{std::stoull(c[1]), soft}}, 0});
    }
    if (c[0] == "slicecolor" && c.size() == 7)
        return azl::msg::command(azl::SetSliceColor{std::stoull(c[1]), colorArgs(c)});
    if (c[0] == "screencolor" && c.size() == 7)
        return azl::msg::command(azl::SetScreenColor{std::stoull(c[1]), colorArgs(c)});
    return std::nullopt;
}

std::optional<azl::Message> maskMessage(const std::vector<std::string>& c)
{
    if (c[0] != "mask" || c.size() < 3)
        return std::nullopt;
    if (c[1] == "add" && c.size() >= 11 && (c[4] == "normal" || c[4] == "invert"))
        return azl::msg::command(azl::AddMask{std::stoull(c[2]), std::stoull(c[3]), std::nullopt,
                                              {.invert = c[4] == "invert", .points = pointArgs(c, 5)}});
    if (c[1] == "rm" && c.size() == 3)
        return azl::msg::command(azl::RemoveMask{std::stoull(c[2])});
    if (c[1] == "points" && c.size() >= 9)
        return azl::msg::command(azl::SetMask{std::stoull(c[2]), pointArgs(c, 3), std::nullopt, std::nullopt});
    if (c[1] == "invert" && c.size() == 4)
        return azl::msg::command(azl::SetMask{std::stoull(c[2]), std::nullopt, onOff(c[3]), std::nullopt});
    return std::nullopt;
}

// x=POS or y=POS each, or none.
std::optional<azl::Message> guidesMessage(const std::vector<std::string>& c)
{
    if (c[0] != "guides" || c.size() < 2)
        return std::nullopt;
    std::vector<azl::Guide> guides;
    for (size_t i = 1; i < c.size() && c[i] != "none"; ++i) {
        if (c[i].size() < 3 || (c[i][0] != 'x' && c[i][0] != 'y') || c[i][1] != '=')
            usage(2);
        guides.push_back({c[i][0] == 'x', std::stod(c[i].substr(2))});
    }
    return azl::msg::command(azl::SetGuides{guides});
}

// screen move SCREEN X Y|grid, screen hide SCREEN on|off, screen order SCREEN INDEX.
std::optional<azl::Message> screenArrangeMessage(const std::vector<std::string>& c)
{
    if (c[0] != "screen" || c.size() < 4)
        return std::nullopt;
    const uint64_t screen = std::stoull(c[2]);
    if (c[1] == "move" && c.size() == 4 && c[3] == "grid")
        return azl::msg::command(azl::SetScreenPosition{screen, std::nullopt});
    if (c[1] == "move" && c.size() == 5)
        return azl::msg::command(azl::SetScreenPosition{screen, std::array{std::stod(c[3]), std::stod(c[4])}});
    if (c[1] == "hide" && c.size() == 4)
        return azl::msg::command(azl::SetScreenHidden{screen, onOff(c[3])});
    if (c[1] == "order" && c.size() == 4)
        return azl::msg::command(azl::MoveScreen{screen, std::stoull(c[3])});
    return std::nullopt;
}

std::optional<azl::Message> screenMessage(const std::vector<std::string>& c)
{
    if (auto arrange = screenArrangeMessage(c))
        return arrange;
    if (auto warp = warpMessage(c))
        return warp;
    if (auto mask = maskMessage(c))
        return mask;
    if (auto guides = guidesMessage(c))
        return guides;
    if (c[0] == "assign" && c.size() == 3)
        return azl::msg::command(azl::AssignDisplay{std::stoull(c[1]), c[2]});
    if (c[0] == "addscreen" && c.size() == 2)
        return azl::msg::command(azl::AddScreen{std::nullopt, {.display = displayArg(c[1]), .slices = {{}}}});
    if (c[0] == "rmscreen" && c.size() == 2)
        return azl::msg::command(azl::RemoveScreen{std::stoull(c[1])});
    if (c[0] == "screendisplay" && c.size() == 3)
        return azl::msg::command(azl::SetScreenDisplay{std::stoull(c[1]), displayArg(c[2])});
    if (c[0] == "testcard" && c.size() == 3)
        return azl::msg::command(azl::SetScreenTestCard{c[1] == "all" ? 0 : std::stoull(c[1]), onOff(c[2])});
    if (c[0] == "outputonly" && c.size() == 3)
        return azl::msg::command(azl::SetGroupOutputOnly{std::stoull(c[1]), onOff(c[2])});
    return sliceMessage(c);
}

int quantiseBeats(const std::string& text)
{
    if (text == "off")
        return 0;
    if (text == "beat")
        return 1;
    if (text == "bar")
        return 4;
    if (text.ends_with("bars"))
        return 4 * std::stoi(text);
    return std::stoi(text);
}

std::optional<azl::Message> gridMessage(const std::vector<std::string>& c)
{
    if (c[0] == "cellclear" && c.size() == 3)
        return azl::msg::command(azl::SetSlot{std::stoull(c[1]), std::stoull(c[2]), std::nullopt});
    if (c[0] == "addcolumn" && (c.size() == 2 || c.size() == 3))
        return azl::msg::command(azl::AddColumn{
            std::stoull(c[1]), c.size() == 3 ? std::optional<size_t>(std::stoull(c[2])) : std::nullopt, {}, {}});
    if (c[0] == "rmcolumn" && c.size() == 2)
        return azl::msg::command(azl::RemoveColumn{std::stoull(c[1])});
    if (c[0] == "adddeck" && c.size() <= 2)
        return azl::msg::command(
            azl::AddDeck{std::nullopt, {0, "", std::vector<azl::Column>(c.size() == 2 ? std::stoull(c[1]) : 4)}, {}});
    if (c[0] == "rmdeck" && c.size() == 2)
        return azl::msg::command(azl::RemoveDeck{std::stoull(c[1])});
    if (c[0] == "movedeck" && c.size() == 3)
        return azl::msg::command(azl::MoveDeck{std::stoull(c[1]), std::stoull(c[2])});
    if (c[0] == "quantise" && c.size() == 2)
        return azl::msg::command(azl::SetQuantise{quantiseBeats(c[1])});
    if (c[0] == "cliptrigger" && c.size() == 4)
        return azl::msg::command(azl::SetClipTrigger{std::stoull(c[1]), std::stoull(c[2]),
                                                     c[3] == "default" ? std::nullopt : std::optional(quantiseBeats(c[3]))});
    if (c[0] == "autopilot" && (c.size() == 3 || c.size() == 4)) {
        azl::Message m{{"op", "setLayerAutopilot"}, {"layer", std::stoull(c[1])}, {"mode", c[2]}};
        if (c.size() == 4)
            m["bars"] = std::stoi(c[3]);
        return azl::msg::command(azl::editFromJson(m));
    }
    if (c[0] == "fxpilot" && (c.size() == 3 || c.size() == 4)) {
        azl::Message m{{"op", "setEffectAutopilot"}, {"effect", std::stoull(c[1])}, {"mode", c[2]}};
        if (c.size() == 4)
            m["bars"] = std::stoi(c[3]);
        return azl::msg::command(azl::editFromJson(m));
    }
    if (c[0] == "fxhold" && c.size() == 3)
        return azl::msg::command(azl::SetEffectAutopilot{std::stoull(c[1]), std::nullopt, std::nullopt, std::stoi(c[2])});
    if (c[0] == "fxtransition" && c.size() == 3)
        return azl::msg::command(
            azl::SetEffectAutopilot{std::stoull(c[1]), std::nullopt, std::nullopt, std::nullopt, std::stod(c[2])});
    if (c[0] == "hold" && c.size() == 3)
        return azl::msg::command(azl::SetLayerAutopilot{std::stoull(c[1]), std::nullopt, std::nullopt, std::stoi(c[2])});
    if (c[0] == "pace" && c.size() == 2)
        return azl::msg::command(azl::SetAutopilotPace{std::stod(c[1])});
    if (c[0] == "columnpilot" && (c.size() == 2 || c.size() == 3)) {
        azl::Message m{{"op", "setColumnAutopilot"}, {"mode", c[1]}};
        if (c.size() == 3)
            m["bars"] = std::stoi(c[2]);
        return azl::msg::command(azl::editFromJson(m));
    }
    if (c[0] == "columnhold" && c.size() == 2)
        return azl::msg::command(azl::SetColumnAutopilot{std::nullopt, std::nullopt, std::stoi(c[1])});
    if (c[0] == "guard" && c.size() == 2)
        return azl::msg::command(azl::SetFrameGuard{onOff(c[1])});
    if (c[0] == "transition" && (c.size() == 3 || c.size() == 4)) {
        azl::Message m{{"op", "setLayerTransition"}, {"layer", std::stoull(c[1])}};
        for (size_t i = 2; i < c.size(); ++i) {
            if (c[i] == "cut")
                m["beats"] = 0.0;
            else if (std::isdigit(static_cast<unsigned char>(c[i][0])) || c[i][0] == '.')
                m["beats"] = std::stod(c[i]);
            else
                m["mode"] = c[i];
        }
        return azl::msg::command(azl::editFromJson(m));
    }
    if (c[0] == "transitionparam" && c.size() == 5)
        return azl::msg::command(azl::SetTransitionParam{
            std::stoull(c[1]), azl::transitionFromName(c[2]), c[3],
            c[4] == "default" ? std::nullopt : std::optional<double>(std::stod(c[4]))});
    if (c[0] == "cliptransition" && c.size() == 4)
        return azl::msg::command(azl::SetClipTransition{
            std::stoull(c[1]), std::stoull(c[2]),
            c[3] == "layer" ? std::nullopt : std::optional<double>(std::stod(c[3]))});
    if (c[0] == "step" && c.size() == 4)
        return azl::msg::command(
            azl::SetClipStep{std::stoull(c[1]), std::stoull(c[2]), c[3] == "layer" ? 0 : std::stoi(c[3])});
    return std::nullopt;
}

std::optional<azl::Message> isfMessage(const std::vector<std::string>& c)
{
    if (c[0] != "isf")
        return std::nullopt;
    if (c.size() == 2 && (c[1] == "list" || c[1] == "rescan"))
        return azl::Message{{"type", "isf"}, {"op", c[1]}};
    usage(2);
}

std::vector<std::string> split(const std::string& text, char by);

std::array<double, 4> colorArg(const std::string& text)
{
    std::array<double, 4> color{0.0, 0.0, 0.0, 1.0};
    if (!text.empty() && text[0] == '#') {
        if (text.size() != 7 && text.size() != 9)
            usage(2);
        for (size_t i = 0; i * 2 + 1 < text.size(); ++i)
            color[i] = double(std::stoul(text.substr(1 + i * 2, 2), nullptr, 16)) / 255.0;
        return color;
    }
    const std::vector<std::string> parts = split(text, ',');
    if (parts.size() < 3 || parts.size() > 4)
        usage(2);
    for (size_t i = 0; i < parts.size(); ++i)
        color[i] = std::stod(parts[i]);
    return color;
}

std::string unescapeNewlines(const std::string& text)
{
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == 'n') {
            out += '\n';
            ++i;
        } else {
            out += text[i];
        }
    }
    return out;
}

// The text clip fields of the KEY=VALUE arguments from `from` on, as setClipText takes them.
azl::Message textFields(const std::vector<std::string>& c, size_t from)
{
    azl::Message m = azl::Message::object();
    for (size_t i = from; i < c.size(); ++i) {
        const size_t eq = c[i].find('=');
        if (eq == std::string::npos)
            usage(2);
        const std::string key = c[i].substr(0, eq);
        const std::string value = c[i].substr(eq + 1);
        if (key == "text") m["text"] = unescapeNewlines(value);
        else if (key == "font" || key == "align") m[key] = value;
        else if (key == "size" || key == "x" || key == "y" || key == "outline") m[key] = std::stod(value);
        else if (key == "spacing") m["lineSpacing"] = std::stod(value);
        else if (key == "color") m["color"] = colorArg(value);
        else if (key == "outlinecolor") m["outlineColor"] = colorArg(value);
        else usage(2);
    }
    return m;
}

std::optional<azl::Message> textMessage(const std::vector<std::string>& c)
{
    if (c[0] == "textcell" && c.size() >= 4) {
        azl::Message fields = textFields(c, 4);
        fields["text"] = unescapeNewlines(c[3]);
        return azl::msg::command(azl::SetSlot{std::stoull(c[1]), std::stoull(c[2]),
                                              azl::ClipRef{.name = "Text", .text = fields.get<azl::TextClip>()}});
    }
    if (c[0] == "text" && c.size() >= 4) {
        azl::Message m = textFields(c, 3);
        m["op"] = "setClipText";
        m["layer"] = std::stoull(c[1]);
        m["column"] = std::stoull(c[2]);
        return azl::msg::command(azl::editFromJson(m));
    }
    return std::nullopt;
}

// A 1-based step number as the protocol's 0-based index.
size_t stepIndex(const std::string& number)
{
    const size_t n = std::stoull(number);
    if (n < 1)
        throw std::runtime_error("steps are numbered from 1");
    return n - 1;
}

std::optional<azl::Message> triggerMessage(const std::vector<std::string>& c)
{
    if (c[0] == "trigger" && c.size() == 3)
        return azl::Message{{"type", "trigger"}, {"op", "clip"}, {"layer", std::stoull(c[1])},
                            {"column", std::stoull(c[2])}};
    if (c[0] == "column" && c.size() == 2)
        return azl::Message{{"type", "trigger"}, {"op", "column"}, {"column", std::stoull(c[1])}};
    if (c[0] == "deck" && c.size() == 2)
        return azl::Message{{"type", "trigger"}, {"op", "deck"}, {"deck", std::stoull(c[1])}, {"by", "ctl"}};
    if (c[0] == "arm" && (c.size() == 3 || (c.size() == 4 && c[3] == "off")))
        return azl::Message{{"type", "trigger"}, {"op", "arm"}, {"layer", std::stoull(c[1])},
                            {"column", std::stoull(c[2])}, {"armed", c.size() == 3}};
    if (c[0] == "stop" && c.size() == 2)
        return azl::Message{{"type", "trigger"}, {"op", "stop"}, {"layer", std::stoull(c[1])}};
    if (c[0] == "cancel" && c.size() == 1)
        return azl::Message{{"type", "trigger"}, {"op", "cancel"}};
    if (c[0] == "cancel" && c.size() == 2)
        return azl::Message{{"type", "trigger"}, {"op", "cancel"}, {"layer", std::stoull(c[1])}};
    if (c[0] == "cancel" && c.size() == 3 && c[1] == "column")
        return azl::Message{{"type", "trigger"}, {"op", "cancel"}, {"column", std::stoull(c[2])}};
    if (c[0] == "fxstep" && c.size() == 3)
        return azl::Message{{"type", "trigger"}, {"op", "step"}, {"effect", std::stoull(c[1])},
                            {"step", stepIndex(c[2])}};
    return std::nullopt;
}

std::optional<azl::Message> renameMessage(const std::vector<std::string>& c)
{
    static const std::set<std::string> kinds{"deck", "layer", "column", "screen", "slice", "mask", "macro"};
    if (c[0] != "rename")
        return std::nullopt;
    if (c.size() < 4)
        usage(2);
    if (!kinds.contains(c[1]))
        throw std::runtime_error("rename: unknown kind " + c[1] + " (deck, layer, column, screen, slice, mask, macro)");
    if (c[2].find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error("rename: " + c[2] + " is not an id");
    const uint64_t id = std::stoull(c[2]);
    std::string name = c[3];
    for (size_t i = 4; i < c.size(); ++i)
        name += " " + c[i];
    return azl::msg::command(azl::Rename{c[1], id, name});
}

std::optional<azl::Message> commandMessage(const std::vector<std::string>& c)
{
    if (auto rename = renameMessage(c))
        return rename;
    if (auto grid = gridMessage(c))
        return grid;
    if (auto trigger = triggerMessage(c))
        return trigger;
    if (auto text = textMessage(c))
        return text;
    if (auto speed = speedMessage(c))
        return speed;
    if (c[0] == "jump" && (c.size() == 4 || c.size() == 5))
        return azl::msg::command(azl::SetClipTransport{
            std::stoull(c[1]), std::nullopt, azl::PlayMode::RandomJump, std::nullopt, beatValue(c[2]), beatValue(c[3]),
            c.size() == 5 ? std::optional<uint64_t>(std::stoull(c[4])) : std::nullopt});
    if (c[0] == "undo" && c.size() == 1)
        return azl::msg::undo();
    if (c[0] == "redo" && c.size() == 1)
        return azl::msg::redo();
    if (c[0] == "opacity" && c.size() == 3)
        return azl::msg::command(azl::SetLayerOpacity{std::stoull(c[1]), std::stod(c[2])});
    if (c[0] == "clip" && c.size() == 4)
        return azl::msg::command(azl::SetLayerClip{std::stoull(c[1]), azl::ClipRef{c[2], c[3], {}}});
    if (c[0] == "clear" && c.size() == 2)
        return azl::msg::command(azl::SetLayerClip{std::stoull(c[1]), std::nullopt});
    if (c[0] == "fx" && c.size() >= 3)
        return azl::msg::command(addEffect(c, false));
    if (c[0] == "clipfx" && c.size() >= 4)
        return azl::msg::command(addEffect(c, true));
    if (c[0] == "fxrm" && c.size() == 2)
        return azl::msg::command(azl::RemoveEffect{std::stoull(c[1])});
    if (c[0] == "fxmove" && c.size() == 3)
        return azl::msg::command(azl::MoveEffect{std::stoull(c[1]), std::stoull(c[2])});
    if (c[0] == "fxbypass" && c.size() == 3)
        return azl::msg::command(azl::SetEffectBypass{std::stoull(c[1]), c[2] == "on"});
    if (c[0] == "sensitivity" && c.size() == 3)
        return azl::msg::command(azl::SetSensitivity{c[1], std::stod(c[2])});
    if (c[0] == "bands" && c.size() == 3)
        return azl::msg::command(azl::SetBandEdges{{std::stod(c[1]), std::stod(c[2])}});
    if (c[0] == "param" && c.size() == 4)
        return azl::msg::command(azl::SetEffectParam{std::stoull(c[1]), c[2], std::stod(c[3])});
    if (c[0] == "bpm" && c.size() == 2)
        return azl::msg::command(azl::SetTempo{std::stod(c[1])});
    if (auto layer = layerMessage(c))
        return layer;
    if (auto screen = screenMessage(c))
        return screen;
    return transportMessage(c);
}

azl::Message scrubMessage(uint64_t layer, double position)
{
    return {{"type", "clock"}, {"op", "scrub"}, {"layer", layer}, {"position", position}};
}

std::optional<azl::Message> clockMessage(const std::vector<std::string>& c)
{
    if ((c[0] == "tap" && (c.size() == 1 || c.size() == 3)) || (c[0] == "resync" && c.size() == 1))
        return azl::Message{{"type", "clock"}, {"op", c[0]}};
    if (c[0] == "clock" && c.size() == 1)
        return azl::Message{{"type", "clock"}, {"op", "state"}};
    if (c[0] == "nudge" && c.size() == 2)
        return azl::Message{{"type", "clock"}, {"op", "nudge"}, {"percent", std::stod(c[1])}};
    if (c[0] == "scrub" && (c.size() == 3 || c.size() == 5))
        return scrubMessage(std::stoull(c[1]), std::stod(c[2]));
    return std::nullopt;
}

std::optional<azl::Message> presetMessage(const std::vector<std::string>& c)
{
    if (c[0] != "preset" || c.size() < 2)
        return std::nullopt;
    if (c[1] == "list" && c.size() <= 3) {
        azl::Message m{{"type", "preset"}, {"op", "list"}};
        if (c.size() == 3)
            m["effectType"] = c[2];
        return m;
    }
    if ((c[1] == "save" || c[1] == "apply") && c.size() == 4)
        return azl::Message{{"type", "preset"}, {"op", c[1]}, {"effect", std::stoull(c[2])}, {"name", c[3]}};
    if (c[1] == "default" && c.size() == 4)
        return azl::Message{{"type", "preset"}, {"op", "default"}, {"effectType", c[2]}, {"name", c[3] == "none" ? "" : c[3]}};
    if (c[1] == "rmchain" && c.size() == 3)
        return azl::Message{{"type", "preset"}, {"op", "deleteChain"}, {"name", c[2]}};
    if (c[1] == "import" && c.size() == 3) {
        const auto file = azl::readJsonFile(c[2]);
        if (!file)
            throw std::runtime_error("cannot read " + c[2]);
        return azl::Message{{"type", "preset"}, {"op", "merge"}, {"file", *file}};
    }
    return std::nullopt;
}

// The effects of the layer's own chain (0: the composition's) or of its clip in COLUMN, as the snapshot has them.
azl::Message chainInSnapshot(const azl::Message& composition, uint64_t layer, std::optional<uint64_t> column)
{
    if (layer == 0)
        return composition.value("effects", azl::Message::array());
    std::function<const azl::Message*(const azl::Message&)> find = [&](const azl::Message& layers) {
        for (const azl::Message& l : layers) {
            if (l.value("id", uint64_t(0)) == layer)
                return &l;
            if (const azl::Message* inner = find(l.value("layers", azl::Message::array())))
                return inner;
        }
        return static_cast<const azl::Message*>(nullptr);
    };
    const azl::Message* found = find(composition["layers"]);
    if (!found)
        throw std::runtime_error("no layer " + std::to_string(layer));
    if (!column)
        return found->value("effects", azl::Message::array());
    const azl::Message clips = found->value("clips", azl::Message::object());
    const std::string key = std::to_string(*column);
    if (!clips.contains(key))
        throw std::runtime_error("no clip in that cell");
    return clips[key].value("effects", azl::Message::array());
}

azl::Message saveChainMessage(const std::vector<std::string>& c, const azl::Message& snapshot)
{
    const std::optional<uint64_t> column = c.size() == 5 ? std::optional<uint64_t>(std::stoull(c[4])) : std::nullopt;
    azl::Message effects = chainInSnapshot(snapshot["composition"], std::stoull(c[3]), column);
    for (azl::Message& e : effects)
        e.erase("id");
    return {{"type", "preset"}, {"op", "saveChain"}, {"name", c[2]}, {"effects", effects}};
}


std::optional<azl::Message> machineMessage(const std::vector<std::string>& c)
{
    if (c[0] != "machine")
        return std::nullopt;
    if (c.size() == 1)
        return azl::Message{{"type", "machine"}, {"op", "status"}};
    const std::string& op = c[1];
    if (c.size() == 2 && (op == "unmirror" || op == "restart" || op == "stop"))
        return azl::Message{{"type", "machine"}, {"op", op}};
    if (c.size() == 2 && op == "safemode")
        return azl::Message{{"type", "machine"}, {"op", "safeMode"}};
    if (c.size() == 3 && (op == "testcard" || op == "lock"))
        return azl::Message{{"type", "machine"}, {"op", op}, {"on", onOff(c[2])}};
    if (c.size() <= 3 && op == "identify")
        return azl::Message{{"type", "machine"}, {"op", op}, {"seconds", c.size() == 3 ? std::stod(c[2]) : 3.0}};
    return std::nullopt;
}

// Machine-local requests answered to the sender with a message of their own type.
std::optional<azl::Message> requestMessage(const std::vector<std::string>& c)
{
    if (c[0] == "library" && c.size() == 1)
        return azl::Message{{"type", "library"}, {"op", "list"}};
    if ((c[0] == "add" && c.size() == 2) || (c[0] == "play" && c.size() == 3) || (c[0] == "cell" && c.size() == 4))
        return azl::Message{{"type", "library"}, {"op", "add"}, {"path", c.back()}};
    if (c[0] == "displays" && c.size() == 1)
        return azl::Message{{"type", "outputs"}, {"op", "list"}};
    if (c[0] == "identify" && c.size() <= 3) {
        azl::Message m{{"type", "outputs"}, {"op", "identify"}};
        if (c.size() >= 2 && c[1] != "all")
            m["display"] = c[1];
        if (c.size() == 3)
            m["seconds"] = std::stod(c[2]);
        return m;
    }
    if (c[0] == "main" && c.size() == 2)
        return azl::Message{{"type", "outputs"}, {"op", "main"}, {"display", c[1]}};
    if (c[0] == "unmirror" && c.size() == 1)
        return azl::Message{{"type", "outputs"}, {"op", "unmirror"}};
    if (c[0] == "unmirror" && c.size() == 2)
        return azl::Message{{"type", "outputs"}, {"op", "unmirror"}, {"on", onOff(c[1])}};
    if (c[0] == "clippreviews" && c.size() == 1)
        return azl::Message{{"type", "preview"}, {"op", "clips"}};
    if (c[0] == "clippreviews" && c.size() == 2)
        return azl::Message{{"type", "preview"}, {"op", "clips"}, {"on", onOff(c[1])}};
    if (c[0] == "showguides" && c.size() == 2)
        return azl::Message{{"type", "outputs"}, {"op", "guides"}, {"show", onOff(c[1])}};
    if (c[0] == "target" && c.size() == 3 && c[2] == "off")
        return azl::Message{{"type", "outputs"}, {"op", "target"}, {"display", c[1]}, {"point", nullptr}};
    if (c[0] == "target" && c.size() == 4)
        return azl::Message{{"type", "outputs"},
                            {"op", "target"},
                            {"display", c[1]},
                            {"point", {std::stod(c[2]), std::stod(c[3])}}};
    return std::nullopt;
}

void recordSetting(azl::Message& m, const std::string& key, const std::string& value)
{
    if (key == "target" || key == "codec") {
        m[key] = value;
    } else if (key == "path") {
        m["path"] = std::filesystem::absolute(value).string();
    } else if (key == "audio") {
        m["audio"] = onOff(value);
    } else if (key == "precise") {
        m["precise"] = onOff(value);
    } else if (key == "reserve" && value.ends_with('%')) {
        m["reservePercent"] = std::stod(value);
    } else if (key == "reserve") {
        m["reserveMB"] = std::stoull(value);
    } else if (key == "max-size") {
        m["maxMB"] = std::stoull(value);
    } else if (key == "max-length") {
        m["maxSeconds"] = std::stod(value);
    } else if (key == "max-total") {
        m["maxTotalSeconds"] = value == "off" ? 0.0 : std::stod(value);
    } else if (key == "gig-grace") {
        m["gigGraceSeconds"] = std::stod(value);
    } else if (key == "size") {
        const size_t x = value.find('x');
        if (x == std::string::npos)
            usage(2);
        m["width"] = std::stoul(value.substr(0, x));
        m["height"] = std::stoul(value.substr(x + 1));
    } else {
        usage(2);
    }
}

std::optional<azl::Message> recordMessage(const std::vector<std::string>& c)
{
    if (c[0] != "record" || c.size() < 2)
        return std::nullopt;
    if ((c[1] == "stop" || c[1] == "state") && c.size() == 2)
        return azl::Message{{"type", "record"}, {"op", c[1]}};
    azl::Message m{{"type", "record"}, {"op", c[1] == "start" ? "start" : "set"}};
    if (c[1] == "start") {
        for (size_t i = 2; i < c.size(); ++i) {
            const size_t eq = c[i].find('=');
            if (eq == std::string::npos)
                usage(2);
            recordSetting(m, c[i].substr(0, eq), c[i].substr(eq + 1));
        }
        return m;
    }
    if (c.size() != 3)
        return std::nullopt;
    recordSetting(m, c[1], c[2]);
    return m;
}

std::optional<azl::Message> settingsMessage(const std::vector<std::string>& c)
{
    if (c[0] != "settings")
        return std::nullopt;
    if (c.size() == 1)
        return azl::Message{{"type", "settings"}, {"op", "state"}};
    if (c.size() != 3)
        usage(2);
    azl::Message m{{"type", "settings"}, {"op", "set"}};
    const std::string& value = c[2];
    if (c[1] == "profile")
        m["profile"] = value;
    else if (c[1] == "protect")
        m["protectTransitions"] = onOff(value);
    else if (c[1] == "bake")
        m["bakeStatic"] = onOff(value);
    else if (c[1] == "render-scale")
        m["renderScale"] = std::stoull(value);
    else if (c[1] == "in-flight")
        m["framesInFlight"] = value == "ring" ? 0 : std::stoull(value);
    else if (c[1] == "clip-cache")
        m["frameCacheMB"] = std::stoull(value);
    else if (c[1] == "preload")
        m["preloadMB"] = std::stoull(value);
    else if (c[1] == "kept-previews")
        m["clipPreviews"] = std::stoull(value);
    else
        usage(2);
    return m;
}

std::optional<azl::Message> compositionMessage(const std::vector<std::string>& c)
{
    if (c[0] != "save" && c[0] != "load")
        return std::nullopt;
    std::vector<std::string> args(c.begin() + 1, c.end());
    const bool remap = c[0] == "load" && std::erase(args, std::string("--remap-displays")) > 0;
    if (args.empty() || args.size() > 2)
        return std::nullopt;
    azl::Message m{{"type", "composition"}, {"op", c[0]}, {"path", std::filesystem::absolute(args[0]).string()}};
    if (args.size() == 2)
        m["mediaRoot"] = std::filesystem::absolute(args[1]).string();
    if (remap)
        m["remapDisplays"] = true;
    return m;
}

// Each distinct media file once: where it is on this machine, and where it goes under the media root.
// Browses for two seconds.
int listNodes()
{
    azl::Browser browser;
    std::this_thread::sleep_for(std::chrono::seconds(2));
    const std::vector<azl::Service> services = browser.services();
    for (const azl::Service& s : services) {
        auto role = s.txt.find("role");
        std::printf("%s (%s) at %s:%u\n", s.name.c_str(), role == s.txt.end() ? "?" : role->second.c_str(),
                    s.host.c_str(), unsigned(s.port));
    }
    if (services.empty())
        std::printf("no engines found\n");
    return services.empty() ? 1 : 0;
}

// The venues the UI's menu lists, with their dots: Tailscale (venues.json and the API key) and two seconds of Bonjour.
int listVenues(const Options& opt)
{
    const azl::VenueSources sources =
        azl::VenueSources::system(azl::loadTailscaleSettings(), [token = opt.token](const azl::Venue&) { return token; });
    const auto browsed = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    const azl::TailscaleList tailscale = sources.tailscale();
    std::this_thread::sleep_until(browsed);
    const azl::VenueScan scan = azl::scanVenues(sources, tailscale, {});
    if (!scan.tailscaleError.empty())
        std::fprintf(stderr, "nocturne-ctl: tailscale: %s\n", scan.tailscaleError.c_str());
    for (const azl::VenueEntry& e : scan.entries)
        for (const std::string& line : azl::venueLines(e))
            std::printf("%s\n", line.c_str());
    if (scan.entries.empty())
        std::printf("no venues found\n");
    return scan.entries.empty() ? 1 : 0;
}

int showMedia(const std::vector<std::string>& c)
{
    const std::filesystem::path file = std::filesystem::absolute(c[1]);
    std::optional<azl::Message> j = azl::readJsonFile(file);
    if (!j)
        throw std::runtime_error("no file " + file.string());
    azl::compositionFileFromJson(*j);
    const std::filesystem::path root =
        c.size() == 3 ? std::filesystem::absolute(c[2])
                      : std::filesystem::path(j->value("mediaRoot", file.parent_path().string()));
    std::set<std::string> seen;
    azl::forEachCellJson((*j)["composition"], [&](uint64_t, uint64_t, azl::Message& clip) {
        const std::filesystem::path path = clip.is_object() ? clip.value("path", "") : "";
        if (path.empty()) {
            std::fprintf(stderr, "nocturne-ctl: %s has no path\n", clip.value("name", "a clip").c_str());
            return;
        }
        const auto [source, destination] = path.is_absolute() ? std::pair(path, path.filename())
                                                              : std::pair(root / path, path);
        if (seen.insert(source.string()).second)
            std::printf("%s\t%s\n", source.string().c_str(), destination.string().c_str());
    });
    return 0;
}


std::vector<int> channelArg(const std::string& list)
{
    std::vector<int> channels;
    for (size_t at = 0; at < list.size();) {
        const size_t comma = std::min(list.find(',', at), list.size());
        channels.push_back(std::stoi(list.substr(at, comma - at)));
        at = comma + 1;
    }
    return channels;
}

std::optional<azl::Message> audioMessage(const std::vector<std::string>& c)
{
    if (c[0] != "audio" || c.size() < 2)
        return std::nullopt;
    const std::string& op = c[1];
    if ((op == "devices" || op == "apps" || op == "input" || op == "state") && c.size() == 2)
        return azl::Message{{"type", "audio"}, {"op", op}};
    if (op == "watch" && c.size() <= 3)
        return azl::Message{{"type", "audio"}, {"op", "subscribe"}, {"rate", c.size() == 3 ? std::stod(c[2]) : 30.0}};
    if (op == "select" && (c.size() == 3 || c.size() == 4)) {
        azl::Message m{{"type", "audio"}, {"op", "select"}, {"device", c[2] == "default" ? "" : c[2]}};
        if (c.size() == 4)
            m["channels"] = channelArg(c[3]);
        return m;
    }
    if (op == "gain" && c.size() == 3)
        return c[2] == "auto" ? azl::Message{{"type", "audio"}, {"op", "settings"}, {"autoGain", true}}
                              : azl::Message{{"type", "audio"}, {"op", "settings"}, {"autoGain", false},
                                             {"gainDb", std::stod(c[2])}};
    if (op == "latency" && c.size() == 3)
        return azl::Message{{"type", "audio"}, {"op", "settings"}, {"latencyMs", std::stod(c[2])}};
    if (op == "envelope" && c.size() == 5)
        return azl::Message{
            {"type", "audio"},
            {"op", "settings"},
            {"envelopes", {{c[2], {{"attackMs", std::stod(c[3])}, {"releaseMs", std::stod(c[4])}}}}}};
    return std::nullopt;
}

std::optional<azl::Message> djlinkMessage(const std::vector<std::string>& c)
{
    if (c[0] != "djlink")
        return std::nullopt;
    if (c.size() == 2 && c[1] == "status")
        return azl::Message{{"type", "djlink"}, {"op", "status"}};
    if (c.size() == 2 && c[1] == "off")
        return azl::Message{{"type", "djlink"}, {"op", "set"}, {"enabled", false}};
    if ((c.size() == 2 || c.size() == 3) && c[1] == "on") {
        azl::Message m{{"type", "djlink"}, {"op", "set"}, {"enabled", true}};
        if (c.size() == 3)
            m["interface"] = c[2];
        return m;
    }
    usage(2);
}

std::optional<azl::Message> mediaSyncMessage(const std::vector<std::string>& c)
{
    if (c[0] != "media")
        return std::nullopt;
    if (c.size() == 3 && c[1] == "sync" && c[2] == "status")
        return azl::Message{{"type", "mediaSync"}, {"op", "status"}};
    if (c.size() == 3 && c[1] == "sync" && (c[2] == "allow" || c[2] == "block" || c[2] == "auto"))
        return azl::Message{{"type", "mediaSync"}, {"op", "set"}, {"override", c[2]}};
    usage(2);
}

std::vector<std::string> split(const std::string& text, char by)
{
    std::vector<std::string> parts;
    for (size_t at = 0; at <= text.size();) {
        const size_t end = std::min(text.find(by, at), text.size());
        parts.push_back(text.substr(at, end - at));
        at = end + 1;
    }
    return parts;
}

azl::Message curveArg(const std::string& text)
{
    if (text == "linear")
        return {0.0, 0.0, 1.0, 1.0};
    if (text == "easeIn")
        return {0.42, 0.0, 1.0, 1.0};
    if (text == "easeOut")
        return {0.0, 0.0, 0.58, 1.0};
    if (text == "easeInOut")
        return {0.42, 0.0, 0.58, 1.0};
    const std::vector<std::string> p = split(text, ',');
    if (p.size() != 4)
        usage(2);
    return {std::stod(p[0]), std::stod(p[1]), std::stod(p[2]), std::stod(p[3])};
}

// "0:1s,0.5:0" is two points, phase:value, the first eased into the next ("s" for smooth).
azl::Message pointsArg(const std::string& text)
{
    azl::Message points = azl::Message::array();
    for (std::string point : split(text, ',')) {
        const bool smooth = !point.empty() && point.back() == 's';
        if (smooth)
            point.pop_back();
        const std::vector<std::string> p = split(point, ':');
        if (p.size() != 2)
            usage(2);
        points.push_back({{"phase", std::stod(p[0])}, {"value", std::stod(p[1])}, {"smooth", smooth}});
    }
    return points;
}

// LAYER.opacity (or another of the layer's own parameters), EFFECT.NAME or, when macros are allowed, m:MACRO. An older
// LAYER.EFFECT.NAME drops the layer.
azl::Message targetArg(const std::string& text, bool macros)
{
    if (macros && text.rfind("m:", 0) == 0)
        return {{"macro", std::stoull(text.substr(2))}};
    const std::vector<std::string> p = split(text, '.');
    if (p.size() < 2 || p.size() > 3)
        usage(2);
    const std::string& name = p.back();
    if (p.size() == 2 && (name == "opacity" || azl::textParamFromName(name)))
        return {{"layer", std::stoull(p[0])}, {"param", name}};
    return {{"effect", std::stoull(p[p.size() - 2])}, {"param", name}};
}

// A time set by hand puts a modulator without a preset on Custom, so it follows like the presets.
void setEnvelopeTime(azl::Message& m, const std::string& key, double ms)
{
    m[key] = ms;
    if (m.value("envelope", std::string("none")) == "none")
        m["envelope"] = "custom";
}

// Applies KEY=VALUE options and the positional MIN MAX [CURVE] from `from` on.
void modulatorArgs(azl::Message& m, const std::vector<std::string>& c, size_t from)
{
    std::vector<std::string> positional;
    for (size_t i = from; i < c.size(); ++i) {
        const size_t eq = c[i].find('=');
        if (eq == std::string::npos) {
            positional.push_back(c[i]);
            continue;
        }
        const std::string key = c[i].substr(0, eq);
        const std::string value = c[i].substr(eq + 1);
        if (key == "shape" || key == "feature" || key == "combine" || key == "name")
            m[key] = value;
        else if (key == "curve")
            m[key] = curveArg(value);
        else if (key == "points") {
            m[key] = pointsArg(value);
            m["shape"] = "custom";
        } else if (key == "beats")
            m[key] = beatValue(value);
        else if (key == "phase" || key == "min" || key == "max")
            m[key] = std::stod(value);
        else if (key == "attack" || key == "hold" || key == "release")
            setEnvelopeTime(m, key, std::stod(value));
        else if (key == "autorange")
            m["autoRange"] = onOff(value);
        else
            usage(2);
    }
    if (positional.size() == 1 || positional.size() > 3)
        usage(2);
    if (positional.size() >= 2) {
        m["min"] = std::stod(positional[0]);
        m["max"] = std::stod(positional[1]);
    }
    if (positional.size() == 3)
        m["curve"] = curveArg(positional[2]);
}

azl::Message edit(const azl::Message& json)
{
    return azl::msg::command(azl::editFromJson(json));
}

std::optional<azl::Message> macroMessage(const std::vector<std::string>& c)
{
    if (c[0] != "macro" || c.size() < 2)
        return std::nullopt;
    if (c[1] == "add" && c.size() <= 4)
        return edit({{"op", "addMacro"},
                     {"macro", {{"name", c.size() > 2 ? c[2] : ""}, {"group", c.size() > 3 ? std::stoull(c[3]) : 0}}}});
    if (c[1] == "rm" && c.size() == 3)
        return edit({{"op", "removeMacro"}, {"macro", std::stoull(c[2])}});
    if (c[1] == "set" && c.size() == 4)
        return edit({{"op", "setMacroValue"}, {"macro", std::stoull(c[2])}, {"value", std::stod(c[3])}});
    if (c[1] == "target" && (c.size() == 6 || c.size() == 7)) {
        azl::Message target = targetArg(c[3], false);
        target["min"] = std::stod(c[4]);
        target["max"] = std::stod(c[5]);
        if (c.size() == 7)
            target["curve"] = curveArg(c[6]);
        return edit({{"op", "addMacroTarget"}, {"macro", std::stoull(c[2])}, {"target", target}});
    }
    if (c[1] == "untarget" && c.size() == 4)
        return edit({{"op", "removeMacroTarget"}, {"macro", std::stoull(c[2])}, {"index", std::stoull(c[3])}});
    return std::nullopt;
}

std::optional<azl::Message> modMessage(const std::vector<std::string>& c)
{
    if (c[0] != "mod" || c.size() < 3)
        return std::nullopt;
    if ((c[1] == "lfo" || c[1] == "audio") && c.size() >= 4) {
        azl::Message m{{"source", c[1]}, {"target", targetArg(c[2], true)}};
        if (c[1] == "lfo" && c.size() < 5)
            usage(2);
        if (c[1] == "lfo") {
            m["shape"] = c[3];
            m["beats"] = beatValue(c[4]);
        } else {
            m["feature"] = c[3];
            m["envelope"] = "meter";
            m["autoRange"] = true;
        }
        modulatorArgs(m, c, c[1] == "lfo" ? 5 : 4);
        return edit({{"op", "addModulator"}, {"modulator", m}});
    }
    if (c[1] == "rm" && c.size() == 3)
        return edit({{"op", "removeModulator"}, {"modulator", std::stoull(c[2])}});
    return std::nullopt;
}

// "mod set" edits the modulator as the snapshot has it.
azl::Message modSet(const std::vector<std::string>& c, const azl::Message& snapshot)
{
    const uint64_t id = std::stoull(c[2]);
    for (azl::Message m : snapshot["composition"].value("modulators", azl::Message::array()))
        if (m["id"] == id) {
            modulatorArgs(m, c, 3);
            return edit({{"op", "setModulator"}, {"modulator", m}});
        }
    throw std::runtime_error("no modulator " + c[2]);
}

bool paletteCommand(const std::vector<std::string>& c)
{
    if (c[0] != "palette" || c.size() < 2)
        return false;
    const std::set<std::string> one = {"remove", "new", "delete", "select", "hold", "transition"};
    return ((c[1] == "list" || c[1] == "next") && c.size() == 2) || (one.contains(c[1]) && c.size() == 3) ||
           ((c[1] == "add" || c[1] == "set" || c[1] == "name" || c[1] == "unlink" || c[1] == "deck") &&
            c.size() == 4) ||
           (c[1] == "link" && c.size() == 5) || (c[1] == "pilot" && (c.size() == 3 || c.size() == 4));
}

azl::Message activePaletteOf(const azl::Message& snapshot)
{
    const azl::Message palettes = snapshot["composition"].value("palettes", azl::Message::array());
    if (palettes.empty())
        return {{"id", 0}, {"name", "Palette 1"}, {"colours", azl::Message::array()}};
    return palettes[azl::activePaletteIndex(snapshot["composition"].get<azl::Composition>())];
}

// A palette by its 1-based number or its name.
uint64_t paletteId(const azl::Message& snapshot, const std::string& palette)
{
    const azl::Message palettes = snapshot["composition"].value("palettes", azl::Message::array());
    for (const azl::Message& p : palettes)
        if (p.value("name", "") == palette)
            return p["id"].get<uint64_t>();
    const size_t number = std::all_of(palette.begin(), palette.end(), ::isdigit) && !palette.empty() ? std::stoull(palette) : 0;
    if (number < 1 || number > palettes.size())
        throw std::runtime_error("no palette " + palette);
    return palettes[number - 1]["id"].get<uint64_t>();
}

azl::AutopilotMode autopilotMode(const std::string& name);

// "palette new|delete|select|next|pilot|hold|transition|deck": the palettes as a whole, or none when it edits the
// active palette's colours.
std::optional<azl::Message> palettesEdit(const std::vector<std::string>& c, const azl::Message& snapshot)
{
    if (c[1] == "next")
        return azl::Message{{"type", "trigger"}, {"op", "palette"}, {"next", true}};
    if (c[1] == "select")
        return azl::Message{{"type", "trigger"}, {"op", "palette"}, {"palette", paletteId(snapshot, c[2])}};
    if (c[1] == "delete")
        return azl::msg::command(azl::RemovePalette{paletteId(snapshot, c[2])});
    if (c[1] == "deck")
        return azl::msg::command(azl::SetDeckPalette{std::stoull(c[2]), c[3] == "none" ? 0 : paletteId(snapshot, c[3])});
    if (c[1] == "hold")
        return azl::msg::command(azl::SetPaletteAutopilot{std::nullopt, std::nullopt, std::stoi(c[2])});
    if (c[1] == "transition")
        return azl::msg::command(azl::SetPaletteAutopilot{std::nullopt, std::nullopt, std::nullopt, std::stod(c[2])});
    if (c[1] == "pilot")
        return azl::msg::command(azl::SetPaletteAutopilot{
            autopilotMode(c[2]), c.size() == 4 ? std::optional(std::stoi(c[3])) : std::nullopt});
    if (c[1] != "new")
        return std::nullopt;
    azl::ColourPalette palette = activePaletteOf(snapshot).get<azl::ColourPalette>();
    palette.id = 0;
    palette.name = c[2];
    for (azl::PaletteColour& colour : palette.colours)
        colour.id = 0;
    return azl::msg::command(azl::SetPalette{palette});
}

size_t paletteSlot(const azl::Message& palette, const std::string& slot)
{
    const azl::Message& colours = palette["colours"];
    for (size_t i = 0; i < colours.size(); ++i)
        if (colours[i].value("name", "") == slot)
            return i;
    const size_t number = std::all_of(slot.begin(), slot.end(), ::isdigit) && !slot.empty() ? std::stoull(slot) : 0;
    if (number < 1 || number > colours.size())
        throw std::runtime_error("no palette colour " + slot);
    return number - 1;
}

std::array<double, 4> hexColour(const std::string& hex)
{
    const auto colour = azl::fromHex(hex);
    if (!colour)
        throw std::runtime_error("not a colour: " + hex + " (#RGB, #RRGGBB or #RRGGBBAA)");
    return azl::arrayOf(*colour);
}

std::string colourName(const azl::ColourRef& ref)
{
    if (ref.effect)
        return "effect " + std::to_string(ref.effect) + " " + ref.group;
    return "text " + std::to_string(ref.layer) + "/" + std::to_string(ref.column) + " " + ref.group;
}

int listPalette(const azl::Message& snapshot)
{
    const auto composition = snapshot["composition"].get<azl::Composition>();
    const auto linked = azl::linkedColours(composition);
    const size_t active = azl::activePaletteIndex(composition);
    for (size_t p = 0; p < composition.palettes.size(); ++p) {
        const azl::ColourPalette& palette = composition.palettes[p];
        std::printf("%s%zu %s\n", p == active ? "*" : " ", p + 1, palette.name.c_str());
        for (size_t i = 0; i < palette.colours.size(); ++i) {
            const auto& c = palette.colours[i].colour;
            std::printf("  %zu  %-20s %s\n", i + 1, palette.colours[i].name.c_str(),
                        azl::toHex(azl::rgbaOf(c), c[3] < 1.0).c_str());
            for (const auto& [ref, id] : linked)
                if (azl::linkSlot(composition, id) == i && p == active)
                    std::printf("       linked: %s\n", colourName(ref).c_str());
        }
    }
    static const char* const modes[] = {"off", "next", "random"};
    const azl::Autopilot& pilot = composition.palettePilot;
    std::printf("Autopilot %s every %d bars, hold %d, transition %g beats\n", modes[size_t(pilot.mode)], pilot.bars,
                pilot.hold, composition.paletteTransition);
    for (const azl::Deck& deck : composition.decks)
        if (const azl::ColourPalette* palette = azl::paletteById(composition, deck.palette))
            std::printf("deck %llu: %s\n", (unsigned long long)deck.id, palette->name.c_str());
    return 0;
}

// "palette add|set|name|remove|link|unlink" edits the active palette as the snapshot has it, creating it on the first
// add, or links an effect's colour group to one of its colours.
azl::Message paletteEdit(const std::vector<std::string>& c, const azl::Message& snapshot)
{
    if (auto edit = palettesEdit(c, snapshot))
        return *edit;
    azl::Message palette = activePaletteOf(snapshot);
    auto link = [](const std::string& effect, const std::string& group, azl::Message id) {
        return edit({{"op", "setPaletteLink"}, {"colour", {{"effect", std::stoull(effect)}, {"group", group}}}, {"link", id}});
    };
    if (c[1] == "unlink")
        return link(c[2], c[3], nullptr);
    if (c[1] == "link")
        return link(c[3], c[4], palette["colours"][paletteSlot(palette, c[2])].value("id", uint64_t(0)));
    azl::Message& colours = palette["colours"];
    if (c[1] == "add")
        colours.push_back({{"name", c[2]}, {"colour", hexColour(c[3])}});
    else if (c[1] == "set")
        colours[paletteSlot(palette, c[2])]["colour"] = hexColour(c[3]);
    else if (c[1] == "name")
        colours[paletteSlot(palette, c[2])]["name"] = c[3];
    else
        colours.erase(colours.begin() + ptrdiff_t(paletteSlot(palette, c[2])));
    return edit({{"op", "setPalette"}, {"palette", palette}});
}

bool stepsCommand(const std::vector<std::string>& c)
{
    if (c[0] != "fxsteps")
        return false;
    return c.size() == 2 || (c.size() == 3 && c[2] == "add") || (c.size() == 4 && (c[2] == "add" || c[2] == "rm")) ||
           (c.size() == 5 && c[2] == "move");
}

const azl::Effect& snapshotEffect(const azl::Composition& composition, const std::string& id)
{
    const azl::Effect* effect = azl::effectById(composition, std::stoull(id));
    if (!effect)
        throw std::runtime_error("no effect " + id);
    return *effect;
}

int listSteps(const std::vector<std::string>& c, const azl::Message& snapshot)
{
    const auto composition = snapshot["composition"].get<azl::Composition>();
    const azl::Effect& effect = snapshotEffect(composition, c[1]);
    static const char* const modes[] = {"off", "next", "random"};
    std::printf("%s %llu: Autopilot %s every %d bars, hold %d, transition %g beats\n", effect.type.c_str(),
                (unsigned long long)effect.id, modes[size_t(effect.autopilot.mode)], effect.autopilot.bars,
                effect.autopilot.hold, effect.transition);
    for (size_t i = 0; i < effect.steps.size(); ++i) {
        std::printf("  %zu  %s\n", i + 1, effect.steps[i].name.c_str());
        for (const auto& [name, value] : effect.steps[i].params)
            std::printf("       %s=%g\n", name.c_str(), value);
    }
    return 0;
}

// "fxsteps EFFECT add [NAME] | rm N | move N TO" edits the steps as the snapshot has them.
azl::Message stepsEdit(const std::vector<std::string>& c, const azl::Message& snapshot)
{
    const auto composition = snapshot["composition"].get<azl::Composition>();
    const azl::Effect& effect = snapshotEffect(composition, c[1]);
    std::vector<azl::EffectStep> steps = effect.steps;
    auto at = [&](const std::string& number) {
        const size_t index = stepIndex(number);
        if (index >= steps.size())
            throw std::runtime_error("no step " + number);
        return steps.begin() + ptrdiff_t(index);
    };
    if (c[2] == "add") {
        steps.push_back({c.size() == 4 ? c[3] : "Step " + std::to_string(steps.size() + 1), effect.params});
    } else if (c[2] == "rm") {
        steps.erase(at(c[3]));
    } else {
        const azl::EffectStep moved = *at(c[3]);
        steps.erase(at(c[3]));
        steps.insert(steps.begin() + ptrdiff_t(std::min(stepIndex(c[4]), steps.size())), moved);
    }
    return azl::msg::command(azl::SetEffectSteps{effect.id, steps});
}

// "modenv MODULATOR|all PRESET" puts one modulator, or every audio one in a single undo step, on the preset.
azl::Message modEnvelope(const std::vector<std::string>& c, const azl::Message& snapshot)
{
    const auto preset = azl::envelopePresetFromName(c[2]);
    if (!preset || *preset == azl::EnvelopePreset::None)
        throw std::runtime_error("no envelope " + c[2] + ": meter, punchy, smooth, breathe, gate or custom");
    const auto composition = snapshot["composition"].get<azl::Composition>();
    if (c[1] == "all")
        return azl::msg::command(azl::setEnvelopes(composition, *preset));
    for (azl::Modulator m : composition.modulators)
        if (m.id == std::stoull(c[1])) {
            azl::setEnvelope(m, *preset);
            return azl::msg::command(azl::SetModulator{m});
        }
    throw std::runtime_error("no modulator " + c[1]);
}

std::optional<azl::Message> modulationRequest(const std::vector<std::string>& c)
{
    if (c[0] == "mod" && c.size() == 2 && c[1] == "state")
        return azl::Message{{"type", "modulation"}, {"op", "state"}};
    if (c[0] != "macroset" || c.size() < 2)
        return std::nullopt;
    if (c[1] == "list" && c.size() == 2)
        return azl::Message{{"type", "modulation"}, {"op", "list"}};
    if ((c[1] == "save" || c[1] == "load") && c.size() == 3)
        return azl::Message{{"type", "modulation"}, {"op", c[1]}, {"name", c[2]}};
    return std::nullopt;
}

azl::AutopilotMode autopilotMode(const std::string& name)
{
    if (name == "off")
        return azl::AutopilotMode::Off;
    if (name == "next")
        return azl::AutopilotMode::Next;
    if (name == "random")
        return azl::AutopilotMode::Random;
    throw std::runtime_error("unknown autopilot mode " + name);
}

azl::CueEffect& cueEffect(azl::Cue& cue, uint64_t effect)
{
    for (azl::CueEffect& e : cue.effects)
        if (e.effect == effect)
            return e;
    return cue.effects.emplace_back(azl::CueEffect{effect, std::nullopt, std::nullopt});
}

// The palette Autopilot of "palettepilot:MODE[/BARS]" or "palettepilot:/BARS".
void paletteCueChange(azl::Cue& cue, const std::string& value)
{
    azl::CueEffect& palette = cueEffect(cue, azl::kPaletteEffect);
    const size_t slash = value.find('/');
    if (slash != 0)
        palette.mode = autopilotMode(value.substr(0, slash));
    if (slash != std::string::npos)
        palette.bars = std::stoi(value.substr(slash + 1));
}

// autopilot:LAYER=MODE[/BARS], autopilot:LAYER=/BARS, macro:MACRO=VALUE, effect:EFFECT=MODE[/BARS], step:EFFECT=N,
// palette:N or palettepilot:MODE[/BARS]
void addChange(azl::Cue& cue, const std::string& change)
{
    const size_t colon = change.find(':');
    const size_t equals = change.find('=');
    const std::string kind = change.substr(0, colon);
    if (kind == "palette" && colon != std::string::npos) {
        cueEffect(cue, azl::kPaletteEffect).step = stepIndex(change.substr(colon + 1));
        return;
    }
    if (kind == "palettepilot" && colon != std::string::npos)
        return paletteCueChange(cue, change.substr(colon + 1));
    if (colon == std::string::npos || (kind != "column" && (equals == std::string::npos || equals < colon)))
        throw std::runtime_error("bad cue change " + change);
    if (kind == "column") {
        const std::string target = change.substr(colon + 1);
        cue.column = target.starts_with("#") ? azl::CueColumn{0, size_t(std::stoull(target.substr(1)) - 1)}
                                             : azl::CueColumn{std::stoull(target), std::nullopt};
        return;
    }
    const uint64_t id = std::stoull(change.substr(colon + 1, equals - colon - 1));
    const std::string value = change.substr(equals + 1);
    if (kind == "macro") {
        const size_t slash = value.find('/');
        cue.macros.push_back({id, std::stod(value.substr(0, slash)),
                              slash == std::string::npos ? 0 : std::stoi(value.substr(slash + 1))});
        return;
    }
    if (kind == "step") {
        cueEffect(cue, id).step = stepIndex(value);
        return;
    }
    if (kind != "autopilot" && kind != "effect")
        throw std::runtime_error("bad cue change " + change);
    const size_t slash = value.find('/');
    if (kind == "effect") {
        azl::CueEffect& effect = cueEffect(cue, id);
        if (slash != 0)
            effect.mode = autopilotMode(value.substr(0, slash));
        if (slash != std::string::npos)
            effect.bars = std::stoi(value.substr(slash + 1));
        return;
    }
    azl::CueAutopilot autopilot{id, std::nullopt, std::nullopt};
    if (slash != 0)
        autopilot.mode = autopilotMode(value.substr(0, slash));
    if (slash != std::string::npos)
        autopilot.bars = std::stoi(value.substr(slash + 1));
    cue.autopilot.push_back(autopilot);
}

azl::Cue cueOf(const std::vector<std::string>& c)
{
    azl::Cue cue{.name = c[2]};
    const std::string& when = c[3];
    const size_t at = when.find('@');
    if (at != 0)
        cue.section = azl::sectionFromName(when.substr(0, at));
    if (at != std::string::npos)
        cue.bar = std::stoi(when.substr(at + 1));
    for (size_t i = 4; i < c.size(); ++i)
        addChange(cue, c[i]);
    return cue;
}

// Everything answered with a change: Section marks and Cue edits.
std::optional<azl::Message> cueCommand(const std::vector<std::string>& c)
{
    if (c[0] == "section" && c.size() >= 2 && c[1] == "cancel") {
        azl::Message m{{"type", "cue"}, {"op", "cancel"}};
        if (c.size() == 3)
            m["bar"] = std::stoi(c[2]);
        return m;
    }
    if (c[0] == "section" && (c.size() == 2 || c.size() == 3)) {
        azl::Message m{{"type", "cue"}, {"op", "mark"}, {"section", c[1]}};
        if (c.size() == 3)
            m["bar"] = std::stoi(c[2]);
        return m;
    }
    if (c[0] != "cue" || c.size() < 2)
        return std::nullopt;
    if (c[1] == "add" && c.size() >= 4)
        return azl::msg::command(azl::AddCue{std::nullopt, cueOf(c)});
    if (c[1] == "at" && c.size() >= 4)
        return azl::msg::command(azl::AddCue{std::nullopt, azl::cueAtWords(c)});
    if (c[1] == "rm" && c.size() == 3)
        return azl::msg::command(azl::RemoveCue{std::stoull(c[2])});
    if ((c[1] == "on" || c[1] == "off") && c.size() == 3)
        return azl::msg::command(azl::SetCueEnabled{std::stoull(c[2]), c[1] == "on"});
    if (c[1] == "every" && c.size() == 4)
        return azl::msg::command(
            azl::SetCueRepeat{std::stoull(c[2]), c[3] == "once" ? std::nullopt : std::optional(std::stoi(c[3]))});
    if (c[1] == "cancel" && c.size() == 3)
        return azl::Message{{"type", "cue"}, {"op", "cancel"}, {"cue", std::stoull(c[2])}};
    if (c[1] == "clear" && c.size() == 2)
        return azl::Message{{"type", "cue"}, {"op", "clear"}};
    return std::nullopt;
}

class Queue {
public:
    void push(azl::Message m)
    {
        std::lock_guard lock(mutex_);
        messages_.push_back(std::move(m));
        changed_.notify_all();
    }

    void close()
    {
        std::lock_guard lock(mutex_);
        closed_ = true;
        changed_.notify_all();
    }

    bool closed()
    {
        std::lock_guard lock(mutex_);
        return closed_ && messages_.empty();
    }

    // Null when the connection closed or nothing came within the timeout.
    std::optional<azl::Message> pop(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        if (!changed_.wait_for(lock, timeout, [&] { return !messages_.empty() || closed_; }) || messages_.empty())
            return std::nullopt;
        azl::Message m = std::move(messages_.front());
        messages_.pop_front();
        return m;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<azl::Message> messages_;
    bool closed_ = false;
};

void print(const azl::Message& m)
{
    std::printf("%s\n", m.dump().c_str());
    std::fflush(stdout);
}

// Says hello and waits for the snapshot; none when the engine says nothing in time. Throws with the engine's words when
// it refuses, e.g. without its pairing token.
std::optional<azl::Message> greet(azl::WebSocketClient& client, Queue& queue, const Options& opt)
{
    client.send(azl::msg::hello(opt.token));
    std::optional<azl::Message> snapshot = queue.pop(std::chrono::seconds(5));
    if (snapshot && (*snapshot)["type"] == "error")
        throw std::runtime_error(snapshot->value("message", "the engine refused the connection"));
    return snapshot;
}

int watch(azl::WebSocketClient& client, Queue& queue, const azl::Message& snapshot)
{
    azl::DocumentMirror mirror;
    mirror.receive(snapshot);
    while (auto m = queue.pop(std::chrono::hours(24))) {
        print(*m);
        if (auto request = mirror.receive(*m)) {
            std::printf("missed a change, re-requesting the snapshot\n");
            client.send(*request);
        }
    }
    std::printf("connection closed\n");
    return 1;
}

std::filesystem::path preparePath(const Options& opt)
{
    return azl::appDataFolder() / "Prepare" / (opt.host + "-" + std::to_string(opt.port) + ".json");
}

// The copy while Prepare is on for this engine.
struct Preparing {
    std::filesystem::path path;
    azl::PreparedCopy copy;
};
std::optional<Preparing> preparing;

std::optional<Preparing> loadPrepared(const Options& opt, const azl::Composition& venue)
{
    const std::filesystem::path path = preparePath(opt);
    std::ifstream in(path);
    if (!in)
        return std::nullopt;
    return Preparing{path, azl::PreparedCopy::fromJson(azl::Message::parse(in), venue)};
}

void savePrepared(const Preparing& prepared)
{
    std::filesystem::create_directories(prepared.path.parent_path());
    std::ofstream(prepared.path) << prepared.copy.toJson().dump();
}

// In Prepare an undoable edit, undo or redo changes the copy.
int prepareCommand(const azl::Message& command)
{
    const std::string op = command.value("op", "");
    if (op == "undo" || op == "redo") {
        if (!(op == "undo" ? preparing->copy.undo() : preparing->copy.redo()))
            throw std::runtime_error("nothing to " + op + " in the copy");
    } else {
        azl::Message edit = command;
        preparing->copy.edit(azl::editFromJson(edit), command.value("gesture", uint64_t(0)));
    }
    savePrepared(*preparing);
    std::printf("prepared %s (%zu steps in the copy)\n", op.c_str(), preparing->copy.undoSteps());
    return 0;
}

int prepareStatus()
{
    for (const azl::PreparedEdit& prepared : preparing->copy.edits())
        std::printf("edit %s\n", azl::editToJson(prepared.edit).dump().c_str());
    for (const std::string& skipped : preparing->copy.skipped())
        std::printf("left out %s\n", skipped.c_str());
    return 0;
}

int prepareSend(azl::WebSocketClient& client, Queue& queue, bool atSwitch)
{
    client.send({{"type", "prepare"},
                 {"op", "send"},
                 {"at", atSwitch ? "switch" : "now"},
                 {"edits", azl::preparedToJson(preparing->copy.edits())}});
    while (const auto reply = queue.pop(std::chrono::seconds(5))) {
        if ((*reply)["type"] == "error") {
            print(*reply);
            return 1;
        }
        if ((*reply)["type"] != "prepare")
            continue;
        print(*reply);
        std::filesystem::remove(preparing->path);
        return 0;
    }
    std::fprintf(stderr, "nocturne-ctl: no reply\n");
    return 1;
}

int prepare(const Options& opt, azl::WebSocketClient& client, Queue& queue, const azl::Message& snapshot)
{
    const std::vector<std::string>& c = opt.command;
    if (c[1] == "start") {
        if (preparing)
            throw std::runtime_error("already preparing; send or discard first");
        savePrepared({preparePath(opt), azl::PreparedCopy(snapshot["composition"].get<azl::Composition>())});
        std::printf("preparing: edits go to a copy until prepare send or discard\n");
        return 0;
    }
    if (!preparing)
        throw std::runtime_error("not preparing; prepare start first");
    if (c[1] == "status")
        return prepareStatus();
    return prepareSend(client, queue, c[2] == "at-switch");
}

// Waits for what the command caused: its change or an error. When verbose, other clients' changes on the way are
// printed too.
int sendCommand(azl::WebSocketClient& client, Queue& queue, const azl::Message& command, const azl::Message& self,
                bool verbose = true)
{
    if (preparing && command.value("type", "") == "command")
        return prepareCommand(command);
    client.send(command);
    while (const auto reply = queue.pop(std::chrono::seconds(5))) {
        const bool error = (*reply)["type"] == "error";
        if (verbose || error)
            print(*reply);
        if (error)
            return 1;
        if ((*reply)["client"] == self)
            return 0;
    }
    std::fprintf(stderr, "nocturne-ctl: no reply\n");
    return 1;
}

int animate(azl::WebSocketClient& client, Queue& queue, const std::vector<std::string>& c, const azl::Message& self)
{
    const uint64_t effect = std::stoull(c[1]);
    const double from = std::stod(c[3]);
    const double to = std::stod(c[4]);
    const int steps = std::max(1, int(std::stod(c[5]) * 60.0));
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i <= steps; ++i) {
        const double value = from + (to - from) * double(i) / steps;
        if (sendCommand(client, queue, azl::msg::command(azl::SetEffectParam{effect, c[2], value}), self, false))
            return 1;
        std::this_thread::sleep_until(start + std::chrono::microseconds(int64_t((i + 1) * 1e6 / 60.0)));
    }
    std::printf("%s swept from %g to %g in %d edits\n", c[2].c_str(), from, to, steps + 1);
    return 0;
}

// Adding to the library hashes the whole file, so the wait is long.
std::optional<azl::Message> requestReply(azl::WebSocketClient& client, Queue& queue, const azl::Message& request)
{
    client.send(request);
    while (const auto reply = queue.pop(std::chrono::seconds(600))) {
        print(*reply);
        if ((*reply)["type"] == request["type"] || (*reply)["type"] == "error")
            return reply;
    }
    std::fprintf(stderr, "nocturne-ctl: no reply\n");
    return std::nullopt;
}

int request(azl::WebSocketClient& client, Queue& queue, const std::vector<std::string>& c, const azl::Message& request,
            const azl::Message& self)
{
    const auto reply = requestReply(client, queue, request);
    if (!reply || (*reply)["type"] == "error")
        return 1;
    if (c[0] != "play" && c[0] != "cell")
        return 0;
    const azl::Message& entry = (*reply)["entry"];
    if (entry["status"] != "ok") {
        std::fprintf(stderr, "nocturne-ctl: not playable: %s\n", entry["reason"].get<std::string>().c_str());
        return 1;
    }
    const azl::ClipRef clip{entry["hash"], entry["name"], {}};
    if (c[0] == "cell")
        return sendCommand(client, queue, azl::msg::command(azl::SetSlot{std::stoull(c[1]), std::stoull(c[2]), clip}),
                           self);
    return sendCommand(client, queue, azl::msg::command(azl::SetLayerClip{std::stoull(c[1]), clip}), self);
}

// Loading may hash media, so the wait is long; a load then waits for its change.
int compositionRequest(azl::WebSocketClient& client, Queue& queue, const azl::Message& request,
                       const azl::Message& self)
{
    const auto reply = requestReply(client, queue, request);
    if (!reply || (*reply)["type"] == "error")
        return 1;
    if (request["op"] != "load")
        return 0;
    while (const auto change = queue.pop(std::chrono::seconds(10))) {
        if ((*change)["type"] == "error") {
            print(*change);
            return 1;
        }
        if ((*change)["type"] == "change" && (*change)["client"] == self) {
            std::printf("loaded: change %llu\n", (unsigned long long)(*change)["change"].get<uint64_t>());
            return 0;
        }
    }
    std::fprintf(stderr, "nocturne-ctl: no change\n");
    return 1;
}

// Waits for the answer to a machine-local request (same type and op); other messages on the way are printed too.
// Keeps a clip preview watch open on the cell for `seconds`, as a UI's Preview Monitor does, and reads the pictures.
int clipPreview(azl::WebSocketClient& client, Queue& queue, const std::vector<std::string>& c,
                const azl::Message& snapshot)
{
    azl::DocumentMirror mirror;
    mirror.receive(snapshot);
    const uint64_t layer = std::stoull(c[1]), column = std::stoull(c[2]);
    std::string hash;
    azl::forEachLayer(mirror.composition().layers, [&](const azl::Layer& l) {
        if (auto cell = l.clips.find(column); l.id == layer && cell != l.clips.end())
            hash = cell->second.hash;
    });
    if (hash.empty()) {
        std::fprintf(stderr, "nocturne-ctl: no clip in layer %llu column %llu\n", (unsigned long long)layer,
                     (unsigned long long)column);
        return 1;
    }
    const auto clips = requestReply(client, queue, {{"type", "preview"}, {"op", "clips"}});
    if (!clips || !clips->value("on", false)) {
        std::fprintf(stderr, "nocturne-ctl: clip previews are off; nocturne-ctl clippreviews on\n");
        return 1;
    }
    client.send({{"type", "preview"}, {"op", "watch"}, {"watch", "clips"}, {"sources", {"clip:" + hash}},
                 {"clip", {{"layer", layer}, {"column", column}}}, {"via", "surface"}, {"width", 160},
                 {"height", 90}, {"fps", 4}});
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(std::stod(c[3]));
    auto report = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    uint64_t pictures = 0, total = 0;
    while (std::chrono::steady_clock::now() < end) {
        const auto m = queue.pop(std::chrono::seconds(1));
        if (!m)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (m && (*m)["type"] == "preview" && (*m)["op"] == "frame")
            ++pictures;
        if (std::chrono::steady_clock::now() >= report) {
            std::printf("clip preview: %llu pictures in 10 s\n", (unsigned long long)pictures);
            std::fflush(stdout);
            total += pictures;
            pictures = 0;
            report += std::chrono::seconds(10);
        }
    }
    std::printf("clip preview: %llu pictures\n", (unsigned long long)(total + pictures));
    return 0;
}

bool isOutput(const azl::Message& m, const std::string& output)
{
    return m.value("display", "") == output || std::to_string(m.value("screen", uint64_t(0))) == output;
}

// Watches the return video as a Director's Machines tab does and writes one output's stream to a file.
int watchVideo(azl::WebSocketClient& client, Queue& queue, const std::vector<std::string>& c)
{
    const std::string& output = c[1];
    std::ofstream file(c[2], std::ios::binary);
    if (!file)
        throw std::runtime_error("cannot write " + c[2]);
    const double seconds = c.size() == 4 ? std::stod(c[3]) : 10.0;
    client.send({{"type", "video"}, {"op", "watch"}, {"on", true}});
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    uint64_t frames = 0, bytes = 0, all = 0;
    bool keyed = false;
    while (std::chrono::steady_clock::now() < end) {
        const auto m = queue.pop(std::chrono::seconds(1));
        if (!m) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        if ((*m)["type"] == "error") {
            print(*m);
            return 1;
        }
        if ((*m)["type"] == "video" && (*m)["op"] == "watch") {
            print(*m);
            const azl::Message outputs = m->value("outputs", azl::Message::array());
            if (std::none_of(outputs.begin(), outputs.end(), [&](const azl::Message& o) { return isOutput(o, output); })) {
                std::fprintf(stderr, "nocturne-ctl: no output %s\n", output.c_str());
                return 1;
            }
        }
        const std::optional<azl::return_video::Packet> packet = azl::return_video::packetOf(*m);
        if (!packet)
            continue;
        all += packet->data.size() + packet->config.size();
        keyed = keyed || (isOutput(*m, output) && packet->key);
        if (!keyed || !isOutput(*m, output))
            continue;
        const std::vector<uint8_t> stream = azl::return_video::annexB(*packet);
        file.write(reinterpret_cast<const char*>(stream.data()), std::streamsize(stream.size()));
        bytes += packet->data.size() + packet->config.size();
        ++frames;
    }
    client.send({{"type", "video"}, {"op", "watch"}, {"on", false}});
    std::printf("return video %s: %llu frames, %.0f kbit/s; all outputs %.0f kbit/s\n", output.c_str(),
                (unsigned long long)frames, bytes * 8.0 / 1000.0 / seconds, all * 8.0 / 1000.0 / seconds);
    return frames ? 0 : 1;
}

// Writes this machine's saved presets, defaults and chain presets as a Presets.json file.
int exportPresets(azl::WebSocketClient& client, Queue& queue, const std::string& path)
{
    client.send({{"type", "preset"}, {"op", "list"}});
    while (const auto reply = queue.pop(std::chrono::seconds(5))) {
        if ((*reply)["type"] != "preset" || (*reply)["op"] != "list")
            continue;
        azl::Message mine = azl::Message::object();
        for (const azl::Message& entry : reply->value("entries", azl::Message::array()))
            if (entry.value("group", "") == "mine")
                mine[entry.value("effectType", "")][entry.value("name", "")] = entry["params"];
        const azl::Message file{{"version", 1},
                                {"presets", mine},
                                {"defaults", reply->value("defaults", azl::Message::object())},
                                {"chains", reply->value("chains", azl::Message::object())}};
        azl::writeFileAtomically(path, file.dump(2), false);
        std::printf("exported to %s\n", path.c_str());
        return 0;
    }
    std::fprintf(stderr, "nocturne-ctl: no reply\n");
    return 1;
}

// Prints replies until the one with the request's type and `op` (default the request's).
int replyOnce(azl::WebSocketClient& client, Queue& queue, const azl::Message& request, const char* op = nullptr)
{
    client.send(request);
    while (const auto reply = queue.pop(std::chrono::seconds(5))) {
        print(*reply);
        if ((*reply)["type"] == "error")
            return 1;
        if ((*reply)["type"] == request["type"] && (*reply)["op"] == (op ? azl::Message(op) : request["op"]))
            return 0;
    }
    std::fprintf(stderr, "nocturne-ctl: no reply\n");
    return 1;
}

int clock(azl::WebSocketClient& client, Queue& queue, const std::vector<std::string>& c, const azl::Message& request)
{
    const int count = c.size() == 3 ? std::stoi(c[1]) : 1;
    const auto interval = std::chrono::duration<double>(c.size() == 3 ? std::stod(c[2]) : 0.0);
    auto next = std::chrono::steady_clock::now();
    for (int i = 0; i < count; ++i) {
        std::this_thread::sleep_until(next);
        next += std::chrono::duration_cast<std::chrono::steady_clock::duration>(interval);
        if (const int result = replyOnce(client, queue, request))
            return result;
    }
    return 0;
}

// One character per value, from ' ' to a full block.
std::string meter(double v)
{
    static const char* const kBlocks[] = {" ", "\u2581", "\u2582", "\u2583", "\u2584",
                                          "\u2585", "\u2586", "\u2587", "\u2588"};
    return kBlocks[std::clamp(int(std::lround(v * 8.0)), 0, 8)];
}

void printAudioState(const azl::Message& m, bool tty)
{
    static const char* const kBands[] = {"level", "sub", "bass", "lowMid", "mid", "highMid", "high"};
    static const char* const kDrums[] = {"kick", "snare", "hat"};
    static const char* const kSplit[] = {"lowBand", "midBand", "highBand"};
    const azl::Message& values = m["values"];
    std::string line;
    char buffer[96];
    std::snprintf(buffer, sizeof buffer, "t %9.2f  ", m["time"].get<double>());
    line += buffer;
    auto levels = [&](const auto& bands) {
        for (const char* band : bands) {
            const double v = values.value(band, 0.0);
            if (tty)
                std::snprintf(buffer, sizeof buffer, "%s %s  ", band, meter(v).c_str());
            else
                std::snprintf(buffer, sizeof buffer, "%s %.2f  ", band, v);
            line += buffer;
        }
        line += "| ";
    };
    levels(kBands);
    levels(kSplit);
    for (const char* drum : kDrums) {
        const double v = values[drum].get<double>();
        if (tty)
            std::snprintf(buffer, sizeof buffer, "%s %s %-5llu ", drum, meter(v).c_str(),
                          (unsigned long long)m["onsets"][drum].get<uint64_t>());
        else
            std::snprintf(buffer, sizeof buffer, "%s %.2f (%llu)  ", drum, v,
                          (unsigned long long)m["onsets"][drum].get<uint64_t>());
        line += buffer;
    }
    std::snprintf(buffer, sizeof buffer, "| gain %.1f dB%s%s", m["gainDb"].get<double>(),
                  m["live"].get<bool>() ? "" : "  no audio", m["silent"].get<bool>() ? "  digital silence" : "");
    line += buffer;
    std::printf(tty ? "\r%s\x1b[K" : "%s\n", line.c_str());
    std::fflush(stdout);
}

int audio(azl::WebSocketClient& client, Queue& queue, const azl::Message& request, bool json)
{
    const std::string op = request["op"];
    client.send(request);
#ifdef _WIN32
    const bool tty = _isatty(_fileno(stdout)) && !json;
#else
    const bool tty = isatty(STDOUT_FILENO) && !json;
#endif
    const auto timeout = op == "subscribe" ? std::chrono::seconds(3600) : std::chrono::seconds(10);
    while (const auto reply = queue.pop(timeout)) {
        const azl::Message& m = *reply;
        if (m["type"] == "error") {
            print(m);
            return 1;
        }
        if (m["type"] != "audio")
            continue;
        if (op == "subscribe") {
            if (m["op"] == "state")
                json ? print(m) : printAudioState(m, tty);
            else if (m["op"] == "input" && !m["event"].get<std::string>().empty())
                std::printf("%s%s\n", tty ? "\n" : "", m["event"].get<std::string>().c_str());
            continue;
        }
        if (op == "select" ? (m["op"] == "input" && m["event"].get<std::string>().rfind("select", 0) == 0)
                           : m["op"] == op) {
            print(m);
            return 0;
        }
    }
    std::fprintf(stderr, "nocturne-ctl: no reply\n");
    return 1;
}

int scrubSweep(azl::WebSocketClient& client, Queue& queue, const std::vector<std::string>& c)
{
    const uint64_t layer = std::stoull(c[1]);
    const double from = std::stod(c[2]);
    const double to = std::stod(c[3]);
    const int steps = std::max(1, int(std::stod(c[4]) * 60.0));
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i <= steps; ++i) {
        client.send(scrubMessage(layer, from + (to - from) * double(i) / steps));
        while (const auto reply = queue.pop(std::chrono::seconds(5)))
            if ((*reply)["type"] == "error" || (*reply)["op"] == "scrub")
                break;
        std::this_thread::sleep_until(start + std::chrono::microseconds(int64_t((i + 1) * 1e6 / 60.0)));
    }
    std::printf("layer %llu scrubbed from %g to %g in %d steps\n", (unsigned long long)layer, from, to, steps + 1);
    return 0;
}

const azl::Message* cellOf(const azl::Message& layers, uint64_t layer, uint64_t column)
{
    for (const azl::Message& l : layers) {
        if (l.value("id", uint64_t(0)) == layer) {
            const azl::Message& clips = l["clips"];
            auto it = clips.find(std::to_string(column));
            return it == clips.end() ? nullptr : &*it;
        }
        if (l.contains("layers"))
            if (const azl::Message* found = cellOf(l["layers"], layer, column))
                return found;
    }
    return nullptr;
}

void cellsWith(const azl::Message& layers, const std::string& hash, std::vector<azl::Cell>& cells)
{
    for (const azl::Message& l : layers) {
        for (const auto& [column, clip] : l["clips"].items())
            if (clip.value("hash", "") == hash)
                cells.push_back({l["id"], std::stoull(column), clip.get<azl::ClipRef>()});
        if (l.contains("layers"))
            cellsWith(l["layers"], hash, cells);
    }
}

// Media found again under its hash just plays; other content at the path replaces the hash in every cell holding it,
// in one gesture, so one undo takes it back.
int relink(azl::WebSocketClient& client, Queue& queue, const std::vector<std::string>& c, const azl::Message& snapshot)
{
    const azl::Message& layers = snapshot["composition"]["layers"];
    const azl::Message* cell = cellOf(layers, std::stoull(c[1]), std::stoull(c[2]));
    if (!cell)
        throw std::runtime_error("no clip in that cell");
    const std::string hash = (*cell)["hash"];
    azl::Message m{{"type", "library"}, {"op", "relink"}, {"hash", hash}};
    if (c.size() == 4)
        m["path"] = c[3];
    const auto reply = requestReply(client, queue, m);
    if (!reply || (*reply)["type"] == "error")
        return 1;
    const azl::Message& entry = (*reply)["entry"];
    if (entry["hash"] == hash)
        return 0;
    std::vector<azl::Cell> cells;
    cellsWith(layers, hash, cells);
    const uint64_t gesture = uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()) | 1;
    for (azl::Cell& found : cells) {
        found.clip.hash = entry["hash"];
        found.clip.name = entry["name"];
        azl::Message command = azl::msg::command(azl::SetSlot{found.layer, found.column, found.clip});
        command["gesture"] = gesture;
        if (sendCommand(client, queue, command, snapshot["client"]))
            return 1;
    }
    return 0;
}

azl::Gig findGig(const std::string& venue, const std::optional<std::string>& date)
{
    const std::filesystem::path folder = azl::defaultGigsFolder();
    if (date)
        return azl::loadGig(azl::gigPath(folder, venue, *date));
    if (auto gig = azl::currentGig(azl::venueGigs(folder, venue), azl::unixTime()))
        return *gig;
    throw std::runtime_error("no Gig ahead for " + venue + " in " + (folder / venue).string());
}

// Applies the Gig to the venue's composition as it is now, in one undo step.
int applyGig(const Options& opt, const azl::Gig& gig)
{
    Queue queue;
    azl::WebSocketClient client(opt.host, opt.port, opt.encoding, [&](const azl::Message& m) { queue.push(m); },
                                [&] { queue.close(); });
    const auto snapshot = greet(client, queue, opt);
    if (!snapshot) {
        std::fprintf(stderr, "nocturne-ctl: no snapshot\n");
        return 1;
    }
    const azl::Composition composition = (*snapshot)["composition"].get<azl::Composition>();
    return sendCommand(client, queue, azl::msg::command(azl::gigEdit(gig, composition)), (*snapshot)["client"]);
}

int gig(const Options& opt, const std::vector<std::string>& c)
{
    const azl::GigCommand command = azl::gigCommandWords(c);
    if (command.op == "set") {
        azl::Gig gig = command.gig;
        gig.venue = opt.host;
        azl::saveGig(azl::gigPath(azl::defaultGigsFolder(), gig.venue, gig.date), gig);
        return applyGig(opt, gig);
    }
    azl::Gig gig = findGig(opt.host, command.date);
    if (command.op == "show") {
        std::printf("%s\n", azl::Message(gig).dump(2).c_str());
        return 0;
    }
    if (command.op == "shift") {
        azl::shiftSlot(gig, command.slot, command.minutes, command.following);
        azl::saveGig(azl::gigPath(azl::defaultGigsFolder(), opt.host, gig.date), gig);
    }
    return applyGig(opt, gig);
}

// Sends `request` and waits for its answer: a message of its type and op, or an error.
std::optional<azl::Message> ask(azl::WebSocketClient& client, Queue& queue, const azl::Message& request)
{
    client.send(request);
    while (auto reply = queue.pop(std::chrono::seconds(30)))
        if ((*reply)["type"] == "error" || ((*reply)["type"] == request["type"] && (*reply)["op"] == request["op"]))
            return reply;
    return std::nullopt;
}

azl::Message lastUpdate(const azl::Message& status)
{
    return status.value("update", azl::Message::object()).value("last", azl::Message());
}

// The installer's result once the engine is back with one other than `before`; none if the engine stays away.
std::optional<azl::Message> awaitUpdate(const Options& opt, const azl::Message& before)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::minutes(4);
    while (std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        try {
            Queue queue;
            azl::WebSocketClient client(opt.host, opt.port, opt.encoding, [&](const azl::Message& m) { queue.push(m); },
                                        [&] { queue.close(); });
            if (!greet(client, queue, opt))
                continue;
            const std::optional<azl::Message> status = ask(client, queue, {{"type", "machine"}, {"op", "status"}});
            if (status && (*status)["type"] == "machine" && lastUpdate(*status) != before)
                return lastUpdate(*status);
        } catch (const std::exception&) {
        }
    }
    return std::nullopt;
}

// Pushes Nocturne.app to the engine machine, where tools/update-bundle.sh installs it, and waits for its result.
int update(const Options& opt, const std::string& bundle)
{
    azl::BundlePush push(bundle);
    std::printf("packed %s: build %s, %.1f MB\n", bundle.c_str(), push.build().c_str(), double(push.size()) / 1e6);
    std::fflush(stdout);
    azl::Message before;
    {
        Queue queue;
        azl::WebSocketClient client(opt.host, opt.port, opt.encoding, [&](const azl::Message& m) { queue.push(m); },
                                    [&] { queue.close(); });
        if (!greet(client, queue, opt)) {
            std::fprintf(stderr, "nocturne-ctl: no snapshot\n");
            return 1;
        }
        if (const auto status = ask(client, queue, {{"type", "machine"}, {"op", "status"}}))
            before = lastUpdate(*status);
        std::optional<azl::Message> reply = ask(client, queue, push.start());
        int shown = 0;
        while (reply && (*reply)["type"] == "machine" && (*reply)["state"] == "receiving") {
            if (const int percent = int((*reply)["received"].get<double>() * 100.0 / double(push.size()));
                percent >= shown + 10) {
                shown = percent - percent % 10;
                std::printf("sent %d%%\n", shown);
                std::fflush(stdout);
            }
            const std::optional<azl::Message> part = push.next();
            reply = part ? ask(client, queue, *part) : std::nullopt;
        }
        if (!reply) {
            std::fprintf(stderr, "nocturne-ctl: the engine stopped answering during the update\n");
            return 1;
        }
        print(*reply);
        if ((*reply)["type"] == "error")
            return 1;
    }
    std::printf("installing; waiting for the engine to come back\n");
    std::fflush(stdout);
    const std::optional<azl::Message> result = awaitUpdate(opt, before);
    if (!result) {
        std::fprintf(stderr, "nocturne-ctl: no update result from the engine within 4 minutes\n");
        return 1;
    }
    print(*result);
    return result->value("result", "") == "updated" ? 0 : 1;
}

bool keepLocalCommand(const std::vector<std::string>& c)
{
    static const std::set<std::string> ops = {"keep", "drop", "follow", "local", "fetch"};
    return c[0] == "media" && c.size() >= 2 && ops.contains(c[1]);
}

// The venue's open show as KeepLocal names it, and the media its composition uses.
std::pair<std::string, std::set<std::string>> openShow(const Options& opt)
{
    Queue queue;
    azl::WebSocketClient client(opt.host, opt.port, opt.encoding, [&](const azl::Message& m) { queue.push(m); },
                                [&] { queue.close(); });
    const auto snapshot = greet(client, queue, opt);
    if (!snapshot)
        throw std::runtime_error("no snapshot from " + opt.host);
    const std::string file = snapshot->value("file", azl::Message::object()).value("path", "");
    if (file.empty())
        throw std::runtime_error("the venue's show was never saved");
    const azl::Composition composition = (*snapshot)["composition"].get<azl::Composition>();
    std::set<std::string> media;
    azl::forEachLayer(composition.layers, [&](const azl::Layer& layer) {
        for (const auto& [column, clip] : layer.clips)
            if (!clip.hash.empty())
                media.insert(clip.hash);
    });
    return {azl::showKey(opt.host, file), media};
}

std::string mediaHash(const std::string& text)
{
    if (!azl::isMediaHash(text))
        throw std::runtime_error("not a media hash: " + text);
    return text;
}

int keepLocal(const Options& opt, const std::vector<std::string>& c)
{
    azl::KeepLocal keep(azl::defaultKeepLocalFile());
    if (c[1] == "local" && c.size() == 2) {
        for (const auto& [hash, kept] : keep.clips())
            std::printf("%s\t%s\n", hash.c_str(), kept ? "keep" : "drop");
        for (const auto& [show, media] : keep.shows())
            std::printf("%s\tshow, %zu clips\n", show.c_str(), media.size());
        return 0;
    }
    if (c.size() != 3)
        usage(2);
    if (c[1] == "fetch") {
        const std::string hash = mediaHash(c[2]);
        azl::S3Client client(azl::loadR2Config(azl::defaultR2ConfigPath()));
        const std::filesystem::path file = azl::fetchedFile(azl::defaultFetchFolder(), hash);
        azl::fetchObject(client, hash, file);
        std::printf("%s\n", file.string().c_str());
        return 0;
    }
    if (c[2] == "show" && c[1] != "follow") {
        const auto [show, media] = openShow(opt);
        keep.setShow(show, c[1] == "keep", media);
        std::printf("%s %s, %zu clips\n", c[1] == "keep" ? "keeping" : "not keeping", show.c_str(), media.size());
        return 0;
    }
    const std::string hash = mediaHash(c[2]);
    keep.setClip(hash, c[1] == "follow" ? std::nullopt : std::optional(c[1] == "keep"));
    return 0;
}

bool uploadCommand(const std::vector<std::string>& c)
{
    return c[0] == "media" && ((c.size() >= 3 && c[1] == "upload") || (c.size() >= 2 && c[1] == "status"));
}

std::string uploadStatus(const std::optional<azl::UploadState>& state, bool local)
{
    if (!state)
        return local ? "local only" : "not on this machine";
    char progress[64] = "";
    if (state->kind != azl::UploadState::Kind::Uploaded && state->size)
        std::snprintf(progress, sizeof progress, " %d%% (%.1f of %.1f MB)", int(state->bytes * 100 / state->size),
                      double(state->bytes) / 1e6, double(state->size) / 1e6);
    return azl::uploadStateName(state->kind) + std::string(progress) + (state->error.empty() ? "" : ": " + state->error);
}

int upload(const std::vector<std::string>& c)
{
    if (c[1] == "upload") {
        azl::S3Client client(azl::loadR2Config(azl::defaultR2ConfigPath()));
        const azl::RunOptions options{.manifest = azl::newManifestPath()};
        const nlohmann::json manifest =
            azl::runUpload(&client, azl::mediaSources({c.begin() + 2, c.end()}), options);
        int failed = 0;
        for (const nlohmann::json& file : manifest["files"]) {
            failed += file["status"] == "failed";
            std::puts(azl::manifestLine(file).c_str());
        }
        return failed ? 1 : 0;
    }
    const azl::UploadStates states(azl::defaultUploadStatesFile());
    std::map<std::string, std::string> local;
    for (const azl::MediaEntry& entry : azl::MediaLibrary(azl::MediaLibrary::defaultRoot()).entries())
        if (entry.status == azl::MediaStatus::Ok)
            local[entry.hash] = entry.name;
    std::set<std::string> hashes;
    for (size_t i = 2; i < c.size(); ++i)
        hashes.insert(mediaHash(c[i]));
    if (c.size() == 2) {
        for (const auto& [hash, name] : local)
            hashes.insert(hash);
        for (const auto& [hash, state] : states.all())
            hashes.insert(hash);
    }
    for (const std::string& hash : hashes) {
        const std::optional<azl::UploadState> state = states.find(hash);
        const std::string name = local.contains(hash) ? local[hash] : state ? state->source : "";
        std::printf("%s\t%s\t%s\n", hash.c_str(), uploadStatus(state, local.contains(hash)).c_str(), name.c_str());
    }
    return 0;
}

volatile std::sig_atomic_t interrupted = 0;

// The header of a WAV file of mono 32-bit float samples at 48 kHz; the largest length while it is not known.
void writeWavHeader(FILE* out, uint32_t samples)
{
    const uint32_t data = samples == UINT32_MAX ? UINT32_MAX - 64 : samples * 4;
    const uint32_t rate = uint32_t(azl::kOpusRate);
    struct __attribute__((packed)) {
        char riff[4] = {'R', 'I', 'F', 'F'};
        uint32_t size;
        char wave[8] = {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '};
        uint32_t fmtSize = 16;
        uint16_t format = 3;
        uint16_t channels = 1;
        uint32_t sampleRate;
        uint32_t byteRate;
        uint16_t align = 4;
        uint16_t bits = 32;
        char dataId[4] = {'d', 'a', 't', 'a'};
        uint32_t dataSize;
    } header;
    header.size = data + 36;
    header.sampleRate = rate;
    header.byteRate = rate * 4;
    header.dataSize = data;
    std::fwrite(&header, sizeof header, 1, out);
}

int listen(const Options& opt, const std::vector<std::string>& c)
{
    const double seconds = c.size() > 1 ? std::stod(c[1]) : 0.0;
    const std::string path = c.size() > 2 ? c[2] : "-";
    FILE* out = path == "-" ? stdout : std::fopen(path.c_str(), "wb");
    if (!out)
        throw std::runtime_error("cannot write " + path);
    Queue queue;
    azl::WebSocketClient client(opt.host, opt.port, opt.encoding, [&](const azl::Message& m) { queue.push(m); },
                                [&] { queue.close(); });
    if (!greet(client, queue, opt))
        throw std::runtime_error("no snapshot");
    client.send({{"type", "listen"}, {"op", "start"}});
    std::signal(SIGINT, [](int) { interrupted = 1; });
    writeWavHeader(out, UINT32_MAX);

    azl::OpusStreamDecoder decoder;
    std::vector<float> pcm;
    uint64_t samples = 0, bytes = 0, second = 0, lost = 0;
    std::optional<uint64_t> next;
    const auto start = std::chrono::steady_clock::now();
    auto tick = start + std::chrono::seconds(1);
    while (!interrupted && !queue.closed() &&
           (seconds <= 0.0 || std::chrono::steady_clock::now() - start < std::chrono::duration<double>(seconds))) {
        if (const auto now = std::chrono::steady_clock::now(); now >= tick) {
            std::fprintf(stderr, "listen: %.1f kbit/s, %llu lost\n", double(second) * 8.0 / 1000.0,
                         (unsigned long long)lost);
            second = 0;
            tick += std::chrono::seconds(1);
        }
        const auto m = queue.pop(std::chrono::milliseconds(100));
        if (!m)
            continue;
        if ((*m)["type"] == "error") {
            std::fprintf(stderr, "nocturne-ctl: %s\n", (*m).value("message", "").c_str());
            return 1;
        }
        if ((*m)["type"] != "listen" || (*m)["op"] != "audio")
            continue;
        const uint64_t seq = (*m)["seq"];
        if (next && seq > *next)
            lost += (seq - *next) * (*m)["sizes"].size();
        next = seq + 1;
        const uint64_t size = azl::encode(*m, azl::Encoding::MessagePack).size();
        bytes += size;
        second += size;
        const auto& payload = (*m)[azl::kReturnPayload].get_binary();
        size_t offset = 0;
        for (const size_t packet : (*m)["sizes"]) {
            pcm.clear();
            decoder.decode(payload.data() + offset, packet, pcm);
            offset += packet;
            std::fwrite(pcm.data(), sizeof(float), pcm.size(), out);
            samples += pcm.size();
        }
        std::fflush(out);
    }
    client.send({{"type", "listen"}, {"op", "stop"}});
    if (out != stdout) {
        std::fseek(out, 0, SEEK_SET);
        writeWavHeader(out, uint32_t(samples));
        std::fclose(out);
    }
    const double heard = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::fprintf(stderr, "listen: %.1f s of audio in %.1f s, %.1f kbit/s, %llu packets lost\n",
                 double(samples) / azl::kOpusRate, heard, double(bytes) * 8.0 / 1000.0 / heard,
                 (unsigned long long)lost);
    return 0;
}

int switches(const Options& opt, const std::vector<std::string>& c)
{
    azl::Message request{{"type", "switches"}, {"op", "list"}};
    if (c.size() == 2)
        request["date"] = c[1];
    Queue queue;
    azl::WebSocketClient client(opt.host, opt.port, opt.encoding, [&](const azl::Message& m) { queue.push(m); },
                                [&] { queue.close(); });
    azl::Message hello = azl::msg::hello(opt.token);
    hello["watch"] = false;
    client.send(hello);
    client.send(request);
    while (const auto reply = queue.pop(std::chrono::seconds(5))) {
        if ((*reply)["type"] == "error") {
            std::fprintf(stderr, "nocturne-ctl: %s\n", reply->value("message", "").c_str());
            return 1;
        }
        if ((*reply)["type"] != "switches" || (*reply)["op"] != "list")
            continue;
        if (opt.encoding == azl::Encoding::Json) {
            print(*reply);
            return 0;
        }
        std::printf("switches of %s:\n", reply->value("date", "").c_str());
        for (const azl::Message& line : (*reply)["switches"])
            std::printf("  %s\n", azl::switchText(line).c_str());
        return 0;
    }
    std::fprintf(stderr, "nocturne-ctl: no reply\n");
    return 1;
}

int run(const Options& opt)
{
    const std::vector<std::string>& c = opt.command;
    if (c[0] == "listen" && c.size() <= 3)
        return listen(opt, c);
    if (c[0] == "gig")
        return gig(opt, c);
    if (c[0] == "switches" && c.size() <= 2)
        return switches(opt, c);
    if (c[0] == "machine" && c.size() == 3 && c[1] == "update")
        return update(opt, c[2]);
    if (keepLocalCommand(c))
        return keepLocal(opt, c);
    if (uploadCommand(c))
        return upload(c);
    if (c[0] == "showmedia" && (c.size() == 2 || c.size() == 3))
        return showMedia(c);
    if (c[0] == "nodes" && c.size() == 1)
        return listNodes();
    if (c[0] == "venues" && c.size() == 1)
        return listVenues(opt);
    if (c[0] == "token" && c.size() <= 2) {
        const std::filesystem::path state = c.size() == 2 ? std::filesystem::path(c[1]) : azl::defaultStateFolder();
        std::printf("%s\n", azl::machineToken(azl::tokenFile(state)).c_str());
        return 0;
    }
    if (c[0] == "prepare" && c.size() == 2 && c[1] == "discard") {
        std::filesystem::remove(preparePath(opt));
        std::printf("discarded the copy\n");
        return 0;
    }
    const bool prepareRequest = c[0] == "prepare" && ((c.size() == 2 && (c[1] == "start" || c[1] == "status")) ||
                                                      (c.size() == 3 && c[1] == "send" &&
                                                       (c[2] == "now" || c[2] == "at-switch")));
    const std::set<std::string> nodeOps = {"status", "promote", "demote", "adopt"};
    const bool nodeRequest = c[0] == "node" && c.size() == 2 && nodeOps.count(c[1]);
    std::optional<azl::Message> command = commandMessage(c);
    if (!command)
        command = macroMessage(c);
    if (!command)
        command = modMessage(c);
    if (!command)
        command = cueCommand(c);
    const bool cueState = c[0] == "cue" && c.size() == 2 && c[1] == "state";
    const std::optional<azl::Message> local = requestMessage(c);
    const std::optional<azl::Message> clockRequest = clockMessage(c);
    const std::optional<azl::Message> preset = presetMessage(c);
    const std::optional<azl::Message> audioRequest = audioMessage(c);
    const std::optional<azl::Message> djlink = djlinkMessage(c);
    const std::optional<azl::Message> mediaSync = mediaSyncMessage(c);
    const std::optional<azl::Message> isf = isfMessage(c);
    const std::optional<azl::Message> modulation = modulationRequest(c);
    const std::optional<azl::Message> composition = compositionMessage(c);
    const std::optional<azl::Message> record = recordMessage(c);
    const std::optional<azl::Message> settings = settingsMessage(c);
    const bool sweep = c[0] == "animate" && c.size() == 6;
    const bool modulatorSet = c[0] == "mod" && c.size() >= 4 && c[1] == "set";
    const bool envelopeSet = c[0] == "modenv" && c.size() == 3;
    const bool relinkRequest = c[0] == "relink" && (c.size() == 3 || c.size() == 4);
    const bool clipPreviewRequest = c[0] == "clippreview" && c.size() == 4;
    const bool videoWatch = c[0] == "watch" && (c.size() == 3 || c.size() == 4);
    const bool palette = paletteCommand(c);
    const bool steps = stepsCommand(c);
    const bool presetExport = c[0] == "preset" && c.size() == 3 && c[1] == "export";
    const bool saveChain = c[0] == "preset" && (c.size() == 4 || c.size() == 5) && c[1] == "savechain";
    const std::optional<azl::Message> machine = machineMessage(c);
    if (!command && !local && !clockRequest && !preset && !sweep && !audioRequest && !modulation && !modulatorSet &&
        !envelopeSet && !composition && !record && !settings && !cueState && !nodeRequest && !relinkRequest &&
        !clipPreviewRequest && !videoWatch && !palette && !steps && !presetExport && !saveChain && !djlink && !mediaSync && !isf &&
        !machine && !prepareRequest && !(c.size() == 1 && (c[0] == "watch" || c[0] == "snapshot")))
        usage(2);

    Queue queue;
    azl::WebSocketClient client(opt.host, opt.port, opt.encoding, [&](const azl::Message& m) { queue.push(m); },
                                [&] { queue.close(); });
    auto snapshot = greet(client, queue, opt);
    if (!snapshot) {
        std::fprintf(stderr, "nocturne-ctl: no snapshot\n");
        return 1;
    }
    preparing = loadPrepared(opt, (*snapshot)["composition"].get<azl::Composition>());
    if (prepareRequest)
        return prepare(opt, client, queue, *snapshot);
    if (preparing)
        (*snapshot)["composition"] = preparing->copy.composition();
    if (audioRequest)
        return audio(client, queue, *audioRequest, opt.encoding == azl::Encoding::Json);
    if (djlink)
        return replyOnce(client, queue, *djlink, "status");
    if (mediaSync)
        return replyOnce(client, queue, *mediaSync, "status");
    if (isf)
        return replyOnce(client, queue, *isf);
    if (machine)
        return replyOnce(client, queue, *machine);
    if (videoWatch)
        return watchVideo(client, queue, c);
    if (palette && c[1] == "list")
        return listPalette(*snapshot);
    if (steps && c.size() == 2)
        return listSteps(c, *snapshot);
    if (presetExport)
        return exportPresets(client, queue, c[2]);
    if (saveChain)
        return replyOnce(client, queue, saveChainMessage(c, *snapshot));
    print(*snapshot);
    if (palette)
        return sendCommand(client, queue, paletteEdit(c, *snapshot), (*snapshot)["client"]);
    if (steps)
        return sendCommand(client, queue, stepsEdit(c, *snapshot), (*snapshot)["client"]);
    if (relinkRequest)
        return relink(client, queue, c, *snapshot);
    if (clipPreviewRequest)
        return clipPreview(client, queue, c, *snapshot);
    if (command)
        return sendCommand(client, queue, *command, (*snapshot)["client"]);
    if (local)
        return request(client, queue, c, *local, (*snapshot)["client"]);
    if (composition)
        return compositionRequest(client, queue, *composition, (*snapshot)["client"]);
    if (clockRequest && c[0] == "scrub" && c.size() == 5)
        return scrubSweep(client, queue, c);
    if (clockRequest)
        return clock(client, queue, c, *clockRequest);
    if (preset && (*preset)["op"] == "apply")
        return sendCommand(client, queue, *preset, (*snapshot)["client"]);
    if (preset)
        return replyOnce(client, queue, *preset);
    if (sweep)
        return animate(client, queue, c, (*snapshot)["client"]);
    if (modulatorSet)
        return sendCommand(client, queue, modSet(c, *snapshot), (*snapshot)["client"]);
    if (envelopeSet)
        return sendCommand(client, queue, modEnvelope(c, *snapshot), (*snapshot)["client"]);
    if (modulation && (*modulation)["op"] == "load")
        return sendCommand(client, queue, *modulation, (*snapshot)["client"]);
    if (modulation)
        return replyOnce(client, queue, *modulation);
    if (cueState)
        return replyOnce(client, queue, {{"type", "cue"}, {"op", "state"}});
    if (nodeRequest)
        return replyOnce(client, queue, {{"type", "node"}, {"op", c[1]}});
    if (record)
        return replyOnce(client, queue, *record);
    if (settings)
        return replyOnce(client, queue, *settings);
    return c[0] == "watch" ? watch(client, queue, *snapshot) : 0;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        return run(parse(argc, argv));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "nocturne-ctl: %s\n", e.what());
        return 1;
    }
}
