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
- **853 parameters**: 64 settings, 22 texts, 767 display parameters (660 cells). Display
  parameters are not automatable and have constant (empty) display text. The MPC stores only the
  plugin's state chunk in a project, not per-parameter values, so the count does not bloat
  projects (checked against a saved project from this MPC).

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
done...) that the plugin turns into blips, flashes, accompaniment notes and progress records.

## Threads

- **Audio thread** (`process`): MIDI in → engine → events → LinnStrument light diffs → display
  diffs → blips. Calls `audioMasterAutomate` (and rarely `audioMasterUpdateDisplay`) like the
  community wrapper does on devices.
- **Worker thread:** library scan, file loading and layout, the generator, fingering, progress
  files, finding the LinnStrument, reading the project's key. Hands finished pieces to the audio
  thread through an atomic pointer; the old piece comes back the same way to be freed.
- The host's UI thread only reads parameter names and texts (a mutex the audio thread never
  takes).

## Files on the MPC

All inside `<drive>/SightReader/` next to `VST2 Plugins/` (the plugin finds its own folder with
the SDK's `mpc::pluginFolder()`): `Library/<folder>/*.mid` (+ `index.tsv`), `Generated/`,
`progress.tsv`, `days.txt`. Built-in drills are in the plugin folder's `Content/Drills/`. Nothing
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
