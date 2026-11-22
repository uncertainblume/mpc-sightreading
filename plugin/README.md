# Sight Reader 0.7.0 (Practice Tools)

A sight-reading trainer for the MPC: a grand staff on the plugin page, MIDI files and generated
exercises, Wait / Flow / Rhythm practice with adjustable slack, progress tracking, and
LinnStrument guide lights and fingering. Full documentation:
https://github.com/uncertainblume/mpc-sightreading

Tried on an MPC Live III since 0.1.0. Save your work before loading it; plugins run without
crash protection. What changed in each version: CHANGELOG.md on the page above.

Install: MPC OS 3.10.1 or later; Preferences -> Enhancements (Beta) -> Enable Enhancements and
External Plugin Support. Copy the folder "Practice Tools - VST - Sight Reader" into "VST2 Plugins"
on the MPC's internal drive, restart, and load it on a plugin track whose MIDI input is your
controller (monitor Merge). Keep your instrument on another track listening to the same input.

Pages: PLAY, LOAD, LINN (press again for LAYOUT), GEN, SETUP, MORE, SOUND, COLOR, STATS. Your library:
SightReader/Library/<folder>/*.mid on the same drive. On LOAD, turn the data wheel and push it to
load; tap the title on PLAY for the folder's piece list. Tap Bar, Note or BPM in PLAY's header to
move with the data wheel. LISTEN plays the piece to any instrument track whose MIDI input is
"Sight Reader MIDI Out" (monitor In).

MIT licence. Glyphs rendered from Bravura and Titillium Web (SIL Open Font License 1.1).
