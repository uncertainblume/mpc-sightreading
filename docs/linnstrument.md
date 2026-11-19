# LinnStrument features

Switch **Linn Mode** on (LINN page). The plugin finds the LinnStrument on the MPC's MIDI
system by itself (it looks every three seconds) and sends it light messages directly. The status
line says whether it is connected. Nothing here changes your LinnStrument's own settings: all
lights are overlays the instrument removes again, and **CLEAR LIGHTS** removes everything this
plugin lit.

![LINN, Night](img/linn-night.png)

## Guide lights

**Guide**: Off, Next (the notes to play now), Next+1 (now, plus the step after in white),
Fingered (only the suggested cell, in the finger's colour). **Guide Clr**:

- **Played** uses the LinnStrument's own highlight: a Note On sent to the instrument lights every
  cell of that note in your split's *Played* colour, and the Note Off clears it
  (`ls_midi.ino`, `highlightPossibleNoteCells`). It needs no layout settings, and the cell under
  your finger clears when you play it.
- Any other colour paints the cells with CC 20 (column), CC 21 (row), CC 22 (colour) in the
  firmware's custom layer, and clears them with colour 7. This needs the layout settings below.

**Linn Ch** is the channel for the Note On highlights: your split's main channel (1 for the left
split in MPE mode with the factory settings).

While **LISTEN** plays (PLAY page), the guide lights each note as it sounds instead of the next
one to play, in the same colour (or by finger with Fingering on): watch the shape of a passage
on the surface while you hear it.

## Fingering

**Fingering = Colours** picks a cell and a finger for every note, per hand, and colours the next
notes' cells: yellow thumb, green index, cyan middle, blue ring, magenta little. The header on
PLAY shows the finger numbers.

How it chooses: a hand position is the column of your index finger; one finger per column (like
a fretting hand), thumb one column back; reaching another row is cheap, moving the hand along
the rows is not, and the same finger on two different cells in a row costs extra. A Viterbi pass
over every candidate cell and hand position finds the cheapest path through the piece. A C major
scale comes out as the familiar box: C D on one row, E F G on the next, A B C on the next, all in
one hand position. Chord notes go where the hand already is.

It is a suggestion from geometry alone: it knows nothing of your hands or of phrasing. It is
planned when a piece loads, and again when you change the layout settings.

## LIGHT BARS

Lights every note of the loop (if Loop is on) or of the bars on screen, in **Bars Clr**, so you
can see the shape of a passage on the surface before playing it. Press again to clear. It
re-lights itself when you move the loop.

## Key Lights

Paints every cell of a key's scale in **Key Clr** (the root in white):

- **MPC Key**: the project key (the *key* the MPC shows in its Sounds browser and uses for key
  matching, saved as `"key": "C Major"` in the project file).
- **Pad Scale**: the pad-perform scale (`padPerformSettings`, root and scale type).
- **Piece Key**: the key signature of the piece loaded.

The MPC does not tell plugins about its key, so the plugin reads the project file the MPC saves:
`/data/Settings/MPC/CurrentProject` names it, and the newer of the project and its autosave is
read (gzip JSON). It re-reads every 20 seconds while Key Lights is on, and when you press
**READ KEY**. A change of key on the MPC shows up after the MPC saves (autosave is every
5 minutes by default; saving the project shows it at once).

## Note colours on the staff

The COLOR page puts a dot of each note's colour in the noteheads on the MPC screen, so the staff
speaks the same colour language as your instrument. Its palette is the LinnStrument's own colours
(the firmware's Red, Yellow, Green, Cyan, Blue, Magenta, White, Orange, Lime, Pink) plus
in-between hues for the black keys. The default scheme is C cyan, D blue, E pink, F magenta,
G white, A yellow, B orange, each black key the hue halfway between its neighbours except D#,
a deeper blue than D (halfway between blue and pink is a violet too close to F's magenta). It
changes nothing on the LinnStrument itself.

## Layout settings

The plugin needs your layout to know which cell plays which note (everything except the Played
guide uses it):

| Setting | Default | Meaning |
|---|---|---|
| Linn Size | 200 (25 columns) | or 128 (16 columns) |
| Row Offset | +5 (fourths) | +4, +3, +6, +7 or Octave |
| Lowest Note | F#1 (30) | the note of the bottom-left playing cell. Fourths: 30; octave offset: 18; add 12 for each octave you transposed up |
| Left-Handed | Off | mirrors the columns |

These follow the firmware's mapping (`determineRowOffsetNote`, `getNoteNumber`):
`note = lowest + row × offset + column - 1`. Splits and guitar tuning are not modelled; with a
split active, set Lowest Note for the split you play in.

## How the messages travel

The plugin opens its own ALSA sequencer client, "Sight Reader", with two ports:

- **Linn Out** cannot be subscribed to by anyone else (read-only, not exported), so light
  messages reach only the LinnStrument, never an MPC track listening to "All Ports" (where a
  guide Note On would play a sound). The plugin subscribes it to the LinnStrument's USB port
  itself; the kernel's USB MIDI driver only opens its output for a subscriber.
- **MIDI Out** is listed by the MPC as a track input ("Sight Reader MIDI Out") and carries only
  the accompaniment, when Accomp is on.

Light messages are rate-limited (about 24 per audio block) so a full repaint takes a few blocks.
This route was proven on this MPC by mpc-mpe (its ports open, the MPC detects them, channels
survive); sending *to* the LinnStrument this way is new and untested on the device.

## For a version without LinnStrument features

Everything LinnStrument-specific is behind Linn Mode, in `plugin/src/sr/linn.h`, `alsa_out.h`
and the `lightsBlock()` part of `SightReader.cpp`, plus the LINN page. A general version can drop
the LINN page from `tools/gen_skin.py` (PAGES) and leave Linn Mode off; nothing else depends on it.
