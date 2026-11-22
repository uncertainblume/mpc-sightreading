# How Sight Reader works, and why

## The constraint: the MPC does not let plugins draw

MPC plugins (OS 3.10.1, Akai's SDK) are VST2 shared libraries with **no editor window**: the MPC
draws each plugin page from JSON "skins" (`Plugin Skins/TUI.json`) that bind widgets to parameter
indices (SDK `docs/host-contract.md`, "UI"; `docs/ui-reference.md`). There is no canvas, no custom
font, no way to send an image at run time. Widgets that a plugin can drive by itself:

| Widget | What the plugin controls | Evidence |
|---|---|---|
| `Image` film strip | which pre-rendered frame shows (parameter value) | SDK ui-reference 6.6; wavetable pictures in SurgeXXL; community meters |
| `Label` (Value) | the parameter's display string | SDK 6.3; but the MPC re-reads text only on `audioMasterUpdateDisplay`, which rebuilds the whole page (community wrapper `vst2_wrap.c`) |
| `Button` | lit / unlit | SDK 6.4 |
| `Slider` ColourBar | a filled bar | SDK 6.1 |

Value changes reach the screen when the plugin calls `audioMasterAutomate` from the audio thread
(community wrapper; "show live numbers as pictures" because text needs a page rebuild).

## So the staff is a grid of pictures

- **Cells.** 20 note columns ("slots") × 33 staff rows (treble positions 12…-7 then bass 8…-4;
  treble -1…-7 share rows with bass 15…9, which is exactly a grand staff with a four-space gap).
  Each cell is a 54 × 60 px film strip whose frames are every glyph combination that can occupy
  one staff position: notehead (black, half, whole; normal or second-displaced) × accidental ×
  dot × the stem piece passing through; stem ends with flags or beam pieces (left/right levels);
  tie pieces; rests; ledger lines. About 190 frames per strip, in three strips (space rows, staff
  line rows, ledger rows). Stems are drawn piecewise through the cells they cross, beams as
  half-slot pieces that tile across slots.
- **Catalogue as the single source of truth.** `tools/skin/geometry.py` defines the geometry and
  enumerates the frames; it writes both the PNGs (via `render.py`, glyphs from Bravura) and
  `plugin/src/sr/gen/layout_gen.h`, which the C++ layout engine uses to turn "head at position p,
  sharp, stem up" into a frame number. They cannot disagree.
- **Overlays** (one film strip per slot over both staves: current, played/faded, missed, hidden),
  staff masks for the hand not practised, barlines between slots, key and time signatures, a
  red/green flash over the staff, digits, note names and status words are more film strips.
  Titles and the file list are text parameters (they change rarely).
- **Marks** (0.2.0) are a second grid of 660 film strips over the cells, above the staff lines
  and the played-note fade: a colour dot in a notehead, the head redrawn green (played) or red
  (missed), a red "ghost" head with its accidental and ledger lines where a wrong note was played.
  133 frames of 54 x 60 px. The layout engine records which note every drawn head belongs to
  (`Score::heads`, tied continuations included), so the audio thread picks a mark per cell from
  the note's state without searching. Dots are drawn only in columns not yet passed, and marks
  have a lighter "trail" variant for passed columns, which keeps the strip at 7,980 px.
- **1,589 parameters**: 95 settings, 22 texts, 1,472 display parameters (660 cells, 660 marks,
  32 readouts).
  0.1.0 had 853 and the MPC handled them (and 774 widgets on PLAY) without slowing down; PLAY now
  has 1,438 widgets. Display parameters are not automatable and have constant (empty) display
  text. The MPC stores only the plugin's state chunk in a project, not per-parameter values, so
  the count does not bloat projects (checked against a saved project from this MPC).
- **Tabs:** every entry in TUI.json shows on the MPC as its own tab button, sub-tabs included
  (0.1.0's Night/Day sub-tabs doubled all seven), so only PLAY had a Day version (LIGHT), until 0.6.5 made it dark only (below).

### Limits respected (all from device measurements by others)

- Film strips taller than 16,384 px drift (MPC One, community notes); the tallest strip here is
  11,460 px.
- The MPC decodes skin images to RGBA and keeps a cache in RAM-backed `/var` per screen load
  (community notes, Live III). The whole skin is about 1.6 MB of PNG; the cell strips decode to
  about 2.5 MB each.
- Redrawing pictures costs the MPC's screen thread (a 566 × 122 strip at 15 fps used 25-55 % of
  a core on a Live II). So pictures change only when something changes: the plugin keeps the
  value it last pushed for every display parameter and sends only differences, at most 48 a block
  (96 for a full refresh after a reset, from a moving cursor). The default view (Halves) rewrites
  half the screen every few bars rather than scrolling every note.
- `audioMasterUpdateDisplay` rebuilds the page and closes popups (community wrapper), so it is
  used only for text, at most about ten times a second.

## From a MIDI file to slots

`plugin/src/sr/score.h`, on the worker thread:

1. Parse the file (`smf.h`): formats 0 and 1, running status, tempo, time and key signatures,
   track names. Notes become start/end in units of 1/48 quarter.
2. Quantise to the coarsest grid that fits (16th, triplet 8th, 32nd ...).
3. Hands: track names (right/left/treble/bass/upper/lower), else average pitch per track, else a
   split point.
4. Measures from the time signatures, with **pickup detection**: if the left hand enters after
   the right, its first onset marks a downbeat (checked against a metric score); otherwise a
   shift must beat no-shift clearly on downbeat weight. Für Elise, Old French Song and
   Träumerei come out with the right pickups; pieces starting on the downbeat are unchanged.
5. One voice per hand: notes that start together are a chord held until the next onset in
   that hand; short gaps are treated as legato.
6. Note values: split at barlines, then into notatable values; a note starting off the beat is
   tied at the next beat except the eighth + dotted-quarter syncopation.
7. Spelling: diatonic notes by key; chromatic notes by key and melodic direction (D# before E,
   Eb before D), leading tones sharp. Accidentals are tracked per bar.
8. Beams by beat group (flat beams at the extreme stem end), stem directions by the note
   furthest from the middle line, stems reaching the middle line for notes far outside the staff,
   ledger lines from the staff to the note, ties as arc pieces in the neighbouring row.
9. Slots: one per onset in each bar (both hands), one to three wide by duration. Each slot's 33
   cells are encoded once; the audio thread only copies slots to the screen.

Practice steps (what must be played together) are kept separately from the notation, so the
engine checks the real notes, not the simplified picture.

## The engine

`plugin/src/sr/engine.h` runs on the audio thread, without locks or allocation: Wait, Flow and
Rhythm modes, the slack settings, count-in and metronome, loops with a tempo ramp, reminders, and
the screen windows (Halves, Page, Bars, Steps). It reports events (wrong, miss, hit, beat, pass
done...) that the plugin turns into sounds, flashes, marks, accompaniment notes and progress
records. It keeps the last wrong pitch of every step (for the red ghost notes) and ignores
re-strikes: a note of the current step played again, or of the step just completed within 0.6 s.

The host calls the plugin's `reset()` on every resume (`effMainsChanged`) as well as after a state
load. 0.1.0 restarted the practice engine there, which on the MPC could send the player back to
the first note; 0.2.0 only silences sounds and re-sends the screen.

## Threads

- **Audio thread** (`process`): MIDI in → engine → events → LinnStrument light diffs → display
  diffs → sounds (a small synth: six timbres, an ADSR envelope, eight voices). Calls `audioMasterAutomate` (and rarely `audioMasterUpdateDisplay`) like the
  community wrapper does on devices.
- **Worker thread:** library scan, file loading and layout, the generator, fingering, progress
  files, finding the LinnStrument, reading the project's key. Hands finished pieces to the audio
  thread through an atomic pointer; the old piece comes back the same way to be freed.
- The host's UI thread only reads parameter names and texts (a mutex the audio thread never
  takes).

