# Nocturne

A live VJ instrument: clips are composed into a picture, and that picture is mapped onto screens, projectors and LED fixtures across one or more machines. Terms follow Resolume Arena wherever Resolume already has a name for the concept.

## Content

**Composition**:
The whole show document: layers, groups, columns, decks, tempo and the screen setup.
_Avoid_: project, show file, set

**Deck**:
A page of columns within a composition, switched as a whole. Only a time-of-day Cue (a Gig slot, a lineup at 03:00) or a hand (the UI, a manual ctl command, the Venue desk) switches the deck shown; no Section or bar Cue, Autopilot, macro, modulator or prepared change does, and a Node only follows its main's switch (ADR 0013).
_Avoid_: page, bank

**Column**:
A vertical slot across all layers; triggering it starts the clip of every layer in that column.
_Avoid_: scene, row

**Layer**:
A track that plays at most one clip at a time and is blended over the layers below it.
_Avoid_: track, channel

**Group**:
A set of layers blended together first and then blended into the composition as one layer.
_Avoid_: folder, bus

**Clip**:
A media source (video file, generator, live input) with its own transport and effects, placed in a layer cell.
_Avoid_: cue, media item

**Media Library**:
The machine-wide store of media files, identified by content rather than by file path.
_Avoid_: media folder, assets

**Effect**:
A processing step on a clip, layer, group or the composition that turns an input picture into an output picture.
_Avoid_: filter, FX plugin

**Crossfader**:
The A/B fader between layers assigned to side A and side B.

## Time

**Transport**:
How a clip moves through its media: play direction, play mode (loop, bounce, Random Jump, ...) and sync mode (timeline or BPM).
_Avoid_: playback, timeline mode

**Random Jump**:
A play mode where the clip jumps by a random whole number of beats, up to the Distance, every Interval beats.

**Trigger**:
Starting a clip, a column or a deck from the clip grid, live; it lands on the next quantised beat.
_Avoid_: launch, fire

**Quantisation**:
The beat grid a trigger waits for: off, a beat, half a bar, or N bars (the next bar by default).
_Avoid_: beat snap

**Autopilot**:
A layer moving on by itself to the next or a random clip of its deck at the end of every step: a number of bars set once on the layer, which a clip can override. A short clip loops until the step ends; a long one is cut.
_Avoid_: auto-advance, playlist, loop count

**Transition**:
How a layer changes from one clip to the next over a number of beats on the beat clock (0 is a cut): alpha (a crossfade), wipe, dissolve or push. Set on the layer; a clip can override the length. Both clips play, each through its own clip effects and then the layer's effect chain, until it ends.
_Avoid_: crossfade (that is the alpha mode), fade time

**Section**:
A musical part of the current track: intro, build, drop, breakdown or outro.
_Avoid_: phrase, part

**Cue**:
A change scheduled at a musical position (a bar or a section) or at a time of day, so every engine applies it at the same moment without anyone pressing a button live. Only a time-of-day Cue can show a deck. A Cue is scheduled; a Trigger is live.
_Avoid_: event

**Beat Clock**:
The shared tempo of the composition: BPM, beat phase and bar position, which every BPM-synced transport and modulator follows.
_Avoid_: tempo controller, master clock

## Control

**Macro**:
A named high-level control (e.g. Energy) that drives many parameters at once, each through its own curve.
_Avoid_: master knob, dashboard control

**Macro Set**:
A saved, reusable set of macros with their targets, curves and modulators, loadable into any composition.
_Avoid_: dashboard preset, macro preset

**Modulator**:
A moving source (beat LFO, audio feature, MIDI/OSC knob) that drives a macro or a parameter.
_Avoid_: phase source, automation, LFO (as the general term)

**Envelope preset**:
How an audio modulator follows its feature: Meter, Punchy, Smooth, Breathe, Gate or Custom (attack, hold, release);
falls linearly in dB like a level meter. A modulator from before presets has none and is "raw" when unsmoothed.
_Avoid_: smoothing preset, ballistics (outside the tuning notes)

## Mapping

**Screen**:
One destination surface in the screen setup, with its own slices, masks and output.
_Avoid_: display, canvas

**Slice**:
A region of an input picture (the composition, a layer or a group) placed and warped onto a screen.
_Avoid_: surface, region

**Mask**:
A shape on a screen or slice that hides part of the picture.

**Output**:
The physical or network sink a screen is sent to: a display, a DMX lumiverse, a laser, or a network stream.
_Avoid_: device, sink

## Machines

**Engine**:
The headless process that renders a composition and drives outputs; it runs on every machine.
_Avoid_: server, backend

**Venue**:
A place with its own show machine, and so its own composition, media, beat clock and outputs. It runs alone; nothing in it depends on the Director or the internet during a gig.
_Avoid_: site, location

**Director**:
The operator's machine running the UI and the show tools; it connects to one venue at a time and edits it directly. There is no main engine.
_Avoid_: master, controller, server

**Gig**:
One night at a venue, with its date, time window and lineup; the lineup becomes deck-switch Cues on the venue's machine.
_Avoid_: event (reserved against Cue)

**Venue desk**:
A small web page the venue machine serves on the venue LAN, for anyone in the booth on a phone: tonight's lineup, which DJ's deck is live and what switches next. Its own process (`nocturne-desk`), started by the Supervisor; it never touches the picture.
_Avoid_: remote, control panel, DJ link (that is Pro DJ Link)

**Node Sync**:
Following another engine's composition and beat clock without owning it. Only the UI's local preview follower uses it now: it mirrors the venue engine so the Director renders its own previews.
_Avoid_: send, export, deploy

**Node**:
An engine that follows another engine through Node Sync instead of owning its composition: today only the UI's local preview follower.
_Avoid_: slave, client, render server

**Supervisor**:
The small process that runs the engine on a machine, restarts it after a crash or a hang, and after repeated crashes switches to safe mode: black or a fallback image with the reason shown, nothing playing.
_Avoid_: watchdog (for the process), daemon
