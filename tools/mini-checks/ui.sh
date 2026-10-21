#!/bin/bash
# Laptop UI checks by eye (68 section 3 and the UI changes since the gig build 67391c8): MANUAL rows only.
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/lib.sh"
parse_args "$@"
say "== laptop UI by eye"

manual "2b3cf8e show name" "after any engine restart (audio.sh or ab-flags.sh ran one)" \
    "the UI title still names the gig show; the unsaved dot is as before; Save writes that file"
manual "73 text cells live" "layer 5 text cells in two decks, layer Autopilot on, switch decks" \
    "the playing text cell is marked live; the Preview Monitor shows the layer; the other deck's cell takes over"
manual "b0539f3 preview noise" "every monitor showing for a minute" "no torn or noisy preview picture"
manual "101 option dropdowns" "an effect with an option parameter (Classic Stripper, Classic Auto Mask)" \
    "options are dropdowns in order; a cut-off label shows the full name in its tooltip"
manual "75 colour pickers and palette" "a colour group's swatch; the Palette tab; link a colour, recolour the slot" \
    "the picker opens with the palette under it; a palette change recolours every linked colour; one Cmd-Z"
manual "97 Cues tab" "build a deck switch at the next drop and a column every 16 bars; switch one off" \
    "each cue reads as one sentence; both fire on the bar; off skips it; repeat fires every N bars"
manual "76 effect steps" "an effect's step strip: add two steps, Autopilot next every 2 bars" \
    "the badge shows Autopilot; steps change on the bar; tooltips say what each chip does"
manual "1b1f74b folded bypass" "fold an effect strip with the chevron" "its bypass button stays clickable when folded"
manual "20cfc42 chevrons" "effect headers and dropdowns" "chevrons and arrows in a 22 px box, header rows 22 px, nothing overlaps"
manual "ebd65e0 scrolling lists" "wheel and trackpad over Files, Sources, Cues, History, Autopilot, the Map tree, Effects" \
    "every list scrolls; the smallest trackpad step moves it"
manual "83 Map" "drag the projector's screen frame, hide the TV's screen, save, restart the engine" \
    "the TV keeps playing (presents unchanged); positions, hidden flag and order come back"
manual "99 Macro Sets" "load a Macro Set on the gig show; Cmd-Z" \
    "macros take names, values and modulators, the show's targets stay; Cmd-Z restores them"
manual "78 venue picker" "top bar venue menu (with the Tailscale API key set)" \
    "the Mini shows by its Tailscale name with a status dot; picking it connects every tab"
manual "44/48/64 grid and cards" "Clip tab with no clip selected; drag an effect card to another chain; drop an effect on a block of clips" \
    "no overlap; the card moves with its links (Option copies); one Cmd-Z undoes each"
