#!/usr/bin/env python3
"""Writes the drills that ship inside the plugin (plugin/Content/Drills/<folder>/*.mid).

    python3 tools/make_drills.py [out_dir]

Every folder gets an index.tsv (file, title, composer, level, source) that the plugin's LOAD
page reads. Drills are deterministic (fixed random seeds), so they are the same on every build.
Scales and arpeggios come one hand at a time (Scales, Scales LH, ...); everything for both hands at
once is in Two Hands. With Hands = Both, a file with two hands waits for both.
"""
import os, random, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from midi_write import write_midi   # noqa: E402

OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(HERE), "plugin", "Content", "Drills")
Q = 96                      # ticks per quarter
NAMES = ["C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"]
MAJOR_SF = {0: 0, 7: 1, 2: 2, 9: 3, 4: 4, 11: 5, 6: 6, 1: -5, 8: -4, 3: -3, 10: -2, 5: -1}
MINOR_SF = {9: 0, 4: 1, 11: 2, 6: 3, 1: 4, 8: 5, 3: -6, 10: -5, 5: -4, 0: -3, 7: -2, 2: -1}
MODES = {"Major": [0, 2, 4, 5, 7, 9, 11], "Natural Minor": [0, 2, 3, 5, 7, 8, 10], "Harmonic Minor": [0, 2, 3, 5, 7, 8, 11],
         "Dorian": [0, 2, 3, 5, 7, 9, 10], "Phrygian": [0, 1, 3, 5, 7, 8, 10], "Lydian": [0, 2, 4, 6, 7, 9, 11],
         "Mixolydian": [0, 2, 4, 5, 7, 9, 10], "Locrian": [0, 1, 3, 5, 6, 8, 10],
         "Major Pentatonic": [0, 2, 4, 7, 9], "Minor Pentatonic": [0, 3, 5, 7, 10], "Blues": [0, 3, 5, 6, 7, 10]}
MODE_SF_SHIFT = {"Major": 0, "Dorian": -2, "Phrygian": -4, "Lydian": 1, "Mixolydian": -1, "Natural Minor": -3, "Locrian": -5}

index = {}

def key_sf(root, mode):
    if mode in ("Natural Minor", "Harmonic Minor", "Minor Pentatonic", "Blues"):
        return MINOR_SF[root], True
    sf = MAJOR_SF[root] + MODE_SF_SHIFT.get(mode, 0)
    while sf > 6: sf -= 12
    while sf < -6: sf += 12
    return sf, False

def save(folder, fname, title, level, tracks, bpm=80, ts=(4, 4), key=0, minor=False):
    d = os.path.join(OUT, folder)
    os.makedirs(d, exist_ok=True)
    write_midi(os.path.join(d, fname), tracks, Q, bpm, ts, key, minor, title)
    index.setdefault(folder, []).append((fname, title, "Drill", level))

def scale_pitches(tonic, mode, octaves=1):
    iv = MODES[mode]
    up = [tonic + 12 * o + i for o in range(octaves) for i in iv] + [tonic + 12 * octaves]
    return up + up[-2::-1]

def seq(pitches, dur, start=0, vel=84):
    t, out = start, []
    for p in pitches:
        if p is not None:
            out.append((t, t + dur, p, vel))
        t += dur
    return out

def rh_tonic(root):
    return 60 + root if root <= 7 else 48 + root

def lh_tonic(t):
    """Two octaves below the right hand, or one where two would go below C2: the left hand then
    stays on the bass staff and its two ledger lines (0.5.0; before, A1 and the like came out)."""
    return t - 24 if t - 24 >= 36 else t - 12

# ---------------------------------------------------------------------------------------------
FIFTHS = [0, 7, 2, 9, 4, 11, 6, 1, 8, 3, 10, 5]