## LISTEN and the MIDI Out port

The MPC accepts a plugin's VST MIDI output and discards it (SDK `docs/host-contract.md`), so a
plugin cannot play another track that way. Sight Reader instead opens its own ALSA sequencer
client with a subscribable output port, "MIDI Out", which the MPC lists as a MIDI input for tracks
("Sight Reader MIDI Out"). The same route, with the same port flags, was measured on this MPC by
mpc-mpe's bridge: the MPC noticed the plugin's ports, subscribed to them, and every note arrived.

`plugin/src/sr/listen.h` plays the score's notes (the quantised version on screen) with their
velocities, following the file's tempo map times Listen At, on the audio thread, sending note-ons
and note-offs at block boundaries (under 3 ms apart at 128 samples). It ends sounding notes before
re-striking the same pitch, at the end of the range, and whenever it stops. The engine shows its
position on screen (`setListenSlot`) without touching the practice position.

## Captions and readouts (0.4.0)

Switch segments and buttons have their captions drawn into their on and off pictures, the chosen
segment a solid fill with contrasting text, rather than a text label under a translucent button
(0.1.0-0.3.0): nothing can cover them, and it is how the community skins do radio groups on the
device. Under each control a readout says what the current setting does. Text from the plugin
would need `audioMasterUpdateDisplay`, which rebuilds the page and closes the knob overlay you are
turning, so readouts are pictures: one frame per choice, driven by a display parameter that
mirrors the control (32 of them), or a fixed picture for controls whose value says it all. Value
text carries units, because the MPC shows a parameter's display text without its unit label.
Images: 66 MB decoded in all (0.1.0: 53 MB, which ran without trouble).

