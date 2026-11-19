"""Screen geometry and the cell frame catalogue for the notation display.

This module is the single source of truth shared by:
  * tools/skin/render.py  - draws the film strips (PNG) frame by frame in this order;
  * tools/gen_skin.py     - places the widgets (TUI.json) at these coordinates;
  * plugin/src/sr/gen/layout_gen.h - the same numbers and frame tables for the C++ layout
    engine (written by write_cpp_header()).

How the staff is drawn
----------------------
The MPC draws a plugin's screen from JSON; a plugin cannot paint. A grand staff is therefore
a grid of small film-strip images ("cells"): one per (slot, staff row). Each cell shows one of
~190 pre-rendered frames (empty, a notehead with accidental/dot/stem piece, a stem segment,
a flag, a beam piece, a tie piece, a rest, a ledger line). The plugin picks the frame by
writing the cell's parameter. Rows are one staff position (a line or a space) apart, so a
column of cells can show any chord; stems are drawn piecewise through the cells they cross.

Rows: treble positions 12..-7 then bass positions 8..-4. Treble -1..-7 and bass 15..9 are the
same rows (the gap between the staves holds the ledger lines of both), which is exactly how a
grand staff with a four-space gap is engraved.
"""

# ---------------------------------------------------------------------------------------------
# Screen layout (tab area is 1280 x 628; y = 0 is 110 px down the physical screen)
# ---------------------------------------------------------------------------------------------
TAB_W, TAB_H = 1280, 628

S = 16            # staff space in px
P = S // 2        # one staff position (line to space) in px

SLOTS = 20        # note columns on screen
SLOT_W = 54       # px per slot (also the cell frame width, so beams tile)
SLOTS_X0 = 184    # x of slot 0 (clef, key and time signature live to the left)

TREBLE_TOP_POS = 12          # highest treble position with its own row (C6, 2nd ledger line)
TREBLE_LOW_POS = -7          # lowest treble position (E3); rows -1..-7 are shared with bass 15..9
BASS_TOP_POS = 8             # first bass-only row (A3, top line of the bass staff)
BASS_LOW_POS = -4            # lowest bass position (C2, 2nd ledger line below)

ROW0_Y = 96                  # y (tab coords) of the centre of row 0 (treble position 12)

CELL_H = 60                  # cell frame height (row centre +- 30 px)

def treble_rows():
    return list(range(TREBLE_TOP_POS, TREBLE_LOW_POS - 1, -1))   # 12..-7

def bass_only_rows():
    return list(range(BASS_TOP_POS, BASS_LOW_POS - 1, -1))       # 8..-4

NUM_TREBLE_ROWS = len(treble_rows())          # 20
NUM_ROWS = NUM_TREBLE_ROWS + len(bass_only_rows())   # 33

def row_of(staff, pos):
    """Row index for a staff position, or None when off the grid. staff 0 = treble, 1 = bass."""
    if staff == 0:
        if TREBLE_LOW_POS <= pos <= TREBLE_TOP_POS:
            return TREBLE_TOP_POS - pos
        return None
    # bass
    if BASS_LOW_POS <= pos <= BASS_TOP_POS:
        return NUM_TREBLE_ROWS + (BASS_TOP_POS - pos)
    if BASS_TOP_POS < pos <= BASS_TOP_POS + 7:          # 9..15 share treble -7..-1
        return row_of(0, pos - 16)
    return None

def row_y(row):
    return ROW0_Y + row * P

# Row classes decide which film strip a cell uses.
SPACE, LINE, LEDGER = 0, 1, 2
CLASS_NAMES = ["space", "line", "ledger"]

def row_class(row):
    """Staff lines are positions 0,2,4,6,8 of each staff; other even positions are ledger lines."""
    if row < NUM_TREBLE_ROWS:
        pos = TREBLE_TOP_POS - row
        if pos % 2:
            return SPACE
        return LINE if 0 <= pos <= 8 else LEDGER
    pos = BASS_TOP_POS - (row - NUM_TREBLE_ROWS)
    if pos % 2:
        return SPACE
    return LINE if 0 <= pos <= 8 else LEDGER

