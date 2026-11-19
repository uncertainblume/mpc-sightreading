
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
- The LinnStrument guide lights are not worked out yet. With Guide = Next and Next+1 the lights
  change, but several odd behaviours remain, to be debugged in the next version.
- A LinnStrument on the MPC's USB can get into a state where it ignores what the MPC sends. On
  the author's MPC, unplugging it and plugging it back in fixed it, and TEST LIGHTS then worked.
- Not yet tried on the MPC at release: the new touch areas, the piece list's show/hide, and the
  DIN route.


### Install

Needs MPC OS 3.10.1 or later with Preferences > Enhancements (Beta) > Enable Enhancements and External Plugin Support on.
Unzip `sight-reader-0.5.0-mpc.zip` and copy the folder `Practice Tools - VST - Sight Reader` into `VST2 Plugins` at the top of the MPC's internal drive, then restart the MPC app.
Projects that already use Sight Reader keep their settings. See the [README](https://github.com/uncertainblume/mpc-sightreading/blob/v0.5.0/README.md).
