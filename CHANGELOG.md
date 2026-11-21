# Changelog

Each version's zip is attached to its [GitHub release](https://github.com/uncertainblume/mpc-sightreading/releases)
(tags `v0.1.0` ... `v0.6.5`). Install or update by copying the folder in the zip to `VST2 Plugins`
on the MPC's internal drive; projects keep their settings, and new settings start at their
defaults.

## [0.6.5] - 2026-11-21

The jog wheel steps one value at a time, the LISTEN transport is on the PLAY page, LINN comes
third, PLAY is dark only, and the MIDI loader takes links.

### Fixed
- **The jog wheel jumped around** on many settings. Since 0.5.5 the plugin had guessed which
  moves were wheel clicks (a hundredth of the range) and turned those into one step. Any other
  move passed through unchanged, so a faster turn, a value the MPC had not caught up with, or a
  preset could jump.
  - Now the skin lists every value of every number (Akai's documented `replacementStrings`), so
    the MPC itself moves one value per click, on the wheel, the Q-Links and the big knob.
  - The long time settings (Timing and the error sound's Decay, Release and Length) step by 5 ms.
  - The plugin steps in only when the MPC moved from a value it had not caught up with, such as
    Bar after the cursor moved it: the same step is then made from the value shown.
  - Presets and projects are taken as they come.

### Added
- **The transport on PLAY**, next to the loop: back to the start, a bar back, LISTEN (play /
  pause), a bar forward, then PLAY and LOOP.
  - The arrows move what LISTEN plays.
  - Without LISTEN, they move the practice cursor.
  - SET next to Loop From / Loop To still marks the loop at the bar you hear.
- **MIDI loader: Add from a link.** Paste a link to a `.mid` file, a `.zip` of them (as on the
  Mutopia Project), or a page that links to them, and pick from that page's links.
  - GitHub, Google Drive and Dropbox share links work.
  - Your computer downloads it; the MPC never touches the web.
  - The link is kept as the piece's source.
  - A link dragged from another browser tab onto the page works too.
  - Sites that refuse scripts are named, with what to do instead: save the file in the browser and
    drop it on the page.

### Changed
- **LINN is the third page**, after PLAY and LOAD (LAYOUT is still on LINN's key).
- **PLAY is dark only.** The LIGHT sub-tab is gone: one tab less, and nothing else loaded for it.
  - A Light switch in SETUP would have put both themes' pictures in the PLAY page, so it would
    take more memory.
  - To bring LIGHT back in a later version: set `WITH_LIGHT = True` in `tools/gen_skin.py` and
    rebuild. The Day theme is still drawn by `tools/skin/render.py`, and LIGHT returns as its own
    sub-tab on PLAY's key, as in 0.6.0 (see docs/design.md).
- The skin's TUI.json is 977 KB (it was 1.4 MB), with every number's steps included.
- The LOAD page's hint points to the MIDI loader.

## [0.6.0] - 2026-11-20

Your own MIDI files on the MPC from a page in your browser, and the step-after light goes off
when you touch it.

### Added
- **mpc-sightreading-midi-loader** (`loader/`, and `mpc-sightreading-midi-loader-0.6.0.zip` on
  the release). A page in your browser, served by a small Python program on your computer:
  - Drop `.mid` files into collections, and give them titles, composers and levels.
  - **Sync** makes the MPC's library match the computer's: new and changed pieces are copied, and
    pieces you removed are removed there.
  - **Restart the MPC app** (`systemctl restart acvs`, as `mpc_remote.py restart` does).

  All the work happens on the computer. The MPC runs nothing new: a sync is one SSH login and one
  archive. The page can only be opened from the computer it runs on (127.0.0.1, with a key for
  each run). Details:
  - The MPC's address has no default. You enter it once, and it is remembered in your user
    settings folder, never in the repository.
  - A piece is removed from the MPC only if this computer put it there. Pieces that got there
    another way are listed as only on the MPC: copy them to the computer, or delete them.
  - Files copied earlier with `tools/library.py push` are recognised, so they are not copied
    again.
  - On a Mac, double-click `mpc-sightreading-midi-loader.command`. Needs Python 3.8+ and `ssh`;
    nothing to install. See [loader/README.md](loader/README.md).
- **The library rescans by itself.** After a sync, the loader (and `tools/library.py push`) touch
  `SightReader/Library/.rescan`. Sight Reader looks at that one file every 2 seconds and rescans
  when it changes. The LOAD list keeps the folder and file you had selected, and the message line
  says "Library updated from your computer". It runs no server and does no network work on the
  MPC. RESCAN keeps your place now too.

### Changed
- **The step-after light goes off when you touch it** (Guide = Next+1), as a next-note light
  does. In Wait mode a note you play early does not count yet, so when its step becomes the next
  one it lights again, in the next-note colour.
- `tools/library.py push` copies files as the MPC's root user, and tells Sight Reader to rescan.
- The repository's documents use `<mpc-ip>` instead of an example address.

## [0.5.6] - 2026-11-20

The LinnStrument guide lights keep up.

### Fixed
- **Next+1 lights lagged or stuck.** With the next notes in one colour and the step after in
  another, playing a next note sometimes left the step-after notes in their colour instead of
  turning to the next-note colour, so the surface showed only step-after notes.
  - The cause, from the firmware: the LinnStrument reads incoming MIDI one byte per pass of its
    main loop, into a small buffer. While it is busy (reloading a light pattern, redrawing after
    a settings question, many touches), a quick burst can overflow it. The plugin assumed every
    colour had arrived, so a lost one stayed wrong.
  - Every paint now carries its own row and column, so one lost message cannot send the
    following colours to the wrong cell.
  - The plugin sends at a gentler rate (1,500 messages a second by USB) in small bursts, and
    pauses for a moment after a request that makes the instrument redraw everything.
  - Every 0.4 s it sends again what should be lit, and what it let go of lately. The Note On
    highlights and their Note Offs are re-sent every 1.2 s. A lost message is put right within a
    fraction of a second.
- **The next note lost to the note after.** On a cell both steps want, the next note's colour
  now wins over the step after's.

### Changed
- With Restore = Pattern n, the light pattern is reloaded at most every 150 ms (it was 60 ms),
  since each reload makes the instrument redraw everything.
- The instrument's Octave and Transpose are asked for every 8 s (it was 3 s).

## [0.5.5] - 2026-11-20

From the notes taken while playing 0.5.1: mostly fixes, and the transport, accompaniment and
LinnStrument behaviour asked for.

### Added
- **LISTEN transport**
  - LISTEN is play/pause: pressing it while it plays pauses where it is (status PAUSED), and
    pressing it again goes on from there.
  - While LISTEN is on, playing or paused, the Bar and Note fields, the jog wheel and the MPC's
    playhead move what you hear.
  - Switching Loop on or off, or moving the loop, while it plays changes what it plays.
  - **SET** buttons next to Loop From and Loop To mark the loop at the bar you hear, or the bar
    the cursor is in.
- **Fit Range** (LINN, with Linn Mode on): notes the LinnStrument's surface cannot play are
  folded by octaves onto it (**Fold**, the default), or left out (**Skip**). The surface's range
  comes from LAYOUT and the instrument's transposition. For example, the very low notes that
  open *In the Hall of the Mountain King* move up to the surface.
- **After Clr** (LINN): the colour of the step after the next one in Next+1. Played uses the
  instrument's own highlight.
- **Linn Octave, Linn Transp and Read Transp** (LAYOUT): the LinnStrument's Octave and Transpose
  settings. Colours, fingering and Fit Range follow them, so they land on the right cells when
  you transpose the instrument. With Read Transp on (the default) they are read from the
  instrument every few seconds over USB (NRPN 299 requests, answered on a new input port).
- A setting that rebuilds the piece (Linn Mode, Fit Range, the layout, Transpose...) keeps the
  bar you were in.

### Changed
- **Accompaniment**
  - With one hand practised, the other hand now plays to Sight Reader MIDI Out in time. In Wait
    mode it waits for you whenever your next note is due, and its notes that fall with yours
    sound when you play yours.
  - Accomp is **on** (Other Hand) by default. Projects keep their saved setting, so turn it on in
    MORE if yours says Off.
- **Guide lights**
  - The guide always shows the next notes still to press. A note you have played goes dark, and
    the next one lights, also when you play ahead of the clock in Rhythm or Flow.
  - A highlighted note you release is lit again if it is still the one to press: the instrument
    clears the cell under your finger.
- **LINN and LAYOUT pages**: Fit Range and After Clr are on LINN; Linn Ch moved to LAYOUT with
  the transposition settings.

### Fixed
- **The jog wheel skipped values** on number settings. The MPC moves a number by a hundredth of
  its range per click (Tempo 100, 102, 104, 105...; a 1-16 channel needing several clicks).
  Every number setting now moves one step per click; long millisecond ranges move in round steps
  (Timing 5 ms, envelope times 20 ms). This works whether the MPC adds to the value it reads back
  or to the one it last sent, and at either end of a range.
- **The accompaniment was missing notes** in Wait mode: the other hand's notes that fall with
  yours were never sent, and the rest went out all at once when skipped.

### Known issues
- Not yet tried on the MPC: the transposition read-back (needs the LinnStrument on USB), SET, and
  the paused LISTEN.

## [0.5.1] - 2026-11-20

Your LinnStrument's own colour layout no longer drifts while you play.

### Fixed
- **Guide lights left cells recoloured.** Cells the guide had lit came back in the plugin's
  approximation of your note colours (white, orange and so on), not your own layout, so over a
  longer piece the layout drifted. The cause, from the firmware: the plugin's cell paints go into
  the same light layer as the instrument's custom light pattern and overwrite it there.
  - Guide Clr = **Played** now never paints: the step after (Next+1) is lit with the
    instrument's own Played highlight as well. The highlight lives in a separate layer, so
    nothing of your layout is touched.
  - **Restore** has new choices, **Pattern 1 / 2 / 3**. When the plugin is done with painted
    cells (a colour guide, fingering, LIGHT BARS, TEST LIGHTS), the instrument is asked to
    reload that stored custom pattern (NRPN 247), which restores it exactly. Whatever is still
    lit is then painted again.
  - Restore **Off** painted cells black (colour 7). It is now **Instrument**: the cell is handed
    back (colour 0), so the instrument's own note lights show again. This is the default for
    new instances.

### Changed
- Restore is a menu on the LINN page (five choices). Projects keep their saved choice, so pick
  **Pattern n** there if your layout is a custom light pattern.

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
- The LinnStrument guide lights are not worked out yet. With Guide = Next and Next+1 the lights
  change, but several odd behaviours remain, to be debugged in the next version.
- A LinnStrument on the MPC's USB can get into a state where it ignores what the MPC sends. On
  the author's MPC, unplugging it and plugging it back in fixed it, and TEST LIGHTS then worked.
- Not yet tried on the MPC at release: the new touch areas, the piece list's show/hide, and the
  DIN route.

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

[0.6.5]: https://github.com/uncertainblume/mpc-sightreading/releases/tag/v0.6.5
[0.6.0]: https://github.com/uncertainblume/mpc-sightreading/releases/tag/v0.6.0
[0.5.6]: https://github.com/uncertainblume/mpc-sightreading/releases/tag/v0.5.6
[0.5.5]: https://github.com/uncertainblume/mpc-sightreading/releases/tag/v0.5.5
[0.5.1]: https://github.com/uncertainblume/mpc-sightreading/releases/tag/v0.5.1
[0.5.0]: https://github.com/uncertainblume/mpc-sightreading/releases/tag/v0.5.0
[0.4.0]: https://github.com/uncertainblume/mpc-sightreading/releases/tag/v0.4.0
[0.3.0]: https://github.com/uncertainblume/mpc-sightreading/releases/tag/v0.3.0
[0.2.0]: https://github.com/uncertainblume/mpc-sightreading/releases/tag/v0.2.0
[0.1.0]: https://github.com/uncertainblume/mpc-sightreading/releases/tag/v0.1.0
