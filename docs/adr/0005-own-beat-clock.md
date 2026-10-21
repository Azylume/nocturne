# Own beat clock sync instead of Ableton Link

Machines share tempo through our own protocol. The main engine broadcasts the beat timeline (BPM, and the beat position at a reference instant on the shared clock). Every node estimates its clock offset with NTP-style UDP ping-pongs and derives the beat phase locally. External gear connects through MIDI clock in/out.

Ableton Link was rejected because it is GPL or licensed from Ableton. The software may be sold later, and paying Ableton is not wanted.