TREBLE_LINES_Y = [row_y(row_of(0, p)) for p in (8, 6, 4, 2, 0)]
BASS_LINES_Y = [row_y(row_of(1, p)) for p in (8, 6, 4, 2, 0)]

STAFF_TOP_Y = TREBLE_LINES_Y[0]
STAFF_BOTTOM_Y = BASS_LINES_Y[-1]

# Areas
HEADER_H = 62                                   # title / counters strip above the staff
GRID_TOP = row_y(0) - CELL_H // 2               # 40
GRID_BOTTOM = row_y(NUM_ROWS - 1) + CELL_H // 2 # 356 + 30
FOOTER_Y = 392                                  # controls below the staff

# Cell frame layout (x within a 54 px slot)
ACC_X1 = 16          # accidentals end here
HEAD_X0 = 17         # main notehead left edge
HEAD_W = 19          # SMuFL black notehead at S = 16
STEM_UP_X = HEAD_X0 + HEAD_W - 1     # right edge of the main head
STEM_DOWN_X = HEAD_X0                # left edge of the main head
DISP_X0 = STEM_UP_X                  # displaced (second-interval) head starts at the up-stem
STEM_W = 2
STEM_LEN = 7                          # positions (3.5 spaces)

# ---------------------------------------------------------------------------------------------
# Cell frame catalogue
# ---------------------------------------------------------------------------------------------
# A frame is described by a tuple (kind, a, b, c, d, e, f) packed into one integer key so that
# the C++ side can look frames up. Field meaning depends on kind:
#   EMPTY:   -
#   HEAD:    a=head (0 black, 1 half, 2 whole), b=place (0 main, 1 displaced), c=acc (0 none,
#            1 sharp, 2 flat, 3 natural), d=dot (0/1), e=stem (0 none, 1 up-start, 2 down-start,
#            3 through-up, 4 through-down)
#   STEM:    e=stem (3 through-up, 4 through-down, 5 up-end, 6 down-end), a=flag count (only with
#            through stems: the flag hangs from the end, drawn 3 rows inward), b=beams left (0..2),
#            c=beams right (0..2) (only with *-end), f=ledger (0/1, ledger strip only)
#   TIE:     a=piece (1 start, 2 end, 3 through), b=side (0 below the note, 1 above)
#   REST:    a=type (0 whole .. 5 32nd), d=dot
#   LEDGER:  - (a ledger line alone)
#   TUPLET:  b=side (0 below, 1 above) - a small "3"
EMPTY, HEAD, STEM, TIE, REST, LEDGERONLY, TUPLET = range(7)
KIND_NAMES = ["empty", "head", "stem", "tie", "rest", "ledger", "tuplet"]

STEM_NONE, STEM_UP_START, STEM_DOWN_START, STEM_THRU_UP, STEM_THRU_DOWN, STEM_UP_END, STEM_DOWN_END = range(7)

def key(kind, a=0, b=0, c=0, d=0, e=0, f=0):
    return (kind << 24) | (a << 20) | (b << 16) | (c << 12) | (d << 8) | (e << 4) | f

def unkey(k):
    return ((k >> 24) & 0xFF, (k >> 20) & 15, (k >> 16) & 15, (k >> 12) & 15, (k >> 8) & 15, (k >> 4) & 15, k & 15)

# Rests sit on fixed rows: whole rest hangs from the 4th line (pos 6), the rest centre on the
# middle line (pos 4). Rest frames exist only in the LINE strip.
REST_POS = [6, 4, 4, 4, 4, 4]

