# Handoff: where Sight Reader stands

Written 2026-11-19 for the next working session (any model) and for the next test on the MPC.
Read the README, then this; [design.md](design.md) has the reasoning and sources.

## Start here (next session)

**0.5.5 (2026-11-20)** answered the user's notes from playing 0.5.1:
- LISTEN transport: pause/go on, seek with Bar/Note/jog/MPC playhead, SET for the loop ends.
- Fit Range (Fold/Skip) for the LinnStrument's surface.
- The guide moves on as notes are pressed, re-lights after a release, and has After Clr.
- The LinnStrument's Octave/Transpose (LAYOUT, read by NRPN 299 over a new input port).
- The accompaniment plays the other hand in time and waits in Wait mode; it is on by default.
- Even jog-wheel steps on every number setting.
- Rebuilds keep the bar.

All tested on a computer, none on the MPC yet. Ask the user to try, in this order:
1. The jog wheel on Tempo and on a channel.
2. LISTEN pause/SET.
3. One hand with Accomp on and an instrument track on Sight Reader MIDI Out.
4. Hall of the Mountain King with Fit Range = Fold.
5. Transposing the LinnStrument with a colour guide (the LINN status line should say "Read:
   Octave ...").

**Next, 0.6.0** (the user's plan): an easier way to load new MIDI files than the command line.
Discuss strategies with the user before building. Ideas to bring:
- a small web page served from the MPC (or from the plugin);
- a watched "Inbox" folder on a USB stick, copied in by RESCAN;
- pulling from a shared Google Drive or Dropbox link;
- mpcremote integration.

The user said 0.6.0 comes after 0.5.5 is tried.

**Where things were (end of 2026-11-19).**

- 0.5.0 is released (0.5.1 and 0.5.5 since; every release goes to `main` with the README
  updated, and the README keeps the user's PayPal tip line and QR code).
  - `main` holds it (PRs #1-#3).
  - GitHub releases v0.1.0 to v0.5.0 exist, each with its zip; v0.5.0 is Latest and points at
    `main` (a9cf405).
  - The working branch is `claude/intelligent-gauss-fy2z0x`. Its few commits after `main` are
    the Mac test script, docs and this section; merge them into `main` when the user agrees.
- The repository's *default* branch was still the working branch. The user was asked to switch
  it to `main` (Settings > General); check before relying on either.
- The user has installed 0.5.0 on the MPC and is testing it. Their notes come in as messages.
  They often say "hold on until I say go": collect the notes, and start a version only on their
  go.

**LinnStrument lights: they reach the instrument, but the guide is not worked out yet.**
This is where to pick up. What is settled (end of 2026-11-19):

- From a Mac, SendMIDI's prebuilt binary lit the LinnStrument as expected. CC 20/21/22 painted
  the cell in column 5, row 3 green. A Note On C4 on **channel 2** highlighted every C in the
  user's Played colour. Their LinnStrument is in MPE mode, so per-note channels highlight and
  Linn Ch = 2 is right for them.
- On the MPC, **0.5.0's TEST LIGHTS worked after the LinnStrument was unplugged and plugged
  back in.**
- Every earlier failure on the MPC therefore looks like a stuck USB state that a replug clears.
  That covers 0.4.0's lights, hand-sent `aplaymidi` messages, an MPC MIDI track, and the first
  TEST LIGHTS in 0.5.0. Bytes left the MPC throughout: the kernel's Tx counter for the
  LinnStrument went up.
- Not known yet: what gets it stuck. One guess is having the LinnStrument plugged in while the
  MPC starts up, or for a long session.
- Ask the user to note when lights stop working, and whether a replug always brings them back.
  If that is the pattern, the README and linnstrument.md should say "replug the LinnStrument"
  as the first fix. They already mention it.
- A plugin-side "reconnect" (drop and redo the ALSA subscription, `alsa_out.h`
  `findAndConnectLinn`) is cheap to add. A replug resets the instrument's USB side, which the
  plugin cannot do.
- The MIDI-jacks route (Linn Port = MPC Out A/B) is built but **untested and no longer
  needed**. Keep it.

**0.5.1 (2026-11-20) fixed one of the odd behaviours.** The user reported that playing left
cells recoloured (white, A and Ab orange), so their layout drifted over a long piece. Cause,
from the firmware source (rogerlinndesign/linnstrument-firmware):
- CC 20/21/22 paints go into LED_LAYER_CUSTOM1, which also holds the active custom light pattern
  (copied from `Device.customLeds`).
- CC 22 colour 0 clears a cell from that layer; colour 7 paints it black.
- Note On highlights go into LED_LAYER_PLAYED.
- NRPN 247 (active note lights preset, 9-11 = custom patterns 1-3) reloads the stored pattern
  (`loadCustomLedLayer`).

The fixes:
- The Played guide no longer paints (Next+1 uses highlights too).
- Restore offers Pattern 1-3 (reload by NRPN 247, then repaint what is still lit).
- Off became Instrument (colour 0).

Not yet tried on the instrument: ask whether the layout now stays put, and whether Restore =
Pattern n restores it after TEST LIGHTS.

**What is not settled: the guide behaves oddly.** With lights working, the user tried the guide
and reports that **Next** and **Next+1** (two ahead) behave weirdly: "light changes happen but we
have to debug a bunch of weird behaviors". They chose to leave it for the next session. The
behaviours have not been described in detail yet, so start by asking for each one:
- the settings: Guide, Guide Clr, Fingering, Restore, Linn Ch, and the LAYOUT values;
- what was on screen, what they expected to light, and what lit or went dark instead;
- a phone video helps.

Candidates to check against what they describe (from reading `lightsBlock()` in
`SightReader.cpp` and `LinnLights` in `sr/linn.h`; none confirmed):
- **Every cell of the pitch lights.** `setNote()` paints the note on every row it appears on
  (several cells on a fourths layout). With Next+1 that doubles, so it can be unclear which cell
  to play. *Fingered* lights one cell per note.
- **Played-colour highlights and the player's own touches.** Guide Clr = Played uses Note
  On/Off on Linn Ch.
  - The instrument clears a highlighted cell when it is played.
  - A Note Off from the plugin for a pitch the player is holding on the same MPE channel may
    disturb the touch highlight.
  - Highlights and paints are paced differently (Note Ons go at once, paints through
    `flushLights()`), so the two can be out of step for a moment.
- **Next+1 mixes two mechanisms.** The current step uses Note On highlights; the step after uses
  white cell paints. A note in both steps (repeated notes, held chords) gets both.
- **Restore = Note Colors.**
  - A cell the guide leaves goes to the plugin's version of the note colours. The user's own
    scheme on the instrument may differ, so cells the guide has passed can look recoloured.
  - Restore = Off sends colour 7, which turns the painted cell off.
- **LAYOUT mismatch.** Paints land on the wrong notes if size, row offset, lowest note
  (octave/transpose on the instrument) or handedness differ from the instrument. Played
  highlights do not depend on LAYOUT, so if Played looks right and colours look wrong, suspect
  LAYOUT.
- **When it updates.** The guide follows `engine_.currentStep()` and `nextRequiredStep()`.
  - In Wait mode, after a wrong note or a re-strike, the guide stays.
  - In Halves view it does not depend on the screen.
  - Check LISTEN too: the guide shows sounding notes then.
- **Paint ordering.** `flush()` sends CC 21 (row), CC 20 (column), CC 22 (colour) and skips CC
  20/21 when they did not change. If the instrument loses one message (it once ignored
  everything until replugged), every later paint could land on the wrong cell until the cursor
  is resent. `resetCursor()` on each change would make every paint self-contained, at three
  messages per cell.

**Still to check with the LinnStrument on the MPC** (now that lights work):

- Guide = Next with Guide Clr = Played: the Note On highlights on Linn Ch 2.
- Guide with a colour: the cell paints, which need LAYOUT to match the instrument (200, +5,
  F#1).
- **Restore = Note Colors.** Cells go back to the COLOR page colours, not dark. The user has a
  custom colour scheme on the instrument, so ask whether the restored colours look right.
- Fingering, LIGHT BARS, Key Lights (MPC Key needs a saved project), and LISTEN lighting the
  notes as they sound.
- The rest of 0.5.0's checklist: [Next test on the MPC](#next-test-on-the-mpc) below.

**`tools/linn_test_mac.py`.**

- The first version failed on the user's Mac: it listed the destinations but could not read
  their names, so it never found the LinnStrument. SendMIDI found and drove it fine.
- The second version is untested. It:
  - creates the CoreMIDI client before enumerating;
  - runs the run loop for a second;
  - falls back from the display name to the plain name;
  - prints the devices, and the USB tree from `ioreg` when nothing matches.
- Low priority: the Mac test is done. Either get output from the user and fix it, or remove the
  script and point linnstrument.md at SendMIDI's prebuilt zip only.

**How releases work now.**

1. Bump the version in `plugin/plugin.json`, `kVersion` in `SightReader.cpp` (e.g. 600 for
   0.6.0) and `plugin/README.md`.
2. Run `tools/build.sh` and `tools/run_tests.sh`. Package with the SDK's `tools/package.py`
   (see Build in the README) and move the zip to `dist/sight-reader-X.Y.Z-mpc.zip`, removing the
   previous zip from `dist/`.
3. Add `## [X.Y.Z] - date` and its link line to `CHANGELOG.md`.
4. Merge into `main` through a PR, only with the user's go.
5. `.github/workflows/release.yml` then publishes the release by itself.

From the cloud session, tags cannot be pushed (403), and the workflow cannot be started by hand
(the integration may not dispatch workflows). That is why it runs on pushes to `main`.

**Standing instructions from the user.**

- Work on the branch.
- Merge to `main` only for an approved version.
- Never run `mpc_remote.py` add/update/remove/restart yourself. Give the user the command; they
  update the MPC themselves. The MPC is at 192.168.1.20 on their network, which the cloud session
  cannot reach.
- Do not use The Session's tune database (its licence forbids processing with LLM tools).
- Keep the user's email out of everything.
- Commits end with the Co-Authored-By / Claude-Session lines the harness gives.

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
- *LinnStrument:* with 0.4.0 nothing lit on the user's unit over USB, from the plugin or from an
  MPC MIDI track. Bytes left the MPC; see linnstrument.md. After 0.5.0 TEST LIGHTS worked once
  the instrument was unplugged and plugged back in (see Start here). New in 0.5.0:
  - **Linn Port**: USB, or the MPC's MIDI Out A/B to the instrument's MIDI In.
  - **TEST LIGHTS**.
  - **Restore**: cells are painted only where needed and go back to the note colours rather
    than off (paints share the layer of custom light patterns).
  - Pacing per route.
  - **Linn Ch** defaults to 2: an MPE LinnStrument ignores highlights on channel 1.
  - The layout settings are on their own sub-page (LAYOUT).
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
python3 third_party/mpc-plugin-builder-sdk/tools/mpc_remote.py 192.168.1.20 --drive 1 update dist/sight-reader-0.5.5-mpc.zip
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
| LinnStrument connected but dark | TEST LIGHTS shows nothing | unplug the LinnStrument and plug it back in (that fixed it on 2026-11-19); then test from a computer (linnstrument.md); the MIDI-jacks route is the last resort |
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
