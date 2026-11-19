# Handoff: where Sight Reader stands

Written 2026-11-19 for the next working session (any model) and for the next test on the MPC.
Read the README, then this; [design.md](design.md) has the reasoning and sources.

## Status (0.5.0)

**What the first test on the MPC (0.1.0, Live III) showed**

- Works: the skin parses and every tab appears; the staff and notes draw correctly (so the
  film-strip frame mapping is right); the cursor band works; the MPC stays responsive with 853
  parameters and 774 widgets on PLAY.
- Every tab appeared twice, once per theme: the MPC shows sub-tabs as separate tab buttons.
- "The play logic doesn't work, the scrolling doesn't seem to happen", and the wrong-note blip
  was far too loud and harsh.

**What was wrong, and what 0.2.0 changes**

- The scale drill used for the test was written for **both hands** (C4 and C2 together) and
  Hands defaults to Both, so after the first right-hand note the plugin waited for the left
  hand, and every further right-hand note counted as wrong (with a blip). Nothing on screen said
  so. Now: Scales and Arpeggios are one hand per file (left-hand versions in their own folders,
  hands together in Two Hands); the header shows the next note of **each** hand; after two wrong
  notes that look like this slip, the message line says "Waiting for the left hand (C2)..."; an
  instrument an octave off gets its own message.
- `reset()` restarted the practice engine. The host calls it on every resume, so on the MPC a
  suspend/resume could send the player back to the first note. It no longer does.
- Re-striking a note of the current chord, or of the step just completed (within 0.6 s), counted
  as wrong; it is ignored now.
- At the end of a piece in Halves view the left half went blank; it keeps its music now.
- Played notes turn green, missed notes red, and a wrong note shows as a red note where it was
  played (a new layer of 660 film strips over the cells). The user's colour scheme is a dot in
  every notehead still to be played, editable per note (COLOR page).
- The square-wave blip is replaced by a small synth (Wood by default, five other sounds, volume,
  pitch, ADSR + length, reminder level, metronome click type and level: SOUND page). The default
  error sound peaks about 8 dB below 0.1's, and is a soft knock instead of a square wave.
- Only PLAY has a Day version now (tab LIGHT); the settings pages are dark only. 10 tabs instead
  of 14 (9 pages + LIGHT).

**0.3.0: LISTEN.** Asked for after 0.2.0: a MIDI out that plays the piece in time, at any speed,
to any plugin, as an example. The MPC discards a plugin's VST MIDI output, so LISTEN plays through
the plugin's own ALSA port "MIDI Out" (already there for Accomp; the same port type mpc-mpe's
bridge used on this MPC, where every note arrived). An instrument track with MIDI input "Sight
Reader MIDI Out" plays it. From the start of the current bar, or the loop round and round; speed
from the Tempo knob or fixed (Listen At, MORE); Both hands or the practised ones (Listen Hands);
channel Out Ch (also used by Accomp now). The cursor follows, the LinnStrument guide lights the
sounding notes, input is not graded, the practice position stays. Tested: the player's timing,
note-offs, hands and loop in unit tests; through VST2, a 16-bar piece at 200 % lasts 19.2 s with
the cursor and halves following and stops by itself, notes played meanwhile are not graded, PLAY
stops it, and with Loop on it repeats. **Not tested: the MIDI actually reaching a track on the
MPC** (no ALSA here).

