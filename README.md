# Sight Reader for the MPC

A sight-reading trainer that runs entirely on an Akai MPC (OS 3.10.1 or later), as a plugin
built with Akai's official [MPC plugin SDK](https://github.com/inmusic-software/mpc-plugin-builder-sdk).
It shows a grand staff in standard notation on the MPC's screen, listens to what you play on
the track's MIDI input (a LinnStrument, a keyboard, the pads), and checks it against a MIDI file
or a freshly generated exercise. A LinnStrument mode lights the next notes, whole passages,
suggested fingerings and the MPC project's key on the instrument itself.

*Dedicated to Roger Linn and Geert Bevin, creators of MPE and the LinnStrument, and to Wendy
Carlos, the pioneer without whom none of this would be possible.*

![The PLAY page in the Night theme, Für Elise](docs/img/play-night.png)

**Status: version 0.2.0.** 0.1.0 was tried on an MPC Live III: the pages, the staff, the notes and
the cursor drew correctly and the MPC stayed responsive. 0.2.0 answers that first test: played
notes turn green and wrong ones show in red, your LinnStrument colour scheme as a dot in every
note, a gentler error sound you can shape (SOUND page), one-hand scale drills, and fixes for what
made playing seem stuck (see [docs/handoff.md](docs/handoff.md)). It passes the SDK's validator
(0 errors, 0 warnings), its host simulator, and an end-to-end test that plays pieces through the
plugin's VST interface and checks the screen.

## What it does