## Moving around and pieces in order (0.5.0)

**What the plugin can know of the MPC's transport.** A plugin gets the host's time info on every
block (SDK `ctx.transport`: playing or not, song position in quarter notes, bar start, time
signature, tempo) and nothing else: no button presses. So "the transport buttons" are followed
through their effect. While the MPC is stopped and its song position changes, the practice
position moves to the same bar and beat (bars counted from the sequence start with the MPC's time
signature); a change at the moment the MPC stops is ignored, so one Stop pauses where you are;
Stop twice is the MPC returning to the start, followed like any other move.

**Fields in the header.** A field is an empty container laid over the header's pictures, with the
data wheel's handle mapped to a parameter (Bar, Note, BPM, the title's piece number), a Q-Link
action on touch and the large editor on double-tap or push. Its focussed background is a
translucent tint, so a field shows when it has the wheel. Writing those parameters from the plugin
(the fields follow the cursor) goes through `setTracked()`, which also updates the data-wheel
helper's memory: the MPC's wheel sends a hundredth of a parameter's range per click, and
`nudge()` turns each such small move into one step, so it must know where the value really is.

**Restoring exactly.** The data-wheel helper must not take a project load for wheel clicks (a
restored value a hundredth of the range from the current one looks like a click). The entry
point marks `effSetChunk` (and holds a lock the worker takes when it writes list values back), so
values come back as saved; the host simulator's save/restore check passes 12 runs in a row.

**The queue and the piece list.** Loading a file copies its folder's list, in the list's order,
into the worker's queue. The piece list on PLAY is eight text parameters and eight highlight
pictures, shown while a parameter is on (`IndexedEnabling/1/2/<Piece List>` on the panel's
bounds) rather than a UI-only `Local/` value, so the plugin can close it once a piece is chosen.
The playlist is a text file of paths in `SightReader/playlist.txt`, shown as a last folder.

**8va lines.** Rows exist from four ledger lines below each staff to four above it (the grid of
pictures cannot grow). A note beyond that moves by an octave (two if needed), for the whole bar
when the rest of the bar still fits, otherwise for that note alone; the line is a mark-layer
picture on the slot (sign on the first slot, dashes on the rest). The layout keeps every head
where its note sits once the octave is taken off, and a test checks it.

