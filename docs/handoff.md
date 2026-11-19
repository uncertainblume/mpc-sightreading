# Handoff: where Sight Reader stands

Written 2026-11-19 for the next working session (any model) and for the first test on the MPC.
Read the README, then this; [design.md](design.md) has the reasoning and sources.

## Status (0.1.0)

Built and tested on a computer only:

- Akai SDK build (`tools/build.sh`): arm32 + arm64, validator **0 errors, 0 warnings**.
- `tools/run_tests.sh`: core unit tests under AddressSanitizer/UBSan (spelling, keys, a two-hand
  file round trip, pickup detection, wait and rhythm engine runs, tolerances, the generator in 144
  setting combinations, LinnStrument mapping, fingering, light messages); then the built plugin
  driven through VST2 with a library on disk (scan, LOAD, right and wrong notes, counters,
  overlays, flash); then Akai's host simulator (all checks pass; its one warning is that the
  plugin is silent, which is by design).
- All 70 catalogue pieces and all 148 drills load and lay out.
- Screenshots in `docs/img` are rendered from the generated skin with the plugin's real parameter
  values (`tools/skin/render_page.py`), not mock-ups. They approximate the MPC's drawing.

**Never run on an MPC.** The way the staff is drawn (660 film-strip cells driven by the plugin)
is beyond anything the SDK examples or SurgeXXL do, though each mechanism has been seen working
on devices separately.

## First run on the MPC: what to check, in order

Save your project first; plugins run without crash protection.

1. **Install** `plugin/build/Practice Tools - VST - Sight Reader` into `/nvme/VST2 Plugins`
   (SDK: `mpc_remote.py <ip> --drive 1 update plugin --yes`), restart, add Sight Reader to a
   plugin track.
2. **Tabs appear?** If the page is empty with no function-key tabs, TUI.json failed to parse.
   Send `journalctl` lines around "Initialising VST: Sight Reader".
3. **PLAY shows the empty staff** (clefs, brace, lines) and the header says NO FILE.
4. **LOAD → Drills: Scales → C major → LOAD.** Notes should appear on the staff. Check that
   noteheads sit exactly on lines and spaces and stems join up. If every cell is consistently one
   glyph "off" (wrong accidental, a stem where a head should be), the MPC maps values to film-strip
   frames differently from the SDK's knobs: report it; the fix is one line (`frameValue` in
   `SightReader.cpp`).
5. **Play C D E ...** on the LinnStrument in Wait mode (Sight Reader's track input = LinnStrument,
   monitor Merge). The cursor should advance, notes fade, a wrong note flashes red and blips.
6. **Responsiveness:** does the screen keep up when the half page turns? Does the MPC UI feel
   sluggish while it runs? `tools/mpc_threads.sh` from mpc-mpe gives per-thread CPU over SSH.
7. **LOAD page text:** the list rows, folder name and details update when you scroll.
8. **Night/Day:** press PLAY's function key twice.
9. **LinnStrument:** LINN → Linn Mode On. Status should read "LinnStrument connected" within three
   seconds. Guide (Played) lights the next note via the instrument's played colour; then try
   Guide Clr = Blue, Fingering = Colours, LIGHT BARS, Key Lights = MPC Key.
10. **Files:** after finishing a drill, `/nvme/SightReader/progress.tsv` should exist.

## Known risks and what to do

| Risk | Sign | Remedy |
|---|---|---|
| 853 parameters is ~4x SurgeXXL's | slow plugin load, MPC memory, sluggish parameter lists | lower `SLOTS` (20) in `tools/skin/geometry.py` and rebuild: 14 slots is ~460 cells |
| Screen-thread cost of many picture changes | the UI lags when a half page turns | keep View = Halves or Page; lower the per-block budget in `pushDisplay()` |
| MPC's film-strip RAM cache (`/var/volatile/tmp/filmstrips`) grows per screen load | `/var` fills over a day of reloads | delete `temp_*.img` there (files not open are safe to delete, per the community notes); ask before doing it |
| `audioMasterUpdateDisplay` rebuilds the page | popups close while the list updates | it is only sent when text changes, at most about 10 a second |
| Text over 63 characters | cut off | titles are trimmed to 60 |
| Removable drives are `noexec` | plugin missing on a USB stick | install on the internal drive |
| LinnStrument not found | status "not found" | check `/proc/asound/seq/clients` lists it; the plugin looks for a client name containing "LinnStrument" |

## Decisions made without asking (change them if you like)

- **Identity:** manufacturer "Practice Tools", name "Sight Reader", uid `SgRd`. These become
  permanent once a project saves the plugin; change them before first use (`plugin/plugin.json`
  and `makeInfo()` in `SightReader.cpp`) if you want your own brand.
- **"Take the settings from the sounds page"** was read as: the MPC project's key (the key the
  Sounds browser uses) and the pad-perform scale, shown on the LinnStrument by Key Lights. If you
  meant something else, say what.
- **"Every tenth note after that"** was read as: after a mistake the blip repeats on every 10th
  note you play, up to 3 times (both adjustable: Remind and Reminders).
- **Scrolling:** the default view turns half a screen at a time rather than moving every note,
  because each picture change costs the MPC's screen thread (see design.md). The note-by-note
  view (Steps) is there to try.
- **Themes** are sub-tabs (press the page key again), because a plugin cannot recolour pictures.
- **One plugin with a Linn Mode switch**, as you suggested; linnstrument.md says how to make the
  version without it.
- **Sources:** The Session's large folk-tune database was considered for melody reading, but its
  licence forbids processing the material with LLM tools, so it is not used. Copyrighted popular
  songs are not bundled: `tools/library.py add` takes your own files.

## Open questions for you

- Your message ended with "there should also be a" — what was the rest?
- Which LinnStrument model and settings (128 or 200, row offset, transposition, splits)?
- Is the Live III the only MPC this needs to run on? (The 32-bit build is made but would be
  slower.)

## Ideas not built yet

- Note-name flash cards and interval drills that need no file (quick warm-ups).
- Better horizontal spacing (proportional within a bar), two voices per hand, angled beams,
  slurs, dynamics.
- MusicXML import (OpenScore and MuseScore exports read better than MIDI).
- A per-bar accuracy heat map on the PLAY staff; adaptive drills that repeat your weak bars.
- Recording your take and showing it against the score afterwards.
- A web or iPad companion (mpcremote) showing a bigger staff, since the MPC screen is small.
