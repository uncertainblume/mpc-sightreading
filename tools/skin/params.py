"""Parameter table: the plugin's whole interface to the MPC.

Order is permanent once projects exist (the MPC saves the plugin's state by parameter id, but
the skin binds widgets by index). Append only.

Kinds:
  float/int/bool/choice  ordinary settings (saved in the project chunk)
  trigger                a bool the plugin resets to 0 after acting on it (a button)
  text                   a read-only parameter whose display text is a string the plugin sets
                         (titles, list rows); shown with a Label of type Value
  display                a read-only picture parameter: the value selects a film-strip frame
"""
import geometry as G
import readouts as RO

NOTE = "note"     # int displayed as a note name

# id, name, kind, (min, max, default) or (choices, default), unit
CONTROL = [
    ("blip_vol",   "Master Vol",  "int",    (0, 100, 50), "%"),          # parameter 0: harmless (id kept for saved projects)
    ("mode",       "Mode",        "choice", (["Wait", "Flow", "Rhythm"], 0), ""),
    ("tempo",      "Tempo",       "int",    (20, 200, 100), "%"),
    ("hands",      "Hands",       "choice", (["Both", "Right", "Left"], 0), ""),
    ("view",       "View",        "choice", (["Halves", "Page", "Bars", "Steps"], 0), ""),
    ("play",       "Play",        "bool",   (0, 1, 0), ""),
    ("restart",    "Restart",     "trigger",(0, 1, 0), ""),
    ("loop",       "Loop",        "bool",   (0, 1, 0), ""),
    ("loop_from",  "Loop From",   "int",    (1, 999, 1), ""),
    ("loop_bars",  "Loop Bars",   "int",    (1, 32, 4), ""),
    ("timing",     "Timing",      "int",    (40, 400, 160), "ms"),
    ("pitch_tol",  "Pitch Tol",   "choice", (["Exact", "Octave", "Near"], 0), ""),
    ("chord_tol",  "Chords",      "choice", (["All", "All-1", "Any"], 0), ""),
    ("hold",       "Hold",        "choice", (["1/2 Beat", "1 Beat", "2 Beats", "4 Beats", "8 Beats", "Forever"], 2), ""),
    ("wrong",      "Wrong Note",  "choice", (["Ignore", "Count", "Stop"], 1), ""),
    ("count_in",   "Count-in",    "choice", (["Off", "1 Bar", "2 Bars"], 1), ""),
    ("metro",      "Metronome",   "choice", (["Off", "On", "Accent"], 0), ""),
    ("err_blip",   "Error Sound", "bool",   (0, 1, 1), ""),
    ("remind",     "Remind",      "choice", (["Off", "Every 5", "Every 10", "Every 20"], 2), ""),
    ("remind_max", "Reminders",   "int",    (1, 20, 3), ""),
    ("flash",      "Red Flash",   "bool",   (0, 1, 1), ""),
    ("range_lo",   "Range Lo",    NOTE,     (21, 108, 21), ""),
    ("range_hi",   "Range Hi",    NOTE,     (21, 108, 108), ""),
    ("out_range",  "Out Range",   "choice", (["Play", "Skip", "Hide"], 1), ""),
    ("mask",       "Mask",        "choice", (["Off", "Played", "Bar"], 0), ""),
    ("names",      "Note Names",  "choice", (["Off", "Next", "Next+Played"], 2), ""),
    ("accomp",     "Accomp",      "choice", (["Off", "Other Hand", "All"], 0), ""),
    ("start_mode", "Start",       "choice", (["First Note", "Play Button", "MPC Play"], 0), ""),
    ("split",      "Split",       NOTE,     (36, 84, 60), ""),
    ("transpose",  "Transpose",   "int",    (-12, 12, 0), "st"),
    ("ramp",       "Tempo Ramp",  "choice", (["Off", "+2%", "+5%", "+10%"], 0), ""),
    ("goal",       "Goal",        "int",    (50, 100, 90), "%"),
    # LOAD
    ("folder",     "Folder",      "int",    (0, 63, 0), ""),
    ("scroll",     "Scroll",      "int",    (0, 1999, 0), ""),
    ("row",        "Row",         "choice", (["1", "2", "3", "4", "5", "6", "7", "8"], 0), ""),
    ("load",       "Load",        "trigger",(0, 1, 0), ""),
    ("sort",       "Sort",        "choice", (["Name", "Best %", "Recent", "Level"], 0), ""),
    ("rescan",     "Rescan",      "trigger",(0, 1, 0), ""),
    # GEN
    ("gen_key",    "Key",         "choice", (["C", "G", "D", "A", "E", "B", "F#", "Db", "Ab", "Eb", "Bb", "F", "Random"], 0), ""),
    ("gen_scale",  "Scale",       "choice", (["Major", "Minor", "Dorian", "Phrygian", "Lydian", "Mixolydian", "Locrian",
                                              "Harm Minor", "Maj Pent", "Min Pent", "Blues", "Chromatic"], 0), ""),
    ("gen_rhythm", "Rhythm",      "int",    (1, 5, 2), ""),
    ("gen_leaps",  "Leaps",       "int",    (1, 5, 2), ""),
    ("gen_hands",  "Gen Hands",   "choice", (["Right", "Left", "Both"], 0), ""),
    ("gen_bars",   "Bars",        "choice", (["4", "8", "16", "32"], 1), ""),
    ("gen_meter",  "Meter",       "choice", (["4/4", "3/4", "2/4", "6/8"], 0), ""),
    ("gen_range",  "Gen Range",   "choice", (["5 Notes", "Octave", "2 Octaves", "Wide"], 1), ""),
    ("gen_lh",     "LH Style",    "choice", (["Single", "Intervals", "Triads", "Broken"], 0), ""),
    ("gen_go",     "Generate",    "trigger",(0, 1, 0), ""),
    ("gen_save",   "Save",        "trigger",(0, 1, 0), ""),
    # LINN
    ("linn",       "Linn Mode",   "bool",   (0, 1, 0), ""),
    ("linn_guide", "Guide",       "choice", (["Off", "Next", "Next+1", "Fingered"], 1), ""),
    ("linn_gcol",  "Guide Clr",   "choice", (["Played", "Red", "Yellow", "Green", "Cyan", "Blue", "Magenta", "White", "Orange", "Lime", "Pink"], 0), ""),
    ("linn_sect",  "Light Bars",  "trigger",(0, 1, 0), ""),
    ("linn_scol",  "Bars Clr",    "choice", (["Red", "Yellow", "Green", "Cyan", "Blue", "Magenta", "White", "Orange", "Lime", "Pink"], 4), ""),
    ("linn_clear", "Clear Lights","trigger",(0, 1, 0), ""),
    ("linn_finger","Fingering",   "choice", (["Off", "Colours"], 0), ""),
    ("linn_key",   "Key Lights",  "choice", (["Off", "MPC Key", "Pad Scale", "Piece Key"], 0), ""),
    ("linn_kcol",  "Key Clr",     "choice", (["Red", "Yellow", "Green", "Cyan", "Blue", "Magenta", "White", "Orange", "Lime", "Pink"], 2), ""),
    ("linn_size",  "Linn Size",   "choice", (["200 (25 col)", "128 (16 col)"], 0), ""),
    ("linn_rowoff","Row Offset",  "choice", (["+5 (4ths)", "+4", "+3", "+6", "+7", "Octave"], 0), ""),
    ("linn_low",   "Lowest Note", NOTE,     (0, 72, 30), ""),
    ("linn_ch",    "Linn Ch",     "int",    (1, 16, 1), ""),
    ("linn_left",  "Left-Handed", "bool",   (0, 1, 0), ""),
    ("linn_sync",  "Read Key",    "trigger",(0, 1, 0), ""),
    # COLOR (0.2.0): note colour dots and played-note marks
    ("note_colors","Note Colors", "bool",   (0, 1, 1), ""),
    ("feedback",   "Feedback",    "choice", (["Off", "Green", "Green+Red"], 2), ""),
    ("col_reset",  "Reset Colors","trigger",(0, 1, 0), ""),
] + [("col_%s" % pid, name, "choice", (G.PALETTE_NAMES, d), "") for (pid, name, d) in G.NOTE_COLOUR_DEFAULTS] + [
    # SOUND (0.2.0)
    ("snd_type",   "Sound",       "choice", (G.SOUND_TYPES, 0), ""),
    ("snd_vol",    "Error Vol",   "int",    (0, 100, 30), "%"),
    ("snd_pitch",  "Pitch",       "int",    (-24, 24, 0), "st"),
    ("snd_attack", "Attack",      "int",    (0, 200, 2), "ms"),
    ("snd_decay",  "Decay",       "int",    (5, 2000, 160), "ms"),
    ("snd_sustain","Sustain",     "int",    (0, 100, 0), "%"),
    ("snd_release","Release",     "int",    (5, 2000, 60), "ms"),
    ("snd_length", "Length",      "int",    (0, 2000, 0), "ms"),
    ("snd_remind", "Remind Vol",  "int",    (0, 100, 50), "%"),
    ("click_type", "Click",       "choice", (G.CLICK_TYPES, 0), ""),
    ("click_vol",  "Click Vol",   "int",    (0, 100, 40), "%"),
    ("snd_test",   "Test",        "trigger",(0, 1, 0), ""),
    # LISTEN (0.3.0): the piece played in time through "Sight Reader MIDI Out"
    ("listen",     "Listen",      "bool",   (0, 1, 0), ""),
    ("listen_at",  "Listen At",   "choice", (["Tempo", "25%", "50%", "75%", "100%", "125%", "150%"], 0), ""),
    ("listen_hands","Listen Hands","choice", (["Both", "Practised"], 0), ""),
    ("out_ch",     "Out Ch",      "int",    (1, 16, 1), ""),
]

