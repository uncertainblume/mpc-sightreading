# Handoff: where Sight Reader stands

Written 2026-11-19 for the next working session (any model) and for the next test on the MPC.
Read the README, then this; [design.md](design.md) has the reasoning and sources.

## Status (0.3.0)

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

**Tested on a computer** (`tools/run_tests.sh`): unit tests under ASan/UBSan (now also: every
drawn head maps to its note, re-strikes, wrong-note memory); the built plugin through VST2
(LOAD, both hands' next notes, colour dots, green and red marks, the ghost note with its sharp on
a ledger line, a host resume that keeps the place, RESET COLORS, the stuck-hand and octave
messages, all six sounds' levels and a held tone's release, and a 16-bar piece played through:
the halves turn four times and it ends DONE at 100 %); Akai's host simulator (all checks pass;
its one warning is about silence). Validator: 0 errors, 0 warnings.

## Next test on the MPC

Save your project first; plugins run without crash protection. Update with:

```bash
python3 third_party/mpc-plugin-builder-sdk/tools/mpc_remote.py 192.168.1.20 --drive 1 update dist/sight-reader-0.3.0-mpc.zip
```

It asks before restarting the MPC app. If Sight Reader is already on a track, the project keeps
its settings (new settings start at their defaults).

1. **Tabs:** PLAY, LOAD, GEN, SETUP, MORE, SOUND, COLOR, LINN, STATS, and LIGHT on PLAY's key.
2. **LOAD → Drills: Scales → C major scale → LOAD.** Coloured dots in the noteheads (C cyan,
   D blue...). Play C D E... on the LinnStrument: each note turns green and the cursor moves on.
   A wrong note: a red note where you played it, the error sound (a soft knock).
3. **Scrolling:** keep going past the middle of the screen: the left half is rewritten with what
   follows (Halves view).
4. **SOUND:** TEST, try the six sounds, Error Vol, Master Vol.
5. **Two Hands → C major scale, hands together:** the header shows NEXT R and NEXT L; play only
   the right hand: after two wrong notes the message says the left hand is wanted.
6. **Responsiveness with twice the widgets on PLAY:** does switching to PLAY take noticeably
   longer than in 0.1.0? Does the screen keep up when a half turns with dots on?
   (`tools/mpc_threads.sh` from mpc-mpe gives per-thread CPU over SSH.)
7. **LISTEN:** add a track with any instrument, MIDI input **Sight Reader MIDI Out**, channel
   All, monitor In. Load a piece, press LISTEN on PLAY: the instrument plays it, the cursor
   follows. Turn Tempo while it plays. If the port is not listed: Preferences → MIDI / Sync →
   Input Ports, Track on for it; if it is listed but silent, say so (and whether Accomp sounds).
8. **LinnStrument** (LINN → Linn Mode On), as before; with Guide on, LISTEN lights the notes as
   they sound. The layout defaults (200, +5 row offset,
   lowest note F#1) already match a LinnStrument 200 with the default row offset.

## Known risks and what to do

| Risk | Sign | Remedy |
|---|---|---|
| 1,557 parameters, 1,438 widgets on PLAY, TUI.json 1.3 MB | slow plugin load or slow switch to PLAY | the skin can lay the cell and mark grids out with the format's `repeats` feature (one row of widgets repeated 20 times, `{Increment}` parameter indices; SDK ui-reference 4.6) instead of 1,320 entries: much smaller JSON, untested on the device. Or lower `SLOTS` (20) in `tools/skin/geometry.py`: 14 slots is ~460 cells |
| Screen-thread cost of many picture changes | the UI lags when a half page turns | keep View = Halves or Page; Note Colors off halves the pictures changed per turn; lower the per-block budget in `pushDisplay()` |
| MPC's film-strip RAM cache (`/var/volatile/tmp/filmstrips`) grows per screen load | `/var` fills over a day of reloads | delete `temp_*.img` there (files not open are safe to delete, per the community notes); ask before doing it |
| `audioMasterUpdateDisplay` rebuilds the page | popups close while the list or a message updates | it is only sent when text changes, at most about 10 a second |
| Text over 63 characters | cut off | titles and messages are kept under 60 |
| Removable drives are `noexec` | plugin missing on a USB stick | install on the internal drive |
| LinnStrument not found | status "not found" | check `/proc/asound/seq/clients` lists it; the plugin looks for a client name containing "LinnStrument" |

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
- **The in-between colours** (Azure, Violet, Orchid, Cream, Amber) are halfway round the colour
  circle between the neighbouring keys' colours (white counts as its neighbour's hue at half
  saturation). They exist on the screen only; the LinnStrument has no such colours.
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
- **Sources:** The Session's large folk-tune database was considered for melody reading, but its
  licence forbids processing the material with LLM tools, so it is not used. Copyrighted popular
  songs are not bundled: `tools/library.py add` takes your own files.

## Open questions for you

- Your first message ended with "there should also be a" — what was the rest?
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