def catalogue(cls):
    """Ordered list of frame keys for a row class. Frame index = position in this list."""
    frames = [key(EMPTY)]
    if cls == LEDGER:
        frames.append(key(LEDGERONLY))
    # noteheads
    for head in (0, 1, 2):
        stems = (STEM_NONE,) if head == 2 else (STEM_UP_START, STEM_DOWN_START, STEM_THRU_UP, STEM_THRU_DOWN)
        for place in (0, 1):
            for acc in (0, 1, 2, 3):
                for dot in (0, 1):
                    for stem in stems:
                        frames.append(key(HEAD, head, place, acc, dot, stem, 1 if cls == LEDGER else 0))
    # stem pieces
    ledgers = (0, 1) if cls == LEDGER else (0,)
    for led in ledgers:
        for stem in (STEM_THRU_UP, STEM_THRU_DOWN):
            frames.append(key(STEM, 0, 0, 0, 0, stem, led))
    for stem in (STEM_THRU_UP, STEM_THRU_DOWN):
        for flags in (1, 2, 3):
            frames.append(key(STEM, flags, 0, 0, 0, stem, 0))
    for stem in (STEM_UP_END, STEM_DOWN_END):
        for bl in (0, 1, 2):
            for br in (0, 1, 2):
                frames.append(key(STEM, 0, bl, br, 0, stem, 0))
        # beam passing through a slot where this hand has no note (d = 1: no stem)
        for lv in (1, 2):
            frames.append(key(STEM, 0, lv, lv, 1, stem, 0))
    # tie pieces
    for side in (0, 1):
        for piece in (1, 2, 3):
            frames.append(key(TIE, piece, side))
    if cls != LEDGER:
        for side in (0, 1):
            frames.append(key(TUPLET, 0, side))
    if cls == LINE:
        for rest in range(6):
            for dot in (0, 1):
                frames.append(key(REST, rest, 0, 0, dot))
    return frames

CATALOGUES = {cls: catalogue(cls) for cls in (SPACE, LINE, LEDGER)}

