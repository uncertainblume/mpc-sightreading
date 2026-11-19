# Sight Reader 0.4.0 (Practice Tools)

A sight-reading trainer for the MPC: a grand staff on the plugin page, MIDI files and generated
exercises, Wait / Flow / Rhythm practice with adjustable slack, progress tracking, and
LinnStrument guide lights and fingering. Full documentation:
https://github.com/uncertainblume/mpc-sightreading

First tried on an MPC Live III with 0.1.0 (pages, staff and cursor drew correctly). Save your
work before loading it; plugins run without crash protection.

Install: MPC OS 3.10.1 or later; Preferences -> Enhancements (Beta) -> Enable Enhancements and
External Plugin Support. Copy the folder "Practice Tools - VST - Sight Reader" into "VST2 Plugins"
on the MPC's internal drive, restart, and load it on a plugin track whose MIDI input is your
controller (monitor Merge). Keep your instrument on another track listening to the same input.

Pages: PLAY (press its key again for LIGHT, the same page in a day theme), LOAD, GEN, SETUP,
MORE, SOUND, COLOR, LINN, STATS. Your library: SightReader/Library/<folder>/*.mid on the same
drive. LISTEN plays the piece to any instrument track whose MIDI input is "Sight Reader MIDI Out"
(monitor In).

MIT licence. Glyphs rendered from Bravura and Titillium Web (SIL Open Font License 1.1).