**The LinnStrument's layer.** Cell paints (CC 20-22) go into the firmware's custom layer, shared
with custom light patterns, so the compositor (`linn.h`) remembers which cells it ever painted,
writes only those, and returns a cell it is finished with to a base colour (the note colours) or
off. Messages are paced by time, not by block (the MPC's block size varies): about 3,000 a second
by USB, whose last stage is a 115,200-baud serial link inside the instrument, and 900 a second
through a 31,250-baud DIN port.

## 0.5.5: the accompaniment clock, even wheel steps, the instrument's transposition

**Accompaniment in Wait mode.** Wait mode has no clock, so the hand you are not practising used
to be sent only for steps with nothing for you: all at once as they were skipped. Its notes that
fall with yours were never sent. Now:
- the engine keeps a second position for the accompaniment;
- when you complete a step, the accompaniment catches up to it (its notes in that step sound
  with yours);
- it then runs at the tempo towards the next step that wants you, and stops just short of it,
  so a note held up to there keeps sounding while it waits;
- after your last note it runs on to the end of the piece.

In Rhythm and Flow the clock already carried it.

**The wheel** (replaced in 0.6.5, see below). The MPC moves a number by a hundredth of its range per
click (an inference, not Akai's documents), and choices with replacement strings by one step. For
numbers, the plugin recognises a click and replaces it with one step from where the value was:
- a click is a move of about a hundredth, against the value it last showed or the host's last
  value, or a shorter move cut off by either end of the range;
- the step is one unit, or a round number of milliseconds on long time ranges.

Other moves pass unchanged.

**Transposition.** Cell paints address cells, so they move with the instrument's Octave and
Transpose, which the plugin cannot see.
- It asks: NRPN 299 makes the firmware answer with a parameter's value on the same channel
  (`sendNrpnParameter`, `midiSendNRPN`).
- The answer is read on a second, input-only ALSA client subscribed to the instrument's port.
  This keeps the audio thread's output handle unshared and does not depend on what the MPC
  routes to the track.

## 0.7.0: the other hand, end to end

The user practised one hand and heard nothing of the other. The 0.5.5 accompaniment clock
(above) was right in the unit tests. But nothing had ever checked what leaves the plugin: the
tests ran without ALSA, so `alsa_.sendMidi()` went nowhere.

**A fake ALSA for the tests.** `tests/fake_alsa.c` is built as a shared library with the soname
`libasound.so.2`. plugin_test loads it before the plugin, so the plugin's
`dlopen("libasound.so.2")` finds it. The plugin is unchanged. Every event is written to a trace
with the audio time the test sets before each block. The test then checks note-on and note-off
times on "MIDI Out" to the block (about 3 ms). With it:
- the 0.5.5 logic was confirmed;
- an intro went out as one burst, now fixed;
- a note on the waiting step sounded early after a locate, now fixed.

**What was wrong, then.** There were three causes, none in the clock itself:
- **Accomp was Off.** Projects keep what they saved, and Off was the default until 0.5.5. The
  extra state now carries `ver=<kVersion>`. A project without it (saved before 0.7.0) gets Accomp
  = Other Hand once, in `loadExtraState()`, which runs after the parameters are restored.
- **Accomp was out of sight**, on MORE. It is now on PLAY next to Hands, and the message line
  says what the other hand does when either changes.
- **A paused LISTEN swallowed notes**: the engine got none while `listening_`, which stays true
  while paused. A note while paused, or PLAY, now calls `takeOverFromListen()`: LISTEN stops and
  practice is located at the start of the bar LISTEN was in.

**The catch-up.** When you complete a step in Wait mode, `catchUpAccomp()` sends the
accompaniment the clock had not reached:
- notes within a sixteenth before the step, and notes still sounding at it;
- not older ones, which used to go out all at once (an intro started by playing, or the
  in-between notes when you are faster than the tempo).

`resetPosition()` and `locateStep()` put the clock one unit short of the step they land on (as
it waits between steps). So its notes on that step come with yours, and an intro still plays from
PLAY.

## 0.6.5: wheel steps, the transport, dark only

**The wheel, again.** 0.5.5 assumed the MPC moves a number by a hundredth of its range per click,
a figure inferred from the user's Tempo report (100, 102, 104, 105), not from Akai's documents. It
turned such moves into one step and passed every other move through. The user found the wheel
"jumping all over the place". The likely causes:
- a fast turn sends a bigger move;
- the MPC may step from a value the plugin has since changed;
- a preset was not marked as a load.

All of these passed through as jumps. Akai's ui-reference 3.3 is the documented way: a
`replacementStrings` entry gives a parameter N steps, and the data wheel, the overlays and the
Q-Links all honour them.
- So every int and note parameter now has its list of values, with the texts formatValue() would
  show (params.py `value_text`). The long time ranges step by 5 ms (`STEP`).
- That is 21,549 strings, and TUI.json went from 1.4 MB to 977 KB, because the LIGHT copy of PLAY
  went at the same time.
- Folder's knob shows the folder's name through a second handle (`Shown`, mapped to the folder
  name text).
- The plugin keeps one check, in exact steps (`hostStep`). If what the MPC sends is far from the
  value shown, but a small step from what the MPC itself last sent, the MPC was behind the plugin
  (Bar following the cursor, BPM following Tempo). Then the same step is made from the value shown.
- `effSetProgram` is marked as a load like `effSetChunk`, so presets are taken as they come.

**The transport.** LISTEN's pause (0.5.5) and the Bar field's seek were there but hard to find. The
second footer row now starts with a transport bar: |< (RESTART), <<, LISTEN, >>, PLAY, LOOP. The
arrows are two triggers (`bar_back`, `bar_fwd`). Each moves the Bar value by one from the bar shown
and asks for the same locate as the Bar field, which moves LISTEN when it is on and the practice
cursor otherwise. The icons are drawn as shapes in the button pictures (render.py
`transport_icon`), so no font has to have them.

**Dark only, and how to bring Light back.** A Light switch in SETUP, which the user asked for
first, cannot be done without more memory. A widget shows one image, and the MPC cannot make it
show another by a setting. Stacking both themes' frames in one strip would pass the 16,384 px
limit: the cell strips are 11,460 px. So a switch would mean both themes' staff layers (about
1,400 pictures each) in the PLAY page, one set hidden by IndexedEnabling, and both decoded when the
page loads. The two sub-tabs that 0.6.0 had loaded only the one shown. The user chose dark only.

To bring LIGHT back:
- set `WITH_LIGHT = True` in `tools/gen_skin.py` and rebuild;
- the Day theme (`THEMES["day"]` in render.py) is still drawn;
- PLAY's footer, the transport bar included, is built from the theme, so LIGHT gets it too;
- LIGHT returns as a second sub-tab on PLAY's key, as in 0.6.0
  ([what it looked like](img/play-day.png)).

**Pages.** LINN moved to the third function key (PLAY, LOAD, LINN, GEN, SETUP, MORE, SOUND,
COLOR, STATS). Nothing in the plugin depends on the order: Q-Links.json is written from the same
list.

## 0.6.0: the MIDI loader runs on the computer

The user wanted an easier way to load MIDI files than the command line, without making the
plugin bigger or busier. A web page served from the plugin would have needed:
- a listening socket and a thread in every instance;
- an upload parser;
- a way to keep strangers on the network out.

All of that would run on the MPC's CPU. So the page lives on the computer instead:
`loader/mpc-sightreading-midi-loader.py` is one standard-library Python file. It serves the page
on 127.0.0.1 only, and every API call carries a random key given to the page it served. It also
checks the Host and Origin headers, so another site open in the browser cannot reach it, even by
DNS rebinding.

It talks to the MPC with the computer's own `ssh`, as the SDK's `mpc_remote.py` does: the same
options, no known_hosts (the MPC changes its host key), and a ControlMaster connection reused
between commands.

Each operation is one or two SSH commands in plain POSIX `sh`. The tests run them under busybox
alone, with and without `md5sum`:
- **Look**: one command lists the drives with a `VST2 Plugins` or `SightReader` folder, where
  Sight Reader is installed and its version, and the library on those drives. It gives sizes
  (`wc -c`), checksums (`md5sum`, when there is one) and the `index.tsv` files.
- **Plan** (on the computer): the local library against that listing, and a manifest of what this
  computer put on that MPC.
  - The manifest is `synced.json`, keyed by a hash of `/etc/machine-id` and the library path.
  - Pieces missing or different there are copied.
  - Pieces in the manifest that are gone here are removed.
  - Everything else there is left alone and listed as only on the MPC.
  - Each collection's `index.tsv` is rebuilt from the computer's titles, plus the rows of the
    pieces that stay there.
- **Sync**: one tar archive in, owned by root. Then `rm` for the removals, `rmdir` for emptied
  collections, and last `.rescan` written through a temporary file and `mv`.

The plugin's side is the only new code on the MPC. The worker thread, which already wakes every
250 ms, calls `stat()` on `SightReader/Library/.rescan` every 2 s and compares inode, size and
nanosecond mtime. The `mv` gives a new inode each time, so a change is seen whatever the
filesystem's time resolution. Then it rescans as RESCAN does, finds the folder and file it was
showing by name and path, and puts the selection back on them.

## Files on the MPC

All inside `<drive>/SightReader/` next to `VST2 Plugins/` (the plugin finds its own folder with
the SDK's `mpc::pluginFolder()`): `Library/<folder>/*.mid` (+ `index.tsv`), `Library/.rescan`
(0.6.0, written by the loader and `tools/library.py push`), `Generated/`, `progress.tsv`,
`days.txt`, `playlist.txt` (0.5.0). Built-in drills are in the plugin folder's `Content/Drills/`.
Nothing else is written; no system file is touched.

## Sources

- Akai MPC plugin SDK, commit a5949cf (2026-11): `docs/host-contract.md`, `docs/ui-reference.md`,
  `framework/mpc_plugin.h`, host simulator, validator.
- Community: sunskiefer/mpc-vst-plugins `wrapper/vst2_wrap.c` and `docs/NOTES.md` (device traces:
  automate vs UpdateDisplay, film-strip limits, screen-thread cost, filmstrip cache).
- uncertainblume/mpc-mpe: ALSA ports from a plugin on MPC 3.10.1.12 (device findings), the
  LinnStrument notes. uncertainblume/mpcremote: project file format, `CurrentProject`.
- LinnStrument firmware (rogerlinndesign, Apache 2.0): `midi.md`, `ls_midi.ino`,
  `ls_handleTouches.ino`, `ls_midiLights.ino`.
- Bravura (SMuFL reference font, SIL OFL), Titillium Web (SIL OFL).
- Mutopia Project catalogue (crawled 2026-11-19 for `library/catalog.tsv`).
- Existing trainers for the feature set: Synthesia and PianoBooster (wait / follow-you modes),
  Sight Reading Factory (endless generated material), Piano Marvel (assessments, graded levels).