| | |
|---|---|
| **Notation** | Grand staff (treble and bass) with key and time signatures, ledger lines, accidentals spelled by key and by melodic direction, dotted notes, ties, flat beams, flags, rests, triplet marks, pickup bars detected automatically. Engraved with the Bravura music font. |
| **Scrolling** | Four views: *Halves* (default: one half of the screen is rewritten with what comes next while you read the other), *Page* (turns when you reach the end), *Bars* (the current bar is always first), *Steps* (the music moves under a fixed cursor). |
| **Modes** | *Wait*: holds on every note until you play it. *Rhythm*: a clock runs at the tempo you set and grades timing. *Flow*: the clock runs but waits up to *Hold* beats for a missed note before moving on. |
| **Slack** | Timing window (40-400 ms), pitch tolerance (exact, any octave, a semitone either way), chord tolerance (all notes, all but one, any one), how long Flow waits, whether wrong notes are ignored, counted or stop the clock. |
| **Focus** | Right hand, left hand or both; a note range (outside it notes are not asked for, or removed from the score); loop any span of bars, with an optional tempo ramp after each pass at or above your goal; masks that hide notes once played, or hide the bar you are in so you have to read ahead. |
| **Feedback** | A band marks the current note; notes you play turn green, missed notes red, and a wrong note appears as a red note where you played it (with its accidental). Played notes fade. A red flash can cover the staff on a mistake. The header shows the next note of each hand and the note you played, and says what is wrong when you seem stuck (the other hand's note still wanted, or an instrument an octave off). A quiet error sound (six timbres, volume, pitch and an ADSR envelope) sounds on a mistake and again softer on every Nth note afterwards (default every 10th, three times) so you notice during a fast run. Accuracy, misses, bar and tempo are on screen. |
| **Note colours** | A dot of each note's colour in every notehead still to be played, set per note on the COLOR page. The default is a LinnStrument scheme: C cyan, D blue, E pink, F magenta, G white, A yellow, B orange, and the hue halfway between for each black key. |
| **Material** | 224 built-in drills (scales and arpeggios for each hand, modes, intervals, rhythm levels 1-5, ledger lines, hands-together scales and coordination, LinnStrument shapes), an exercise generator (any key and mode, rhythm and leap difficulty 1-5, left-hand styles), and a library you fill from the [Mutopia Project](https://www.mutopiaproject.org) or with your own files over SSH. |
| **Progress** | Per piece: plays, best and last accuracy, minutes, best tempo reached at your goal, and accuracy for each sixteenth of the piece; a day streak. Stored in `SightReader/progress.tsv` on the MPC's drive. |
| **Themes** | PLAY comes in Night and Day: press its function key again for LIGHT, the same page on light paper. The settings pages use the MPC's own dark style. |
| **LinnStrument** | Guide lights for the next note(s) using the instrument's own "played" highlight or any colour; fingering suggestions (a hand-position planner for the fourths grid, coloured by finger); LIGHT BARS lights every note of the loop or of the bars on screen; Key Lights paints the MPC project's key (the Sounds browser's key) or the pad-perform scale. Your own note-light settings are never changed. |

It makes no sound of its own except the error sound and an optional metronome: keep your
instrument (SurgeXXL, a stock MPC instrument...) on its own track listening to the same input.

## Install

You need MPC OS 3.10.1 or later with **Preferences → Enhancements (Beta) → Enable Enhancements**
and **External Plugin Support** switched on.

1. Take [dist/sight-reader-0.2.0-mpc.zip](dist/sight-reader-0.2.0-mpc.zip) (built from this commit) and unzip it,
   or build it yourself (below).
2. Copy the folder `Practice Tools - VST - Sight Reader` into `VST2 Plugins` at the top of the
   MPC's **internal** drive (`/nvme` on a Live III; removable drives are mounted `noexec`), or
   over the network with the SDK (it asks before restarting the MPC app; save your project first):

   ```bash
   python3 third_party/mpc-plugin-builder-sdk/tools/mpc_remote.py <mpc-ip> --drive 1 update dist/sight-reader-0.2.0-mpc.zip
   ```
3. Restart the MPC app. Sight Reader is under **External Plugins**.

### Set up a track

1. Make a plugin track with **Sight Reader**. MIDI input: your LinnStrument (or keyboard), all
   channels; monitor **Merge** (always listening).
2. Your instrument stays on its own track, also listening to the LinnStrument with monitor
   Merge, so you hear yourself while Sight Reader listens.
3. Press **LOAD**, pick a folder and a piece, press **LOAD**; or **GEN** → **GENERATE**.
   *Drills: Scales* is right hand only; *Scales LH* the left; *Two Hands* both at once.
4. **PLAY**: in Wait mode just start playing. In Rhythm or Flow press PLAY (or tap the staff) for
   a count-in. With **Hands = Both**, a piece written for two hands waits for both: set Hands to
   Right or Left to practise one.

Do not set Sight Reader's track input to *All Ports* if you use **Accomp** (its own notes would
come back as wrong notes; the plugin filters echoes, but don't rely on it).

## Fill the library over SSH

```bash
python3 tools/library.py sync 192.168.1.20
```

That downloads the 70 curated pieces in [library/catalog.tsv](library/catalog.tsv) (Bach's
Anna Magdalena notebook, Czerny, Burgmüller, Tchaikovsky, Schumann, Satie, Chopin, Mozart,
Clementi, Joplin...) into `library/cache/` and copies them to `/nvme/SightReader/Library` on the
MPC in one SSH command. Then press **RESCAN** on the LOAD page. More:

```bash
python3 tools/library.py mutopia "Clementi" --max 20
```

```bash
python3 tools/library.py add ~/Music/*.mid --collection "My Songs"
```

```bash
python3 tools/library.py device-script > get.sh && ssh root@192.168.1.20 sh -s < get.sh
```

The last one downloads straight onto the MPC, if its firmware has `curl` or `wget`.
Popular songs under copyright are not included: add your own files with `add`.

## Build

Needs Python 3.9+, git and a C++ compiler; Pillow (`pip install pillow`) only to regenerate the
skins.

```bash
tools/build.sh
```

```bash
tools/run_tests.sh
```

The first build fetches Akai's SDK (pinned commit) and, through it, the Zig cross compiler. The
output folder is `plugin/build/Practice Tools - VST - Sight Reader/`.

## Read more

- [docs/user-guide.md](docs/user-guide.md): every page and setting.
- [docs/linnstrument.md](docs/linnstrument.md): the LinnStrument features and the MIDI they use.
- [docs/design.md](docs/design.md): how a plugin with no drawing API shows scrolling notation,
  the evidence behind each choice, and the limits.
- [docs/handoff.md](docs/handoff.md): what is tested, what to check first on the MPC, open
  questions, ideas.

## Layout

```
plugin/                the SDK project: plugin.json, src/, skins/ (generated), Content/Drills/
plugin/src/SightReader.cpp   the plugin: parameters, audio thread, worker thread
plugin/src/sr/         MIDI files, notation layout, practice engine, generator, LinnStrument,
                       ALSA ports, library and progress files (header-only, no SDK needed)
plugin/src/sr/gen/     tables generated by tools/gen_skin.py (must match the skins)
tools/gen_skin.py      writes plugin/skins and the generated headers
tools/skin/            screen geometry, the frame catalogue, glyph rendering, preview
tools/make_drills.py   the built-in drills
tools/library.py       library download and SSH push
library/catalog.tsv    the curated list of pieces
tests/                 unit tests, the end-to-end plugin test
```

## Credits and licences

MIT licensed (see LICENSE). Music glyphs are rendered from **Bravura** (Steinberg, SIL Open Font
License 1.1) and text from **Titillium Web** (SIL OFL 1.1); only the rendered images ship in the
plugin. MIDI files from the **Mutopia Project** are public domain or Creative Commons as listed on
each piece's page; they are downloaded by you, not bundled. The ALSA port code is adapted from
the author's [mpc-mpe](https://github.com/uncertainblume/mpc-mpe). The LinnStrument note layout
and lighting messages follow Roger Linn Design's open-source firmware (Apache 2.0).

Not affiliated with Akai Professional, inMusic, Roger Linn Design or the Mutopia Project.