TEXTS = ["title", "folder_name", "row0", "row1", "row2", "row3", "row4", "row5", "row6", "row7",
         "info0", "info1", "info2", "info3", "message", "stats0", "stats1", "stats2", "stats3",
         "linn_status", "mpc_key", "gen_desc"]

# Display parameters: (id prefix, count, frames)
def display_groups():
    return [
        ("cell", G.SLOTS * G.NUM_ROWS, None),          # frames depend on the row class
        ("mark", G.SLOTS * G.NUM_ROWS, len(G.MARK_FRAMES)),   # colour dots, green/red heads, wrong-note ghosts
        ("overlay", G.SLOTS, len(G.OVERLAY_FRAMES)),
        ("mask", 2, len(G.STAFFMASK_FRAMES)),
        ("barline", G.SLOTS + 1, len(G.BARLINE_FRAMES)),
        ("keysig", 1, len(G.KEYSIG_FRAMES)),
        ("timesig", 1, len(G.TIMESIG_FRAMES)),
        ("flash", 1, len(G.FLASH_FRAMES)),
        ("status", 1, len(G.STATUS_FRAMES)),
        ("handicon", 1, len(G.HAND_FRAMES)),
        ("d_bar", 3, len(G.DIGIT_FRAMES)),
        ("d_bars", 3, len(G.DIGIT_FRAMES)),
        ("d_acc", 3, len(G.DIGIT_FRAMES)),
        ("d_bpm", 3, len(G.DIGIT_FRAMES)),
        ("d_err", 3, len(G.DIGIT_FRAMES)),
        ("nextname", 1, len(G.notename_frames())),
        ("playedname", 1, len(G.notename_frames())),
        ("finger", 2, len(G.FINGER_FRAMES)),
        ("mnum", 6, len(G.DIGIT_FRAMES)),              # two measure numbers, three digits each
        ("progress", 1, 0),                            # ColourBar slider, continuous
        ("linnstate", 1, len(G.LINNSTATE_FRAMES)),
        ("statbar", 16, G.BAR_FRAMES),
        ("d_best", 3, len(G.DIGIT_FRAMES)),
        ("d_last", 3, len(G.DIGIT_FRAMES)),
        ("d_passes", 3, len(G.DIGIT_FRAMES)),
        ("d_minutes", 4, len(G.DIGIT_FRAMES)),
        ("d_streak", 3, len(G.DIGIT_FRAMES)),
        ("nextname_lh", 1, len(G.notename_frames())),  # 0.2.0: the left hand's next note
        ("swatch", 12, G.SWATCH_FRAMES),               # 0.2.0: COLOR page swatches (mirror col_*)
        ("ro", len(readout_controls()), None),         # 0.4.0: one per control with a readout per value
    ]