# ---------------------------------------------------------------------------------------------
# Other strips
# ---------------------------------------------------------------------------------------------
# Slot overlay frames (drawn over the cells of one slot, both staves): what state the slot is in.
OVERLAY_FRAMES = ["none", "current", "played", "missed", "hidden", "next"]
# Staff masks (one per staff): dim a whole staff (the hand not being practised).
STAFFMASK_FRAMES = ["none", "dim", "hidden"]
BARLINE_FRAMES = ["none", "single", "double", "final", "repeat_start", "repeat_end"]
KEYSIG_FRAMES = list(range(-7, 8))              # sharps positive
TIMESIG_FRAMES = ["4/4", "3/4", "2/4", "2/2", "C", "C|", "3/8", "6/8", "9/8", "12/8", "5/4", "6/4", "7/8", "5/8", "?"]
DIGIT_FRAMES = [" ", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9"]   # frame 0 = blank
STATUS_FRAMES = ["", "WAIT", "FLOW", "RHYTHM", "PAUSED", "COUNT-IN", "DONE", "LOADING", "NO FILE", "LOOP", "LISTEN"]
HAND_FRAMES = ["BOTH", "RIGHT", "LEFT"]
NOTE_NAMES = ["C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"]
# Note name strip: frame 0 blank, then MIDI 21 (A0) .. 108 (C8) spelled with sharps,
# then the same range spelled with flats.
NOTENAME_LOW, NOTENAME_HIGH = 21, 108
def notename_frames():
    sharp = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
    flat = ["C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"]
    out = [""]
    for names in (sharp, flat):
        for n in range(NOTENAME_LOW, NOTENAME_HIGH + 1):
            out.append("%s%d" % (names[n % 12], n // 12 - 1))
    return out
FINGER_FRAMES = ["", "1", "2", "3", "4", "5"]
BAR_FRAMES = 21           # accuracy bars on STATS: 0..100 % in steps of 5
FLASH_FRAMES = ["none", "wrong", "good"]
LINNSTATE_FRAMES = ["", "LINN: NO PORT", "LINN: CONNECTED", "LINN: OFF"]

# ---------------------------------------------------------------------------------------------
# Note colours and marks (0.2.0)
# ---------------------------------------------------------------------------------------------
# The palette: the LinnStrument's own colours (firmware colour numbers 1-11 without "off"), as a
# screen shows them, plus five in-between hues for the black keys of the default scheme
# C cyan, D blue, E pink, F magenta, G white, A yellow, B orange. An in-between colour is the
# hue halfway round the colour circle between its neighbours (for white: the neighbour's hue at
# half saturation).
def _between(a, b):
    import colorsys
    ha, sa, va = colorsys.rgb_to_hsv(*[c / 255.0 for c in a])
    hb, sb, vb = colorsys.rgb_to_hsv(*[c / 255.0 for c in b])
    if sa < 0.05: ha = hb
    if sb < 0.05: hb = ha
    d = hb - ha
    if d > 0.5: d -= 1.0
    if d < -0.5: d += 1.0
    h = (ha + d / 2.0) % 1.0
    r, g, b2 = colorsys.hsv_to_rgb(h, (sa + sb) / 2.0, (va + vb) / 2.0)
    return (int(round(r * 255)), int(round(g * 255)), int(round(b2 * 255)))

_LS = {
    "Red": (240, 36, 36), "Yellow": (255, 226, 0), "Green": (30, 206, 64), "Cyan": (0, 218, 236),
    "Blue": (28, 72, 255), "Magenta": (226, 30, 226), "White": (255, 255, 255), "Orange": (255, 136, 0),
    "Lime": (172, 240, 20), "Pink": (255, 112, 186),
}
PALETTE = [("Off", None)] + [(n, _LS[n]) for n in ("Red", "Yellow", "Green", "Cyan", "Blue", "Magenta", "White", "Orange", "Lime", "Pink")] + [
    ("Azure", _between(_LS["Cyan"], _LS["Blue"])),         # C#: cyan - blue
    ("Violet", _between(_LS["Blue"], _LS["Pink"])),        # D#: blue - pink
    ("Orchid", _between(_LS["Magenta"], _LS["White"])),    # F#: magenta - white
    ("Cream", _between(_LS["White"], _LS["Yellow"])),      # G#: white - yellow
    ("Amber", _between(_LS["Yellow"], _LS["Orange"])),     # A#: yellow - orange
    # 0.4.0: D# halfway between blue and pink came out violet, too close to F's magenta. The
    # scheme now darkens instead: D# is a deeper blue than D. Appended, so saved choices keep
    # their meaning.
    ("Navy", (22, 30, 150)),
]
PALETTE_NAMES = [n for n, _ in PALETTE]
def _pal(n):
    return PALETTE_NAMES.index(n)
# (param id suffix, name shown, default palette index) for C .. B
NOTE_COLOUR_DEFAULTS = [
    ("c", "C", _pal("Cyan")), ("cs", "C#", _pal("Azure")), ("d", "D", _pal("Blue")), ("ds", "D#", _pal("Navy")),
    ("e", "E", _pal("Pink")), ("f", "F", _pal("Magenta")), ("fs", "F#", _pal("Orchid")), ("g", "G", _pal("White")),
    ("gs", "G#", _pal("Cream")), ("a", "A", _pal("Yellow")), ("as", "A#", _pal("Amber")), ("b", "B", _pal("Orange")),
]
SWATCH_FRAMES = len(PALETTE)

def luminance(rgb):
    """Relative luminance (sRGB), 0..1."""
    def lin(c):
        c /= 255.0
        return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4
    r, g, b = (lin(c) for c in rgb)
    return 0.2126 * r + 0.7152 * g + 0.0722 * b

SOUND_TYPES = ["Wood", "Soft Tick", "Low Thud", "Bell", "Sine", "Square"]
CLICK_TYPES = ["Tick", "Wood", "Beep"]

# Mark frames (one film strip for every row; same 54 x 60 rectangle as the cell under it).
#   dot:    the note's colour inside its head, sized so half and whole notes still look hollow;
#           only on notes not yet passed (passed notes fade with their column)
#   tint:   the head redrawn green (played right) or red (missed); "trail" variant, lighter, for
#           columns already passed (marks sit above the played-note fade)
#   ghost:  a red head where a wrong note was played, with its accidental, on a ledger line or not
#   gledger: a red ledger line alone (between the staff and a ghost)
MARK_EMPTY, MARK_DOT, MARK_TINT, MARK_GHOST, MARK_GLEDGER = range(5)
NUM_DOT_COLOURS = len(PALETTE) - 1
def _mark_frames():
    f = [(MARK_EMPTY, 0, 0, 0, 0)]
    for place in (0, 1):
        for head in (0, 1, 2):
            for c in range(1, len(PALETTE)):
                f.append((MARK_DOT, 0, place, head, c))
    for trail in (0, 1):
        for place in (0, 1):
            for head in (0, 1, 2):
                for tint in (1, 2):
                    f.append((MARK_TINT, trail, place, head, tint))
    for trail in (0, 1):
        for ledger in (0, 1):
            for acc in range(4):
                f.append((MARK_GHOST, trail, 0, ledger, acc))
    for trail in (0, 1):
        f.append((MARK_GLEDGER, trail, 0, 0, 0))
    return f
MARK_FRAMES = _mark_frames()
MARK_DOT_BASE = 1
MARK_TINT_BASE = MARK_DOT_BASE + 2 * 3 * NUM_DOT_COLOURS
MARK_GHOST_BASE = MARK_TINT_BASE + 2 * 2 * 3 * 2
MARK_GLEDGER_BASE = MARK_GHOST_BASE + 2 * 2 * 4
assert MARK_FRAMES[MARK_TINT_BASE][0] == MARK_TINT and MARK_FRAMES[MARK_GHOST_BASE][0] == MARK_GHOST
assert MARK_FRAMES[MARK_GLEDGER_BASE][0] == MARK_GLEDGER and len(MARK_FRAMES) == MARK_GLEDGER_BASE + 2

# ---------------------------------------------------------------------------------------------
# C++ header
# ---------------------------------------------------------------------------------------------
def write_cpp_header(path):
    L = []
    w = L.append
    w("// GENERATED by tools/skin/geometry.py - do not edit. Geometry and cell frame tables shared")
    w("// with the rendered film strips in Plugin Skins/.")
    w("#pragma once")
    w("#include <cstdint>")
    w("namespace sr { namespace gen {")
    w("constexpr int kSlots = %d;" % SLOTS)
    w("constexpr int kRows = %d;" % NUM_ROWS)
    w("constexpr int kTrebleRows = %d;" % NUM_TREBLE_ROWS)
    w("constexpr int kTrebleTopPos = %d, kTrebleLowPos = %d, kBassTopPos = %d, kBassLowPos = %d;" %
      (TREBLE_TOP_POS, TREBLE_LOW_POS, BASS_TOP_POS, BASS_LOW_POS))
    w("constexpr int kStemLen = %d;" % STEM_LEN)
    w("enum RowClass : uint8_t { kSpace = 0, kLine = 1, kLedger = 2 };")
    w("constexpr uint8_t kRowClass[kRows] = {%s};" % ", ".join(str(row_class(r)) for r in range(NUM_ROWS)))
    w("enum CellKind : uint8_t { kEmpty = 0, kHead = 1, kStem = 2, kTie = 3, kRest = 4, kLedgerOnly = 5, kTuplet = 6 };")
    w("enum StemPiece : uint8_t { kStemNone = 0, kUpStart = 1, kDownStart = 2, kThruUp = 3, kThruDown = 4, kUpEnd = 5, kDownEnd = 6 };")
    w("constexpr uint32_t cellKey(int kind, int a = 0, int b = 0, int c = 0, int d = 0, int e = 0, int f = 0)")
    w("{ return ((uint32_t)kind << 24) | ((uint32_t)a << 20) | ((uint32_t)b << 16) | ((uint32_t)c << 12) | ((uint32_t)d << 8) | ((uint32_t)e << 4) | (uint32_t)f; }")
    for cls in (SPACE, LINE, LEDGER):
        cat = CATALOGUES[cls]
        w("constexpr int kFrames_%s = %d;" % (CLASS_NAMES[cls], len(cat)))
        # sorted keys with their frame index, for binary search
        pairs = sorted((k, i) for i, k in enumerate(cat))
        w("constexpr uint32_t kKeys_%s[%d] = {%s};" % (CLASS_NAMES[cls], len(cat), ", ".join("0x%08x" % k for k, _ in pairs)))
        w("constexpr uint8_t kIdx_%s[%d] = {%s};" % (CLASS_NAMES[cls], len(cat), ", ".join(str(i) for _, i in pairs)))
    w("constexpr int kRestPos[6] = {%s};" % ", ".join(str(p) for p in REST_POS))
    for name, frames in (("Overlay", OVERLAY_FRAMES), ("StaffMask", STAFFMASK_FRAMES), ("Barline", BARLINE_FRAMES),
                         ("Status", STATUS_FRAMES), ("Hand", HAND_FRAMES), ("Flash", FLASH_FRAMES),
                         ("LinnState", LINNSTATE_FRAMES)):
        w("constexpr int k%sFrames = %d;" % (name, len(frames)))
    w("constexpr int kKeySigFrames = %d, kTimeSigFrames = %d, kDigitFrames = %d, kFingerFrames = %d, kBarFrames = %d;" %
      (len(KEYSIG_FRAMES), len(TIMESIG_FRAMES), len(DIGIT_FRAMES), len(FINGER_FRAMES), BAR_FRAMES))
    w("constexpr int kNoteNameFrames = %d, kNoteNameLow = %d, kNoteNameHigh = %d;" % (len(notename_frames()), NOTENAME_LOW, NOTENAME_HIGH))
    w('constexpr const char* kTimeSigNames[%d] = {%s};' % (len(TIMESIG_FRAMES), ", ".join('"%s"' % t for t in TIMESIG_FRAMES)))
    w("// Marks: dot = kMarkDotBase + (place*3 + head)*kDotColours + colour-1 (head 0 black, 1 half, 2 whole);")
    w("// tint = kMarkTintBase + ((trail*2 + place)*3 + head)*2 + tint-1 (1 green, 2 red);")
    w("// ghost = kMarkGhostBase + (trail*2 + ledger)*4 + acc (0 none, 1 sharp, 2 flat, 3 natural);")
    w("// ghost ledger line = kMarkGLedgerBase + trail.")
    w("constexpr int kMarkFrames = %d, kDotColours = %d;" % (len(MARK_FRAMES), NUM_DOT_COLOURS))
    w("constexpr int kMarkDotBase = %d, kMarkTintBase = %d, kMarkGhostBase = %d, kMarkGLedgerBase = %d;" %
      (MARK_DOT_BASE, MARK_TINT_BASE, MARK_GHOST_BASE, MARK_GLEDGER_BASE))
    w("constexpr int kSwatchFrames = %d;" % SWATCH_FRAMES)
    w("}} // namespace sr::gen")
    with open(path, "w") as f:
        f.write("\n".join(L) + "\n")

if __name__ == "__main__":
    for cls in (SPACE, LINE, LEDGER):
        print(CLASS_NAMES[cls], len(CATALOGUES[cls]), "frames ->", len(CATALOGUES[cls]) * CELL_H, "px tall")
    print("rows", NUM_ROWS, "cells", NUM_ROWS * SLOTS)
    print("treble lines", TREBLE_LINES_Y, "bass lines", BASS_LINES_Y)
    print("grid", GRID_TOP, GRID_BOTTOM)
