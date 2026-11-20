# Sight Reader for the MPC

A sight-reading trainer that runs entirely on an Akai MPC (MPC OS 3.10.1 or later), as a plugin
built with Akai's official [MPC plugin SDK](https://github.com/inmusic-software/mpc-plugin-builder-sdk).
It shows a grand staff in standard notation on the MPC's screen, listens to what you play on the
track's MIDI input (any keyboard or MIDI controller, the MPC's own keys or pads, a LinnStrument)
and checks it against a MIDI file or a freshly generated exercise: notes turn green when you play
them, a wrong note shows in red where you played it, and the music moves on as you read. It can
play the piece to you first (LISTEN), keeps your progress per piece, and with a LinnStrument it
can light the next notes, whole passages, suggested fingerings and the project's key on the
instrument itself.

*Dedicated to Roger Linn and Geert Bevin, creators of MPE and the LinnStrument, and to Wendy
Carlos, the pioneer without whom none of this would be possible.*

![The PLAY page in the Night theme: a Bach minuet, the first bar played](docs/img/play-night.png)

**[Download the latest release](https://github.com/uncertainblume/mpc-sightreading/releases/latest)**
(a zip to copy to the MPC; what changed in each version is in [CHANGELOG.md](CHANGELOG.md)).



If anyone finds any of this this super useful and can afford it, send me a tip on PayPal, I'm hilariously unemployed: <img width="500" height="500" alt="paypalqrcode" src="https://github.com/user-attachments/assets/f481d192-200a-4d61-a5a7-09f00cb26175" />

**Status: version 0.6.0.** Sight Reader has been in use on an MPC Live III since 0.1: the pages,
the notation, the cursor, the practice modes, LISTEN, the piece list and the LinnStrument's lights
work on the device. 0.6.0 adds:

- **The MIDI loader** ([loader/](loader/README.md)): a page in your browser for putting your own
  MIDI files on the MPC. Drop `.mid` files into collections, press **Sync**, and the MPC's library
  matches your computer's. It also restarts the MPC app. Everything runs on your computer; the
  MPC only sees an SSH login.
- **The library rescans by itself** after a sync (Sight Reader looks at one marker file every 2
  seconds), keeping your place in the LOAD list.
- **The step-after light goes off when you touch it**, as a next-note light does.

0.5.6 made the LinnStrument's guide lights keep up. 0.5.5 brought:
- LISTEN pause, seek and loop marking;
- the other hand played in time to MIDI Out;
- Fit Range for the LinnStrument's surface, and following its Octave and Transpose;
- even jog-wheel steps.

0.6.0 passes the SDK's validator (0 errors, 0 warnings), its host simulator, an end-to-end test
that plays pieces through the plugin's VST interface and checks the screen, and the loader's
tests. Those run every loader operation against a pretend MPC (under `sh` and `busybox`) and the
page in a browser. The 0.5.5 to 0.6.0 changes have not yet been tried on the MPC.

If the LinnStrument's lights do nothing, unplug the LinnStrument and plug it back in, then press
TEST LIGHTS (LINN page). On the author's MPC Live III the LinnStrument once stopped taking
anything the MPC sent it over USB, and a replug brought it back. The MPC's MIDI Out jacks are a
second route. See [docs/linnstrument.md](docs/linnstrument.md).

## What it does

| | |
|---|---|
| **Notation** | Grand staff (treble and bass) with key and time signatures, ledger lines, accidentals spelled by key and by melodic direction, dotted notes, ties, flat beams, flags, rests, triplet marks, pickup bars detected automatically, 8va / 8vb lines for notes far off the staff. Engraved with the Bravura music font. |
| **Scrolling** | Four views: *Halves* (default: one half of the screen is rewritten with what comes next while you read the other), *Page* (turns when you reach the end), *Bars* (the current bar is always first), *Steps* (the music moves under a fixed cursor). |
| **Modes** | *Wait*: holds on every note until you play it. *Rhythm*: a clock runs at the tempo you set and grades timing. *Flow*: the clock runs but waits up to *Hold* beats for a missed note before moving on. |
| **Moving around** | Tap **Bar**, **Note** or **BPM** in the header and turn the data wheel (or Q-Links 5-7): jump bar by bar or note by note, set the tempo directly. With Start = MPC Play, the MPC's Play and Stop start and pause practice, and moving the MPC's playhead (locate, step and event buttons, Stop twice for the start) moves you to the same bar. |
| **Pieces in order** | Load one file and its folder becomes the piece list: tap the title on PLAY to pick another with the data wheel, see *(3/12)* after the title, and let **Auto Next** load the next piece when one ends. Build a **Playlist** of any files, in your order, on the LOAD page. |
| **Slack** | Timing window (40-400 ms), pitch tolerance (exact, any octave, a semitone either way), chord tolerance (all notes, all but one, any one), how long Flow waits, whether wrong notes are ignored, counted or stop the clock. |
| **Focus** | Right hand, left hand or both; a note range (outside it notes are not asked for, or removed from the score); loop any span of bars (Loop From, Loop To and Loop Bars follow each other), with an optional tempo ramp after each pass at or above your goal; masks that hide notes once played, or hide the bar you are in so you have to read ahead. |
| **Listen** | LISTEN (on PLAY) plays the piece, or the loop, in time as MIDI through the port "Sight Reader MIDI Out", so any instrument track (a plugin, a keygroup, a drum program) set to that input plays it as an example. Press it again to pause and once more to go on; while it is on, the Bar and Note fields (and the jog wheel) move what you hear, and **SET** next to Loop From / Loop To marks the loop at the bar you hear. Speed: the Tempo knob, or a fixed 25-150 %. The cursor follows the music, and in Linn Mode the LinnStrument lights the notes as they sound. Your practice position stays where it was. |
| **Accompaniment** | Practising one hand, the other hand plays to "Sight Reader MIDI Out" in time (Accomp, MORE, on by default); in Wait mode it waits for you whenever your next note is due. |
| **Feedback** | A band marks the current note; notes you play turn green, missed notes red, and a wrong note appears as a red note where you played it (with its accidental). A red flash can cover the staff on a mistake. The header shows the next note of each hand and the note you played, and says what is wrong when you seem stuck (the other hand's note still wanted, or an instrument an octave off). A quiet error sound (six timbres, volume, pitch and an ADSR envelope) sounds on a mistake and again softer on every Nth note afterwards, so you notice during a fast run. Accuracy, misses, bar and tempo are on screen. |
| **Note colours** | A dot of each note's colour in every notehead still to be played, set per note on the COLOR page. The default is a LinnStrument scheme: C cyan, D blue, E pink, F magenta, G white, A yellow, B orange, with in-between hues for the black keys (D# navy, F# a light salmon pink...). |
| **Readouts** | Under every setting, a line that says what its current choice does ("Count: a wrong note lowers accuracy; the music goes on"); values carry their units (160 ms, 30%, Ch 1). |
| **Material** | 224 built-in drills (scales and arpeggios for each hand, modes, intervals, rhythm levels 1-5, ledger lines, hands-together scales and coordination, LinnStrument shapes), an exercise generator (any key and mode, rhythm and leap difficulty 1-5, left-hand styles), and a library you fill with your own files (the MIDI loader) or from the [Mutopia Project](https://www.mutopiaproject.org). |
| **Progress** | Per piece: plays, best and last accuracy, minutes, best tempo reached at your goal, and accuracy for each sixteenth of the piece; a day streak. Stored in `SightReader/progress.tsv` on the MPC's drive. |
| **Themes** | PLAY comes in Night and Day: press its function key again for LIGHT, the same page on light paper. The settings pages use the MPC's own dark style. |

![The piece list: tap the title, turn the data wheel, push it to load](docs/img/piece-list.png)

## Install

You need MPC OS 3.10.1 or later with **Preferences → Enhancements (Beta) → Enable Enhancements**
and **External Plugin Support** switched on.

1. Download `sight-reader-0.6.0-mpc.zip` from the
   [releases](https://github.com/uncertainblume/mpc-sightreading/releases) (the same file is in
   [dist/](dist/)) and unzip it, or build it yourself (below).
2. Copy the folder `Practice Tools - VST - Sight Reader` into `VST2 Plugins` at the top of the
   MPC's **internal** drive (`/nvme` on a Live III; removable drives are mounted `noexec`), or
   over the network with the SDK's tool (it asks before restarting the MPC app; save your project
   first):

   ```bash
   python3 third_party/mpc-plugin-builder-sdk/tools/mpc_remote.py <mpc-ip> --drive 1 update dist/sight-reader-0.6.0-mpc.zip
   ```
3. Restart the MPC app. Sight Reader is under **External Plugins**. Projects that already use it
   keep their settings.

### Set up a track

1. Make a plugin track with **Sight Reader**. MIDI input: your keyboard, controller or
   LinnStrument, all channels; monitor **Merge** (always listening).
2. Your instrument sound stays on its own track, also listening to the controller with monitor
   Merge, so you hear yourself while Sight Reader listens.
3. On **LOAD**, turn the data wheel to a piece and push it (or tap it and press LOAD); or **GEN**
   → **GENERATE**. *Drills: Scales* is right hand only; *Scales LH* the left; *Two Hands* both at
   once.
4. **PLAY**: in Wait mode just start playing. In Rhythm or Flow press PLAY (or tap the staff) for
   a count-in. With **Hands = Both**, a piece written for two hands waits for both: set Hands to
   Right or Left to practise one.

To hear the piece with **LISTEN**: give an instrument track (any plugin or program) the MIDI
input **Sight Reader MIDI Out**, channel All (or Out Ch, MORE page), monitor **In**; then press
LISTEN on PLAY. To hear it on the instrument you play, give that instrument a second track with
this input (or set its input to *All Ports*). If the port is not in the list: Preferences → MIDI /
Sync → Input Ports, turn **Track** on for it. Do not set Sight Reader's own track input to *All
Ports* (its LISTEN and Accomp notes would come back to it).

## Add your own MIDI files: the MIDI loader

On a Mac, double-click `loader/mpc-sightreading-midi-loader.command` in this repository (or run
the command below, on any computer with Python 3.8+ and `ssh`). A page opens in your browser:

```bash
python3 loader/mpc-sightreading-midi-loader.py
```

- The first time, enter the MPC's IP address. The MPC shows it in Preferences: the Wi-Fi tab
  (hold Shift) or the Ethernet tab. The address is remembered on your computer, not in the
  repository.
- Drop `.mid` files onto the page. They go into collections, the folders of the LOAD list. Click a
  piece to give it a title, composer and level.
- Press **Sync to the MPC**. New and changed pieces are copied; pieces you removed are removed
  from the MPC, but only ones this computer put there.
- Sight Reader 0.6.0 picks the change up by itself within a few seconds.
- **Restart the MPC app** is there too, for when you need it (save your project first).

The MPC needs developer mode on, which starts its SSH server; see
[loader/README.md](loader/README.md) for that and the rest.

## Download music from the Mutopia Project

```bash
python3 tools/library.py fetch
```

That downloads the 70 curated pieces in [library/catalog.tsv](library/catalog.tsv) into
`library/cache/`: Bach's Anna Magdalena notebook, Czerny, Burgmüller, Tchaikovsky, Schumann,
Satie, Chopin, Mozart, Clementi, Joplin... `library/cache/` is also the loader's library when you
run the loader from this repository, so **Sync** on its page puts them on the MPC. More:

```bash
python3 tools/library.py mutopia "Clementi" --max 20
```

Without the loader, `tools/library.py push <mpc-ip>` copies the cache to the MPC in one SSH
command, and `tools/library.py sync <mpc-ip>` fetches and pushes. Popular songs under copyright
are not included: add your own files with the loader.

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
output folder is `plugin/build/Practice Tools - VST - Sight Reader/`. A release is published by
the [release workflow](.github/workflows/release.yml) when its version and zip reach `main`.

## Read more

- [docs/user-guide.md](docs/user-guide.md): every page and setting.
- [loader/README.md](loader/README.md): the MIDI loader.
- [docs/linnstrument.md](docs/linnstrument.md): the LinnStrument features, the routes, the MIDI
  they use, and the USB problem.
- [docs/design.md](docs/design.md): how a plugin with no drawing API shows scrolling notation,
  the evidence behind each choice, and the limits.
- [docs/handoff.md](docs/handoff.md): what is tested, what to check first on the MPC, open
  questions, ideas.
- [CHANGELOG.md](CHANGELOG.md): what changed in each version.

## Layout

```
plugin/                the SDK project: plugin.json, src/, skins/ (generated), Content/Drills/
plugin/src/SightReader.cpp   the plugin: parameters, audio thread, worker thread
plugin/src/sr/         MIDI files, notation layout, practice engine, generator, LinnStrument,
                       ALSA ports, library and progress files (header-only, no SDK needed)
plugin/src/sr/gen/     tables generated by tools/gen_skin.py (must match the skins)
tools/gen_skin.py      writes plugin/skins and the generated headers
tools/skin/            screen geometry, the frame catalogue, glyph rendering, page previews
tools/make_drills.py   the built-in drills
tools/library.py       library download and SSH push
loader/                the MIDI loader: the page, sync over SSH, restart (one Python file)
library/catalog.tsv    the curated list of pieces
tests/                 unit tests, the end-to-end plugin test, the loader's tests
dist/                  the ready-to-install zips of the current version
```

## Credits and licences

MIT licensed (see LICENSE). Music glyphs are rendered from **Bravura** (Steinberg, SIL Open Font
License 1.1) and text from **Titillium Web** (SIL OFL 1.1); only the rendered images ship in the
plugin. MIDI files from the **Mutopia Project** are public domain or Creative Commons as listed on
each piece's page; they are downloaded by you, not bundled. The ALSA port code is adapted from
the author's [mpc-mpe](https://github.com/uncertainblume/mpc-mpe). The LinnStrument note layout
and lighting messages follow Roger Linn Design's open-source firmware (Apache 2.0).

Not affiliated with Akai Professional, inMusic, Roger Linn Design or the Mutopia Project.