def readout_controls():
    """Controls whose readout changes with the value, in parameter order: (id, number of values)."""
    out = []
    for (pid, name, kind, rng, unit) in CONTROL:
        if isinstance(RO.READOUTS.get(pid), list) or isinstance(RO.SHORT.get(pid), list):
            n = len(rng[0]) if kind == "choice" else (2 if kind in ("bool", "trigger") else int(rng[1] - rng[0] + 1))
            for table in (RO.READOUTS, RO.SHORT):
                if isinstance(table.get(pid), list) and len(table[pid]) != n:
                    raise ValueError("readout %s: %d texts for %d values" % (pid, len(table[pid]), n))
            out.append((pid, n))
    return out

def readout_index(pid):
    return [p for p, _ in readout_controls()].index(pid)

def build_index():
    """Returns {name: index} for every parameter, plus group bases."""
    idx = {}
    i = 0
    for p in CONTROL:
        idx[p[0]] = i; i += 1
    idx["_text_base"] = i
    for t in TEXTS:
        idx["text_" + t] = i; i += 1
    for (gid, count, frames) in display_groups():
        idx["_" + gid] = i
        i += count
    idx["_total"] = i
    return idx

INDEX = build_index()

def write_cpp_header(path):
    L = []
    w = L.append
    w("// GENERATED by tools/skin/params.py - do not edit. The parameter table; order is permanent.")
    w("#pragma once")
    w("namespace sr { namespace gen {")
    w("enum ParamKind { kFloat = 0, kInt = 1, kBool = 2, kChoice = 3, kTrigger = 4, kNote = 5 };")
    w("struct ControlDef { const char* id; const char* name; int kind; float min, max, def; const char* unit; const char* choices; };")
    w("constexpr ControlDef kControls[] = {")
    for (pid, name, kind, rng, unit) in CONTROL:
        k = {"float": 0, "int": 1, "bool": 2, "choice": 3, "trigger": 4, NOTE: 5}[kind]
        if kind == "choice":
            choices, d = rng
            w('    {"%s", "%s", %d, 0, %d, %d, "%s", "%s"},' % (pid, name, k, len(choices) - 1, d, unit, "|".join(choices)))
        else:
            mn, mx, d = rng
            w('    {"%s", "%s", %d, %s, %s, %s, "%s", ""},' % (pid, name, k, float(mn), float(mx), float(d), unit))
    w("};")
    w("constexpr int kNumControls = %d;" % len(CONTROL))
    w("enum ControlId {")
    for i, p in enumerate(CONTROL):
        w("    P_%s = %d," % (p[0].upper(), i))
    w("};")
    w("constexpr int kTextBase = %d, kNumTexts = %d;" % (INDEX["_text_base"], len(TEXTS)))
    w("enum TextId {")
    for i, t in enumerate(TEXTS):
        w("    T_%s = %d," % (t.upper(), i))
    w("};")
    for (gid, count, frames) in display_groups():
        w("constexpr int kD_%s = %d, kN_%s = %d;" % (gid, INDEX["_" + gid], gid, count) +
          ("  // %d frames" % frames if frames else ""))
        if frames:
            w("constexpr int kF_%s = %d;" % (gid, frames))
    ro = readout_controls()
    w("// readouts: display parameter kD_ro + i mirrors control kRoControl[i] (value - min), kRoFrames[i] frames")
    w("constexpr int kRoControl[%d] = {%s};" % (len(ro), ", ".join("P_%s" % p.upper() for p, _ in ro)))
    w("constexpr int kRoFrames[%d] = {%s};" % (len(ro), ", ".join(str(n) for _, n in ro)))
    w("constexpr int kDisplayBase = %d;" % INDEX["_" + display_groups()[0][0]])
    w("constexpr int kNumParams = %d;" % INDEX["_total"])
    w("}} // namespace sr::gen")
    with open(path, "w") as f:
        f.write("\n".join(L) + "\n")

if __name__ == "__main__":
    print("controls", len(CONTROL), "texts", len(TEXTS), "total params", INDEX["_total"])