def scales():
    """One hand at a time (right in Scales, left in Scales LH); hands together are in Two Hands.
    Files are numbered so the list runs round the circle of fifths, majors first."""
    n = 0
    for mode in ("Major", "Natural Minor", "Harmonic Minor"):
        for root in FIFTHS:
            n += 1
            t = rh_tonic(root)
            sf, mi = key_sf(root, mode)
            lvl = 1 if abs(sf) <= 1 else (2 if abs(sf) <= 3 else 3)
            base = "%02d_%s_%s" % (n, NAMES[root], mode.replace(" ", ""))
            title = "%s %s scale" % (NAMES[root], mode.lower())
            save("Scales", base + ".mid", title, lvl, [("Right", 0, seq(scale_pitches(t, mode), Q))], 72, key=sf, minor=mi)
            save("Scales LH", base + "_LH.mid", title + ", left hand", lvl, [("Left", 1, seq(scale_pitches(lh_tonic(t), mode), Q, vel=70))], 72, key=sf, minor=mi)
            if mode == "Major":
                save("Two Hands", "scale_%s.mid" % base, title + ", hands together", lvl + 1,
                     [("Right", 0, seq(scale_pitches(t, mode), Q)), ("Left", 1, seq(scale_pitches(lh_tonic(t), mode), Q, vel=70))], 72, key=sf, minor=mi)
    for mode in ("Dorian", "Phrygian", "Lydian", "Mixolydian", "Locrian", "Major Pentatonic", "Minor Pentatonic", "Blues"):
        for root in (0, 7):
            t = rh_tonic(root)
            sf, mi = key_sf(root, mode) if mode in MODE_SF_SHIFT or mode in ("Minor Pentatonic", "Blues") else (MAJOR_SF[root], False)
            save("Modes", "%s_%s.mid" % (NAMES[root], mode.replace(" ", "")), "%s %s" % (NAMES[root], mode.lower()), 3,
                 [("Right", 0, seq(scale_pitches(t, mode, 2 if len(MODES[mode]) < 7 else 1), Q // 2 if len(MODES[mode]) < 7 else Q))], 72, key=sf, minor=mi)

def arpeggios():
    n = 0
    for minor in (False, True):
        for root in FIFTHS:
            n += 1
            t = rh_tonic(root)
            third = 3 if minor else 4
            up = [t, t + third, t + 7, t + 12, t + 12 + third, t + 19, t + 24]
            pat = up + up[-2::-1]
            sf, mi = (MINOR_SF[root], True) if minor else (MAJOR_SF[root], False)
            lvl = 2 if abs(sf) < 3 else 3
            base = "%02d_%s_%s_arpeggio" % (n, NAMES[root], "minor" if minor else "major")
            title = "%s %s arpeggio, 2 octaves" % (NAMES[root], "minor" if minor else "major")
            save("Arpeggios", base + ".mid", title, lvl, [("Right", 0, seq(pat, Q // 2))], 76, key=sf, minor=mi)
            save("Arpeggios LH", base + "_LH.mid", title + ", left hand", lvl, [("Left", 1, seq([p - t + lh_tonic(t) for p in pat], Q // 2, vel=70))], 76, key=sf, minor=mi)
            if not minor and root in (0, 7, 2, 5):
                save("Two Hands", "arpeggio_%s.mid" % base, title + ", hands together", lvl + 1,
                     [("Right", 0, seq(pat, Q // 2)), ("Left", 1, seq([p - t + lh_tonic(t) for p in pat], Q // 2, vel=70))], 76, key=sf, minor=mi)

def intervals():
    rnd = random.Random(7)
    cmaj = [p for p in range(55, 84) if p % 12 in (0, 2, 4, 5, 7, 9, 11)]
    for size, name in ((1, "seconds"), (2, "thirds"), (3, "fourths"), (4, "fifths"), (5, "sixths"), (6, "sevenths"), (7, "octaves")):
        notes, i = [], len(cmaj) // 2
        for k in range(32):
            if k % 2 == 0:
                i = rnd.randrange(0, len(cmaj) - size)
                notes.append(cmaj[i])
            else:
                j = i + size if rnd.random() < 0.6 else i - size
                if j < 0: j = i + size
                notes.append(cmaj[j])
        save("Intervals", "intervals_%d_%s.mid" % (size + 1, name), "Reading %s (C major)" % name, min(5, 1 + size // 2),
             [("Right", 0, seq(notes, Q))], 70)
        lh = [p - 24 for p in notes]
        save("Intervals", "intervals_%d_%s_LH.mid" % (size + 1, name), "Reading %s, bass clef" % name, min(5, 2 + size // 2),
             [("Left", 1, seq(lh, Q))], 70)

def patterns():
    for root in (0, 7, 5, 2):
        t = rh_tonic(root); sf = MAJOR_SF[root]
        sc = [t + i for i in MODES["Major"]] + [t + 12 + i for i in MODES["Major"]] + [t + 24]
        # 1-2-3-4, 2-3-4-5 ... up then down
        p = []
        for s in range(0, 8):
            p += sc[s:s + 4]
        for s in range(8, 0, -1):
            p += [sc[s + 3], sc[s + 2], sc[s + 1], sc[s]]
        save("Patterns", "%s_fours.mid" % NAMES[root], "%s major: groups of four" % NAMES[root], 2, [("Right", 0, seq(p, Q // 2))], 80, key=sf)
        thirds = []
        for s in range(0, 8):
            thirds += [sc[s], sc[s + 2]]
        thirds += thirds[-2::-1][:15]
        save("Patterns", "%s_broken_thirds.mid" % NAMES[root], "%s major: broken thirds" % NAMES[root], 2, [("Right", 0, seq(thirds, Q // 2))], 80, key=sf)
        triads = []
        for s in range(0, 8):
            triads += [sc[s], sc[s + 2], sc[s + 4]]
        save("Patterns", "%s_broken_triads.mid" % NAMES[root], "%s major: broken triads" % NAMES[root], 3, [("Right", 0, seq(triads, Q // 2))], 80, ts=(3, 4), key=sf)
        # Alberti bass under a simple melody
        b = t - 24
        chords = [(b, b + 4, b + 7), (b - 7 + 12, b + 9 - 12 + 12, b + 12), (b - 5, b + 2, b + 5), (b, b + 4, b + 7)]
        lh = []
        mel = []
        rnd = random.Random(root)
        for bar in range(8):
            c = chords[bar % 4]
            for k, p in enumerate([c[0], c[2], c[1], c[2]] * 2):
                lh.append((bar * 4 * Q + k * Q // 2, bar * 4 * Q + (k + 1) * Q // 2, p, 64))
            for k in range(4 if bar % 4 != 3 else 1):
                dur = Q if bar % 4 != 3 else 4 * Q
                p = rnd.choice([x for x in sc[:9] if (x - c[0]) % 12 in (0, 4, 7, 3)] or sc[:5])
                mel.append((bar * 4 * Q + k * Q, bar * 4 * Q + k * Q + dur, p, 84))
        save("Two Hands", "%s_alberti.mid" % NAMES[root], "%s major: melody over Alberti bass" % NAMES[root], 3,
             [("Right", 0, mel), ("Left", 1, lh)], 72, key=sf)
    for root, pos in ((0, "C"), (7, "G"), (5, "F"), (2, "D")):
        t = rh_tonic(root); sf = MAJOR_SF[root]
        five = [t + i for i in MODES["Major"][:5]]
        rnd = random.Random(40 + root)
        mel = [five[0]]
        for _ in range(31):
            mel.append(five[max(0, min(4, five.index(mel[-1]) + rnd.choice([-2, -1, -1, 1, 1, 2, 0])))])
        mel[-1] = five[0]
        save("Positions", "%s_position_RH.mid" % pos, "%s position, right hand (5 notes)" % pos, 1, [("Right", 0, seq(mel, Q))], 70, key=sf)
        save("Positions", "%s_position_LH.mid" % pos, "%s position, left hand (5 notes)" % pos, 1, [("Left", 1, seq([p - 12 for p in mel], Q))], 70, key=sf)

def rhythm():
    # durations in 16ths; negative = rest. One pitch so only the rhythm is read.
    bars = {
        1: [[16], [8, 8], [8, 4, 4], [4, 4, 8], [4, 4, 4, 4], [12, 4], [4, 12], [-4, 4, 8]],
        2: [[2, 2, 4, 4, 4], [4, 2, 2, 4, 4], [2, 2, 2, 2, 8], [4, 4, 2, 2, 2, 2], [-4, 4, 2, 2, 4]],
        3: [[6, 2, 4, 4], [6, 2, 6, 2], [4, 6, 2, 4], [12, 2, 2], [-2, 2, 4, -2, 2, 4]],
        4: [[2, 4, 2, 8], [2, 4, 2, 2, 4, 2], [4, 8, 4], [-2, 4, 2, 4, 4], [2, 6, 2, 6]],
        5: [[1, 1, 2, 4, 4, 4], [3, 1, 3, 1, 4, 4], [1, 1, 1, 1, 2, 2, 4, 4], [2, 1, 1, 2, 2, 8], [3, 1, 2, 2, 8]],
    }
    for lvl, pats in bars.items():
        for hand, pitch, nm in (("Right", 71, ""), ("Left", 50, "_LH")):
            rnd = random.Random(lvl * 10 + (1 if nm else 0))
            notes, t = [], 0
            for b in range(16):
                pat = rnd.choice(pats) if b % 4 != 3 else [16]
                for d in pat:
                    if d > 0:
                        notes.append((t, t + d * Q // 4, pitch, 84))
                    t += abs(d) * Q // 4
            save("Rhythm", "rhythm_level%d%s.mid" % (lvl, nm), "Rhythm level %d%s" % (lvl, " (bass clef)" if nm else ""), lvl,
                 [(hand, 0 if hand == "Right" else 1, notes)], 60 + 8 * lvl)
    # 3/4 and 6/8
    for ts, pats, name in (((3, 4), [[12], [8, 4], [4, 4, 4], [6, 2, 4], [4, 8], [2, 2, 4, 4]], "waltz time 3/4"),
                           ((6, 8), [[12], [6, 6], [4, 2, 6], [2, 2, 2, 6], [4, 2, 4, 2], [3, 1, 2, 6]], "compound time 6/8")):
        rnd = random.Random(ts[0])
        notes, t = [], 0
        for b in range(16):
            for d in (rnd.choice(pats) if b % 4 != 3 else [12]):
                notes.append((t, t + d * Q // 4, 71, 84)); t += d * Q // 4
        save("Rhythm", "rhythm_%d_%d.mid" % ts, "Rhythm in %s" % name, 2, [("Right", 0, notes)], 72, ts=ts)
    # triplets
    notes, t = [], 0
    rnd = random.Random(99)
    for b in range(16):
        for beat in range(4):
            if b % 4 == 3 and beat == 0:
                notes.append((t, t + 4 * Q, 71, 84)); t += 4 * Q; break
            if rnd.random() < 0.5:
                for k in range(3):
                    notes.append((t + k * Q // 3, t + (k + 1) * Q // 3, 71, 84))
            else:
                notes.append((t, t + Q, 71, 84))
            t += Q
    save("Rhythm", "rhythm_triplets.mid", "Rhythm: eighth-note triplets", 4, [("Right", 0, notes)], 66)

def ledger():
    def walk(lo, hi, seed, n=32):
        rnd = random.Random(seed)
        pool = [p for p in range(lo, hi + 1) if p % 12 in (0, 2, 4, 5, 7, 9, 11)]
        i = rnd.randrange(len(pool)); out = []
        for _ in range(n):
            out.append(pool[i]); i = max(0, min(len(pool) - 1, i + rnd.choice([-2, -1, 1, 2, -3, 3])))
        return out
    save("Ledger Lines", "treble_high.mid", "Treble ledger lines above (F5 - E6)", 3, [("Right", 0, seq(walk(77, 88, 1), Q))], 66)
    save("Ledger Lines", "treble_low.mid", "Treble ledger lines below (F3 - E4)", 3, [("Right", 0, seq(walk(53, 64, 2), Q))], 66)
    save("Ledger Lines", "bass_high.mid", "Bass ledger lines above (A3 - G4)", 3, [("Left", 1, seq(walk(57, 67, 3), Q))], 66)
    save("Ledger Lines", "bass_low.mid", "Bass ledger lines below (C2 - G2)", 3, [("Left", 1, seq(walk(36, 43, 4), Q))], 66)
    save("Ledger Lines", "middle_c_both.mid", "Around middle C, both hands", 2,
         [("Right", 0, seq(walk(60, 67, 5, 16), 2 * Q)), ("Left", 1, seq(walk(53, 60, 6, 16), 2 * Q))], 66)

def two_hands():
    for root in (0, 7, 2, 5):
        t = rh_tonic(root); sf = MAJOR_SF[root]
        up = scale_pitches(t, "Major")
        lh = []
        # contrary motion: LH starts an octave below and moves the other way
        down = [t - 12 - (p - t) for p in up]
        lh = seq([max(36, p) for p in down], Q, vel=70)
        save("Two Hands", "%s_contrary.mid" % NAMES[root], "%s major contrary motion" % NAMES[root], 3, [("Right", 0, seq(up, Q)), ("Left", 1, lh)], 72, key=sf)
    rnd = random.Random(11)
    mel = []
    for b in range(8):
        for k in range(4):
            mel.append(rnd.choice([60, 62, 64, 65, 67, 69, 71, 72]))
    save("Two Hands", "octaves_C.mid", "Parallel octaves in C", 2, [("Right", 0, seq(mel, Q)), ("Left", 1, seq([p - 12 for p in mel], Q, vel=70))], 70)
    # RH quarters against LH halves and wholes
    lh = []
    for b in range(8):
        root = [48, 53, 55, 48][b % 4]
        if b % 2: lh += [(b * 4 * Q, b * 4 * Q + 2 * Q, root, 70), (b * 4 * Q + 2 * Q, (b + 1) * 4 * Q, root + 7, 70)]
        else: lh += [(b * 4 * Q, (b + 1) * 4 * Q, root, 70)]
    save("Two Hands", "independence_C.mid", "Hand independence: quarters over halves", 2, [("Right", 0, seq(mel, Q)), ("Left", 1, lh)], 70)

def linnstrument():
    """Drills for the 4ths layout: shapes that repeat a row up (+5 semitones)."""
    def rows_up(shape, start, times):
        out = []
        for r in range(times):
            out += [start + 5 * r + s for s in shape]
        return out
    spider = rows_up([0, 1, 2, 3], 45, 6)                       # one finger per column, row by row
    save("LinnStrument", "spider_chromatic.mid", "Chromatic spider: four columns, row by row", 2,
         [("Right", 0, seq(spider + spider[::-1], Q // 2))], 80)
    ladder = rows_up([0, 2, 4, 5], 48, 6)
    save("LinnStrument", "fourths_ladder.mid", "Same shape, one row up each time (4ths)", 2, [("Right", 0, seq(ladder + ladder[::-1], Q // 2))], 80)
    octv = []
    for p in [48, 50, 52, 53, 55, 57, 59, 60]:
        octv += [p, p + 12]
    save("LinnStrument", "octave_shape.mid", "Octave shape: two rows up, two columns left", 2, [("Right", 0, seq(octv + octv[::-1], Q // 2))], 80)
    for name, iv in (("minor_thirds", 3), ("major_thirds", 4), ("fifths", 7)):
        pat = []
        for p in [48, 50, 52, 53, 55, 57, 59, 60, 62]:
            pat += [p, p + iv]
        save("LinnStrument", "shape_%s.mid" % name, "Interval shape: %s" % name.replace("_", " "), 2, [("Right", 0, seq(pat, Q // 2))], 80)
    for i, start in enumerate((60, 64, 67)):
        sc = [p for p in range(start, start + 13) if p % 12 in (0, 2, 4, 5, 7, 9, 11)]
        save("LinnStrument", "C_major_box_%d.mid" % (i + 1), "C major box %d (from %s)" % (i + 1, ["C", "E", "G"][i]), 2,
             [("Right", 0, seq(sc + sc[-2::-1], Q // 2))], 80)
    pent = [57, 60, 62, 64, 67, 69, 72, 74, 76, 79, 81]
    save("LinnStrument", "A_minor_pentatonic_box.mid", "A minor pentatonic across the rows", 2, [("Right", 0, seq(pent + pent[-2::-1], Q // 2))], 84, key=0, minor=True)

def write_indexes():
    for folder, rows in index.items():
        with open(os.path.join(OUT, folder, "index.tsv"), "w") as f:
            f.write("# file\ttitle\tcomposer\tlevel\tsource\n")
            for fname, title, comp, lvl in rows:
                f.write("%s\t%s\t%s\t%d\t%s\n" % (fname, title, comp, lvl, "Sight Reader drill (generated, public domain)"))

if __name__ == "__main__":
    scales(); arpeggios(); intervals(); patterns(); rhythm(); ledger(); two_hands(); linnstrument()
    write_indexes()
    total = sum(len(v) for v in index.values())
    print("%d drills in %d folders -> %s" % (total, len(index), OUT))