**0.4.0: readable controls, D# navy.** After 0.3.0 the user said the switches (three-option ones
especially) did not show at a glance what each rounded rectangle did, and asked for specific
readouts like the header's. Now: captions drawn into the switch and button pictures (the chosen
one solid); a readout under every control saying what its current choice does (pictures driven by
32 display parameters mirroring the controls; fixed pictures for knobs); values with units
(160 ms, 30%, +2 st, Ch 1, Level 2, 3 times); the LINN and GEN pages re-laid out to make room, the
help paragraphs they replace removed. D#'s default colour is Navy (a deeper blue than D): the
in-between violet looked like F's magenta. Dark dot colours get a light edge. The open question
about the unfinished sentence in the first message is dropped (the user's call).

**0.5.0: moving around, pieces in order, 8va, LinnStrument route.** From the notes taken while
testing 0.4.0 (basic practice worked well with a drill):

- *Transport:* the header's **Bar**, **Note** and **BPM** are fields: tap one, turn the data
  wheel (or Q-Links 5-7); double-tap or push for the big knob. With Start = MPC Play, moving the
  MPC's playhead while stopped (locate, step, event/increment, Stop twice) moves the practice
  position; one Stop pauses. The plugin sees only the playhead and whether it is playing, never
  the buttons, so this is the closest it can get to "respond to the transport".
- *Loops:* Loop From / To / Bars keep each other consistent (From moves To with it, To pulls From,
  Bars stretches from From) and inside the piece. Loop To is on PLAY next to Loop From.
- *Several files:* loading a file makes its folder the queue: the title is a drop-down (the piece
  list: wheel chooses, push or tap loads), "(k/N)" after the title, **Auto Next** loads the next
  piece two seconds after one ends. **ADD TO LIST** / **REMOVE** on LOAD build a playlist
  (`SightReader/playlist.txt`), shown as the last folder.
- *LOAD by wheel:* the data wheel moves the selection through the whole folder, its push loads.
- *F#* is a light salmon pink.
- *Notes off the grid* (A1 was drawn on C#2's line in the A major LH arpeggio): they move by
  octaves under 8va / 8vb / 15ma / 15mb lines, per bar. The left-hand scale and arpeggio drills
  that ran below C2 start an octave higher.
- *LinnStrument:* nothing lit on the user's unit over USB, from the plugin or from an MPC MIDI
  track (bytes left the MPC; see linnstrument.md). New: **Linn Port** (USB, or the MPC's MIDI
  Out A/B to the instrument's MIDI In), **TEST LIGHTS**, cells painted only where needed and put
  back to the note colours (**Restore**) rather than off (paints share the layer of custom light
  patterns), pacing per route, **Linn Ch** default 2 (an MPE LinnStrument ignores highlights on
  channel 1), the layout settings on their own sub-page (LAYOUT).
- *Found while testing:* the data wheel's first click on Folder, Scroll and the loop jumped by a
  hundredth of the range instead of one (since 0.4.0); a project load could be misread as wheel
  clicks. Both fixed.

**Tested on a computer** (`tools/run_tests.sh`): unit tests under ASan/UBSan (now also 8va/8vb
layout with every head where its note sits, locating, the light compositor leaving untouched
cells alone and returning cells to their base colour); the built plugin through VST2 (everything
before, plus: the Bar/Note fields by wheel steps, BPM and Tempo % following each other, the loop
rules, the MPC playhead moving the position while stopped, the LOAD list by wheel, the piece list
by wheel and by tap, Auto Next after a piece played through, the playlist file, an 8va and an 8vb
line on screen, TEST LIGHTS with nothing connected); Akai's host simulator, 12 runs in a row
(its one warning is about silence). Validator: 0 errors, 0 warnings. **Not tested: anything on
the MPC itself** (the skin's new touch areas, the piece list's visibility switch, the MPC's own
data-wheel steps on the new fields, the DIN route).

## Next test on the MPC

Save your project first; plugins run without crash protection. Update with:

```bash
python3 third_party/mpc-plugin-builder-sdk/tools/mpc_remote.py 192.168.1.20 --drive 1 update dist/sight-reader-0.5.0-mpc.zip
```

It asks before restarting the MPC app. If Sight Reader is already on a track, the project keeps
its settings (new settings start at their defaults; Linn Ch keeps the value saved in the project,
so set it to 2 by hand if it says Ch 1).

1. **Header fields:** load a drill, tap **BAR** in the header: does it light up? Turn the data
   wheel: does the cursor jump a bar per click (not ten)? Same for **NOTE**, and **BPM** (the
   tempo changes, Tempo % follows). Double-tap BAR: the big knob. Q-Links 5-8: Bar, Note, BPM,
   Loop Bars, with the MPC's frame round the header.
2. **Piece list:** LOAD → Drills: Scales → first file → push the data wheel (it loads). On PLAY,
   the title says *(1/36)*. Tap the title: the list opens over the staff. Turn the wheel: the
   highlight moves; push: that scale loads and the list closes. Tap the title, tap a row: loads.
3. **Auto Next:** play a drill to the end: two seconds later the next one loads.
4. **Playlist:** on LOAD select a file, ADD TO LIST, another, ADD TO LIST; the last folder is
   *Playlist* with both, in that order. REMOVE takes the selected one out.
5. **Loop:** Loop From 3 → Loop To follows (3 + Loop Bars - 1); turn Loop To below Loop From:
   From follows it down.
6. **MPC transport:** SETUP → Start = MPC Play. With the MPC stopped, move its playhead (locate,
   the step or event buttons, Stop twice): the cursor follows to that bar. Play/Stop start and
   pause Rhythm and Flow.
7. **8va / 8vb:** Drills: Ledger Lines, or any piece with very high or low notes: an *8va* line
   over notes that went off the top, *8vb* under the bass for ones that went off the bottom. The A
   major LH arpeggio now starts on A2.
8. **LinnStrument:** LINN → **TEST LIGHTS** with Linn Port = USB. If nothing lights, try the
   computer test in linnstrument.md; then the MIDI-jacks route (two cables, LinnStrument MIDI I/O
   = MIDI Jacks, Linn Port = MPC Out A) and TEST LIGHTS again. Note what the STATUS line says.
9. **Responsiveness:** PLAY has a few more widgets (the piece list is 30 more); does it still
   switch quickly?

## Known risks and what to do

| Risk | Sign | Remedy |
|---|---|---|
| 1,647 parameters, about 1,480 widgets on PLAY, TUI.json 1.4 MB, ~70 MB of decoded images | slow plugin load or slow switch to PLAY | the skin can lay the cell and mark grids out with the format's `repeats` feature (one row of widgets repeated 20 times, `{Increment}` parameter indices; SDK ui-reference 4.6) instead of 1,320 entries: much smaller JSON, untested on the device. Or lower `SLOTS` (20) in `tools/skin/geometry.py`: 14 slots is ~460 cells |
| Screen-thread cost of many picture changes | the UI lags when a half page turns | keep View = Halves or Page; Note Colors off halves the pictures changed per turn; lower the per-block budget in `pushDisplay()` |
| MPC's film-strip RAM cache (`/var/volatile/tmp/filmstrips`) grows per screen load | `/var` fills over a day of reloads | delete `temp_*.img` there (files not open are safe to delete, per the community notes); ask before doing it |
| `audioMasterUpdateDisplay` rebuilds the page | popups close while the list or a message updates | it is only sent when text changes, at most about 10 a second |
| Text over 63 characters | cut off | titles and messages are kept under 60 |
| Removable drives are `noexec` | plugin missing on a USB stick | install on the internal drive |
| LinnStrument not found | status "not found" | check `/proc/asound/seq/clients` lists it; the plugin looks for a client name containing "LinnStrument" (USB) or a port named "DIN-A" / "DIN-B" (MPC Out A/B) |
| LinnStrument connected but dark | TEST LIGHTS shows nothing | the 0.4.0 finding (linnstrument.md): test from a computer, then try the MIDI-jacks route |
| The piece list does not open or close | tapping the title does nothing | it is shown while the parameter Piece List is on (`IndexedEnabling` on a parameter, as the SDK documents); if the MPC ignores it, the list can move to its own page |
| Data wheel on the new fields steps by more than one | Bar jumps ten bars a click | the plugin turns a step of a hundredth of the range into one; if the MPC's step is different here, `nudge()` in SightReader.cpp is the place |

## Decisions made without asking (change them if you like)

- **Identity:** manufacturer "Practice Tools", name "Sight Reader", uid `SgRd` (now saved in
  projects; changing them makes old projects lose the plugin).
- **Tabs:** only PLAY keeps a Day version (LIGHT), because every theme copy was a separate tab
  button. `PAGES` in `tools/gen_skin.py` lists the themes per page: adding "day" to a page brings
  its light version back.
- **Played-note colours:** green for right, red for missed and for wrong notes (drawn where you
  played them, rather than turning the written note red). Colour dots disappear once a note is
  played, so the green/red feedback and the dots never compete, and the dots lead the eye
  forward. Note Colors defaults to **on** with the user's scheme.
- **The in-between colours** (Azure, Orchid, Cream, Amber) are halfway round the colour circle
  between the neighbouring keys' colours (white counts as its neighbour's hue at half
  saturation). D# was Violet the same way, but violet reads as F's magenta, so at the user's
  request D# is Navy, a deeper blue than D (Violet stays in the palette). They exist on the
  screen only; the LinnStrument has no such colours.
- **Sounds:** Wood at 30 % of a 50 % master by default. The end-of-piece chime follows the Error
  Sound switch.
- **"Take the settings from the sounds page"** was read as: the MPC project's key (the key the
  Sounds browser uses) and the pad-perform scale, shown on the LinnStrument by Key Lights. If you
  meant something else, say what.
- **"Every tenth note after that"** was read as: after a mistake the sound repeats on every 10th
  note you play, up to 3 times (Remind, Reminders; its level is Remind Vol).
- **Scrolling:** the default view turns half a screen at a time rather than moving every note,
  because each picture change costs the MPC's screen thread (see design.md). The note-by-note
  view (Steps) is there to try.
- **One plugin with a Linn Mode switch**, as you suggested; linnstrument.md says how to make the
  version without it.
- **8va rather than a taller staff** (your call): a note more than about four ledger lines out
  moves by an octave; the whole bar moves if the rest of the bar still fits, so a passage keeps
  its shape. 15ma/15mb for two octaves.
- **The queue is the folder** you load from (or the Playlist), in the list's sort order; the
  playlist is one file per line in `SightReader/playlist.txt`, so it survives updates and can be
  edited on a computer. Auto Next is on by default and waits two seconds; it never runs while
  Loop is on.
- **Tempo and BPM:** whichever you set last wins when the next piece loads (BPM: every piece at
  that BPM; Tempo %: every piece at that share of its own tempo); saved with the project.
- **The header's Note field** counts notes (steps) from the start of the piece; it is the finest
  way to move with the data wheel.
- **Sources:** The Session's large folk-tune database was considered for melody reading, but its
  licence forbids processing the material with LLM tools, so it is not used. Copyrighted popular
  songs are not bundled: `tools/library.py add` takes your own files.

## Open questions for you

- LISTEN plays the quantised notes on screen, without the file's sustain pedal or other
  controllers. Do you want the file's own timing and pedal instead (closer to a performance,
  further from the notation)?
- Do you want light versions of the settings pages back (each adds a tab button)?
- Is the Live III the only MPC this needs to run on? (The 32-bit build is made but would be
  slower.)

## Ideas not built yet

- The LinnStrument guide lights in your note colours (nearest LinnStrument colour for the black
  keys).
- Note-name flash cards and interval drills that need no file (quick warm-ups).
- Better horizontal spacing (proportional within a bar), two voices per hand, angled beams,
  slurs, dynamics.
- MusicXML import (OpenScore and MuseScore exports read better than MIDI).
- A per-bar accuracy heat map on the PLAY staff; adaptive drills that repeat your weak bars.
- Recording your take and showing it against the score afterwards.
- A web or iPad companion (mpcremote) showing a bigger staff, since the MPC screen is small.
