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
  (0.1.0's Night/Day sub-tabs doubled all seven), so only PLAY has a Day version (LIGHT).

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

**The wheel.** The MPC moves a number by a hundredth of its range per click (Akai's
ui-reference 3.3: the coarse increment), and choices with replacement strings by one step. For
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

## Files on the MPC

All inside `<drive>/SightReader/` next to `VST2 Plugins/` (the plugin finds its own folder with
the SDK's `mpc::pluginFolder()`): `Library/<folder>/*.mid` (+ `index.tsv`), `Generated/`,
`progress.tsv`, `days.txt`, `playlist.txt` (0.5.0). Built-in drills are in the plugin folder's `Content/Drills/`. Nothing
else is written; no system file is touched.

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
