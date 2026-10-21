# Cues

A **Cue** is a change scheduled ahead: at a musical position (a Section such as intro, build, drop, breakdown, outro, or
a bar of the beat clock) or at a time of day. Every engine applies it on the same beat, and nobody has to press a button
live. A Cue is scheduled; a Trigger is live. What a Section Cue sets holds until its Section ends, then the
composition's own values come back.

## The sentence

The Cues tab (bottom middle pane) shows each Cue as one sentence:

    WHEN  →  DO  →  ON WHAT
    When the Drop starts → fire → column 1 of this deck

- **WHEN**: a Section and the bar inside it ("When the Drop starts", "On bar 9 of the Build"), a bar of the beat clock
  ("On bar 17 of the beat clock"), or a time of day ("At 03:00"). A musical Cue fires once, or again every N bars from
  there ("When the Drop starts, every 16 bars"). A time-of-day Cue fires on the first bar after that time on the
  venue machine's clock, every day.
- **DO**: fire a column, set a layer's Autopilot, move a macro to a value (at once or over a few bars), or step an
  effect. A time-of-day Cue can also show a deck; it then only shows a deck and fires a column.
- **ON WHAT**: the column, layer, macro, effect or deck. A column is picked by deck: first "This deck (the one in view
  when it fires)", which picks a column by its position in whatever deck is shown at that moment, then each deck by
  name with its columns by name. Long lists page into submenus, so no menu is taller than the screen.

On the right of each Cue: when it fires next ("next: in 3 bars", "at 03:00"), "waits for the Drop" until a Drop is
marked, or "off". A Cue lights up for the bar it fires in. Sections are marked live with the Intro to Outro buttons in
the transport bar; a mark lands on the next bar.

Click a Cue to open its builder under it. Each part of the sentence is a menu, and each pick changes the Cue at once (one
undo step). "+ Add cue" opens the builder on a new Cue, which is added with Add. The square switches a Cue off and on
(one undo step; an off Cue never fires), and the cross removes it.

A Gig's lineup Cues sit under a header row for that Gig, after the hand-made ones. Click the header or one of its Cues
to edit the lineup in the Gig editor; the Gig writes those Cues itself.

## Who switches the deck

Only a time-of-day Cue (a Gig slot, or a lineup Cue at 03:00) or a hand (the deck tabs in the UI, or a manual
`nocturne-ctl` command) switches the deck shown. Nothing else does: no Section or bar Cue, no Autopilot, no macro or
modulator, and a prepared change sent "at the next deck switch" only reacts to a switch. So the builder offers "Show a
deck" only after WHEN is a time of day. A Section or bar Cue that names a deck (from an older or hand-edited show)
loads switched off, with a warning in the load result and the engine log; it cannot be switched on until its DO is
changed, for example to "fire". See ADR 0013.

## Examples

1. **Play another deck's column at the drop.** When the Drop starts → fire → Intro of Peak. Every layer plays the Peak
   deck's Intro column from the first beat of the Drop. The deck in view stays as it is: only a time of day (example
   3) or a hand shows another deck.
2. **Fire a column every 16 bars.** When the Drop starts, every 16 bars → fire → column 2 of this deck. In the builder:
   WHEN the Drop, as it starts, every 16 bars; DO fire; ON WHAT This deck, Column 2.
3. **The 03:00 lineup switch.** At 03:00 → show → Headliner; fire → column 1 of this deck. In the builder: WHEN a time
   of day, 03:00; DO show a deck; ON WHAT Headliner, and fire column 1 of this deck. A Gig's lineup writes these Cues
   for each DJ; they show under the Gig with "at 03:00" as their next firing.
