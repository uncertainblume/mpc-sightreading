# Changelog

Each version's zip is attached to its [GitHub release](https://github.com/uncertainblume/mpc-sightreading/releases)
(tags `v0.1.0` ... `v0.5.0`). Install or update by copying the folder in the zip to `VST2 Plugins`
on the MPC's internal drive; projects keep their settings, and new settings start at their
defaults.

## [0.5.0] - 2026-11-19

From the notes taken while practising with 0.4.0 on an MPC Live III.

### Added
- **Header fields.** Tap **Bar**, **Note** or **BPM** in PLAY's header and turn the data wheel,
  or use Q-Links 5-7 (the MPC frames them as a group). Bar and Note move the cursor a bar or a
  note at a time; BPM sets the tempo directly. Double-tap a field, or push the wheel, for the big
  knob. The fields follow the cursor as you play.
- **The MPC's transport**, with Start = MPC Play. Play and Stop start and pause practice. While
  stopped, anything that moves the MPC's playhead moves the practice position to the same bar and
  beat: locate, step, event and increment back/forward, and Stop twice (back to the start).
- **Loop To**, next to Loop From on PLAY. Loop From, Loop To and Loop Bars follow each other and
  stay inside the piece.
- **Pieces in order.** Loading a file makes its folder the piece list. Tap the title on PLAY to
  open it, turn the data wheel to choose, then push the wheel or tap a row to load. The title
  shows *(k/N)*.
- **Auto Next** (MORE, on by default). Two seconds after a piece ends, the next one loads.
- **Playlist.** ADD TO LIST and REMOVE on LOAD build a list of any files in your order. It shows
  as the last folder, *Playlist*, and is kept in `SightReader/playlist.txt`.
- **8va / 8vb / 15ma / 15mb lines.** Notes beyond the space above or below a staff move by an
  octave or two, for the whole bar where possible.
- **LinnStrument: Linn Port.** Send the lights over USB, or through the MPC's MIDI Out A/B to the
  LinnStrument's MIDI In.
- **LinnStrument: TEST LIGHTS** paints a green diagonal and lights C4 for two seconds.
- **LinnStrument: Restore.** Cells the plugin has finished with go back to your note colours
  instead of off.
- **LAYOUT**, a second LINN page (press LINN again) for the instrument's size, row offset, lowest
  note and handedness.

### Changed
- **LOAD.** The data wheel moves the selection through the whole folder, and pushing it loads the
  file. Tapping a row selects it, and a double tap loads it.
- **Note colours.** F# is a light salmon pink, so it stands apart from E's pink and F's magenta.
- **Left-hand drills.** The scale and arpeggio drills that went below C2 start an octave higher.
- **LinnStrument: painted cells.** The plugin paints only the cells it needs. Paints share the
  firmware layer of custom light patterns, so cells it never used are never written.
- **LinnStrument: pacing.** Messages are paced in time for each route: about 3,000 a second by
  USB and 900 through DIN.
- **LinnStrument: Linn Ch** defaults to 2. An MPE LinnStrument ignores highlights on channel 1.
- **Header.** It is laid out in groups, with thin rules between them.
- **Values.** Loop values read "Bar 4", tempo "80 BPM".

### Fixed
- Notes far below or above the staff were drawn pinned to the last row (A1 sat on C#2's line).
- The data wheel's first click on Folder, Scroll or Loop From jumped by a hundredth of the range
  instead of one step.
- A project load could be taken for data-wheel clicks and restore some values off by one. The
  list's selection could also be rewritten while a project loaded.

### Known issues
- On the author's MPC, a LinnStrument on USB does not respond to anything the MPC sends. Try
  TEST LIGHTS, the computer test in `docs/linnstrument.md`, then the MIDI-jacks route.
- Not yet tried on the MPC: the new touch areas, the piece list's show/hide, and the DIN route.

## [0.4.0] - 2026-11-19

### Added
- **Readouts.** Every control has a line underneath saying what its current choice does. It
  changes as you change the control. PLAY shows one-line versions next to the control names.
- **Units on values**: 160 ms, 30%, +2 st, Ch 1, Level 2, 3 times.

### Changed
- **Captions.** Switches and buttons have their captions drawn in, with the chosen option filled.
- **Layout.** LINN and GEN are re-laid out with 150 px rows. Help paragraphs the readouts replace
  are gone.
- **Note colours.** D# is Navy, a deeper blue than D. The violet between blue and pink read as
  F's magenta. Dark dot colours get a light edge.

## [0.3.0] - 2026-11-19

### Added
- **LISTEN** plays the piece, or the loop, in time through the plugin's port *Sight Reader MIDI
  Out*. Any instrument track with that input plays it.
- **LISTEN speed and hands.** The speed follows the Tempo knob, or a fixed 25-150 % (Listen At).
  Listen Hands plays both hands or only the practised ones.
- **Out Ch** sets the channel for LISTEN and Accomp.
- **While LISTEN plays**, the cursor follows the music and the LinnStrument guide lights the
  sounding notes. Input is not graded and the practice position stays.

## [0.2.0] - 2026-11-19

The answer to the first test on an MPC Live III.

### Added
- **Played-note colours.** Played notes turn green and missed notes red. A wrong note shows as a
  red note where you played it, with its accidental and ledger lines.
- **Note colour dots** (COLOR page): a dot of each note's colour in every note still to play,
  editable per note. The default is the LinnStrument scheme.
- **Error sound** (SOUND page): six sounds, volume, pitch, an ADSR envelope with length, a
  reminder level, metronome click type and level.
- **Next note per hand.** The header shows the next note of each hand.
- **Stuck messages.** Two wrong notes that look like the same slip get a message: the other
  hand's note is still wanted, or the instrument is an octave off.
- **Drills.** Scale and arpeggio drills for each hand separately, and hands together in their own
  folder.

### Changed
- **Error sound level.** The default sound is a soft knock about 8 dB below 0.1.0's square-wave
  blip.
- **Day theme.** Only PLAY has a Day version (LIGHT); the settings pages are dark only. The MPC
  showed every theme copy as its own tab.

### Fixed
- After the first note of a two-hand drill, the plugin waited for the left hand and counted every
  further right-hand note as wrong, without saying so.
- A suspend and resume of the host sent the player back to the first note.
- Re-striking a note of the current chord, or of the step just completed, counted as a mistake.
- At the end of a piece in Halves view, the left half went blank.

## [0.1.0] - 2026-11-19

First version.

### Added
- **The plugin.** A grand staff drawn from film-strip pictures on the MPC's screen, with key and
  time signatures, accidentals, beams, ties, rests and triplets.
- **Practice.** Wait, Flow and Rhythm modes with timing, pitch and chord slack. Hands, range,
  loops with a tempo ramp, masks, four scrolling views and a metronome.
- **Material.** A MIDI file library with progress tracking, 148 built-in drills and an exercise
  generator.
- **LinnStrument.** Guide lights, fingering suggestions, LIGHT BARS and Key Lights from the MPC
  project.
- **Library tool.** Fetches public-domain pieces and copies them to the MPC over SSH.

[0.5.0]: https://github.com/uncertainblume/mpc-sightreading/releases/tag/v0.5.0
[0.4.0]: https://github.com/uncertainblume/mpc-sightreading/releases/tag/v0.4.0
[0.3.0]: https://github.com/uncertainblume/mpc-sightreading/releases/tag/v0.3.0
[0.2.0]: https://github.com/uncertainblume/mpc-sightreading/releases/tag/v0.2.0
[0.1.0]: https://github.com/uncertainblume/mpc-sightreading/releases/tag/v0.1.0
