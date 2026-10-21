# A deck switches only at a time of day or by hand

The deck shown changes in two ways only: a time-of-day Cue (a Gig slot, or a lineup Cue at 03:00) or a hand (the UI, a manual nocturne-ctl command, or the venue desk on a phone in the booth). Nothing else switches it: no Section Cue, no bar Cue, no layer or column Autopilot, no macro or modulator. A prepared change sent "at the next deck switch" reacts to a switch and never causes one: an edit in it that would change the deck shown is left out. A Node only follows its main's switch.

The document enforces it: a Cue names a deck only with a time of day, and a musical Cue that names one is refused, also when it is switched on. A saved show that still holds one (older or hand-edited) loads it switched off with a warning in the load result and the log; the Cue is never dropped silently.

We chose this over a deck switch on a Section ("show Peak when the Drop starts"), first deferred and then briefly decided as permanent on 2026-10-19. The deck decides which DJ's visuals play, so it follows the lineup and the performer, not the music's structure: a mis-marked or early Section, or an Autopilot step, must never take the show onto another DJ's deck. Firing a column of another deck at the drop still works; the deck in view stays.

The cost: switching decks on the music needs a hand on the button, and a Section Cue that should change the look picks a column instead.
