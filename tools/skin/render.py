"""Draws every image the skin uses: the notation cell strips, overlays, barlines, signatures,
digits, words, and the static staff, for one colour theme.

Music glyphs come from Bravura (SMuFL, SIL OFL 1.1); text from Titillium Web (the MPC's own
UI font, SIL OFL 1.1). Both are used only here, at build time: the plugin ships the rendered
PNGs, not the fonts.
"""
import os
from PIL import Image, ImageDraw, ImageFont

import geometry as G

HERE = os.path.dirname(os.path.abspath(__file__))
FONTS = os.path.join(HERE, "fonts")

def font(name, size):
    return ImageFont.truetype(os.path.join(FONTS, name), size)

MUSIC = font("Bravura.otf", 4 * G.S)          # SMuFL: 1 em = 4 staff spaces
MUSIC_SMALL = font("Bravura.otf", int(2.6 * G.S))

U = {
    "noteheadBlack": 0xE0A4, "noteheadHalf": 0xE0A3, "noteheadWhole": 0xE0A2,
    "sharp": 0xE262, "flat": 0xE260, "natural": 0xE261, "dot": 0xE1E7,
    "flag8thUp": 0xE240, "flag8thDown": 0xE241, "flag16thUp": 0xE242, "flag16thDown": 0xE243,
    "flag32ndUp": 0xE244, "flag32ndDown": 0xE245,
    "restWhole": 0xE4E3, "restHalf": 0xE4E4, "restQuarter": 0xE4E5, "rest8th": 0xE4E6, "rest16th": 0xE4E7, "rest32nd": 0xE4E8,
    "gClef": 0xE050, "fClef": 0xE062, "brace": 0xE000,
    "timeSig0": 0xE080, "timeSigCommon": 0xE08A, "timeSigCut": 0xE08B,
    "tuplet3": 0xE883, "repeatDot": 0xE044,
    "ottavaAlta": 0xE511, "ottavaBassaVb": 0xE51C, "quindicesimaAlta": 0xE515, "quindicesimaBassaMb": 0xE51D,
}
def ch(name):
    return chr(U[name])

# ---------------------------------------------------------------------------------------------
# Themes
# ---------------------------------------------------------------------------------------------
THEMES = {
    "day": {
        "paper": (247, 243, 232), "ink": (24, 24, 26), "faint": (120, 116, 108),
        "accent": (40, 104, 214), "current_a": 70, "next_a": 26,
        "played_a": 165, "missed": (214, 48, 49), "missed_a": 70,
        "flash": (230, 40, 40), "flash_a": 64, "good": (40, 170, 90), "good_a": 40,
        "panel": (232, 226, 212), "panel_line": (196, 188, 170), "text": (30, 30, 34), "text_dim": (110, 106, 98),
        "hi": (40, 104, 214),
    },
    "night": {
        "paper": (20, 22, 27), "ink": (226, 222, 210), "faint": (120, 118, 112),
        "accent": (242, 163, 58), "current_a": 80, "next_a": 30,
        "played_a": 175, "missed": (235, 70, 70), "missed_a": 90,
        "flash": (255, 50, 50), "flash_a": 80, "good": (60, 200, 110), "good_a": 50,
        "panel": (34, 36, 42), "panel_line": (60, 62, 70), "text": (232, 228, 218), "text_dim": (140, 138, 130),
        "hi": (242, 163, 58),
    },
}

def rgba(c, a=255):
    return (c[0], c[1], c[2], a)

# ---------------------------------------------------------------------------------------------
# Cells
# ---------------------------------------------------------------------------------------------
CW, CH = G.SLOT_W, G.CELL_H
CY = CH // 2                      # row centre inside a cell frame

def text_at(d, x, y, glyph, fnt, fill, anchor="ls"):
    d.text((x, y), glyph, font=fnt, fill=fill, anchor=anchor)

def glyph_width(name, fnt=MUSIC):
    b = fnt.getbbox(ch(name), anchor="ls")
    return b[2] - b[0]

def draw_stem(d, x, y0, y1, ink):
    if y1 < y0:
        y0, y1 = y1, y0
    d.rectangle([x, y0, x + G.STEM_W - 1, y1], fill=ink)

def draw_ledger(d, x0, x1, ink):
    d.rectangle([x0, CY - 1, x1, CY], fill=ink)

def draw_tie(d, piece, side, ink):
    """Tie arcs: a full arc runs from x=30 in one slot to x=22 in the next (head to head). Each
    cell draws its slice. side 0: arc below the note (the cell is one row below the head)."""
    import math
    span = CW - 30 + 22              # arc length in px across the slot boundary
    depth = 6
    def y_at(t):                     # t in 0..1 along the arc
        sag = depth * math.sin(math.pi * t)
        return CY - 4 + sag if side == 0 else CY + 4 - sag
    pts = []
    if piece == 1:      # start: from x=30 to the right edge
        for x in range(30, CW + 1):
            pts.append((x, y_at((x - 30) / span)))
    elif piece == 2:    # end: from the left edge to x=22
        for x in range(0, 23):
            pts.append((x, y_at((CW - 30 + x) / span)))
    else:               # through: flat at the deepest point
        for x in range(0, CW + 1):
            pts.append((x, y_at(0.5)))
    for (x, y) in pts:
        thick = 1.0 + 1.2 * (1 - abs(((x if piece != 2 else x + CW) - 30 - span / 2) / (span / 2)) ** 2) if piece != 3 else 2.2
        d.line([(x, y - thick / 2), (x, y + thick / 2)], fill=ink, width=1)

def draw_cell(key, cls, ink):
    im = Image.new("RGBA", (CW, CH), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    kind, a, b, c, dd, e, f = G.unkey(key)
    if kind == G.EMPTY:
        return im
    if kind == G.LEDGERONLY:
        draw_ledger(d, G.HEAD_X0 - 5, G.HEAD_X0 + G.HEAD_W + 4, ink)
        return im
    if kind == G.HEAD:
        head, place, acc, dot, stem, led = a, b, c, dd, e, f
        hx = G.HEAD_X0 if place == 0 else G.DISP_X0
        if head == 2 and place == 1:
            hx = G.HEAD_X0 + 10
        if led:
            x1 = (hx + (27 if head == 2 else G.HEAD_W) + 4)
            draw_ledger(d, min(G.HEAD_X0, hx) - 5, min(CW - 1, x1), ink)
        # stem piece first so the head covers its end
        sx = G.STEM_UP_X - 1 if stem in (G.STEM_UP_START, G.STEM_THRU_UP) else G.STEM_DOWN_X
        if stem == G.STEM_UP_START:
            draw_stem(d, sx, CY - G.P // 2, CY - 1, ink)
        elif stem == G.STEM_DOWN_START:
            draw_stem(d, sx, CY + 1, CY + G.P // 2, ink)
        elif stem in (G.STEM_THRU_UP, G.STEM_THRU_DOWN):
            draw_stem(d, sx, CY - G.P // 2, CY + G.P // 2, ink)
        name = ["noteheadBlack", "noteheadHalf", "noteheadWhole"][head]
        text_at(d, hx, CY, ch(name), MUSIC, ink)
        if acc:
            an = ["", "sharp", "flat", "natural"][acc]
            w = glyph_width(an)
            text_at(d, G.ACC_X1 - w, CY, ch(an), MUSIC, ink)
        if dot:
            hw = 27 if head == 2 else G.HEAD_W
            dx = hx + hw + 3
            dy = CY if cls == G.SPACE else CY - G.P
            if dx + 7 <= CW:
                text_at(d, dx, dy, ch("dot"), MUSIC, ink)
        return im
    if kind == G.STEM:
        flags, bl, br, nostem, stem, led = a, b, c, dd, e, f
        if led:
            draw_ledger(d, G.HEAD_X0 - 5, G.HEAD_X0 + G.HEAD_W + 4, ink)
        up = stem in (G.STEM_THRU_UP, G.STEM_UP_END)
        sx = G.STEM_UP_X - 1 if up else G.STEM_DOWN_X
        if not nostem:
            if stem in (G.STEM_THRU_UP, G.STEM_THRU_DOWN):
                draw_stem(d, sx, CY - G.P // 2, CY + G.P // 2, ink)
            elif stem == G.STEM_UP_END:
                draw_stem(d, sx, CY, CY + G.P // 2, ink)
            elif stem == G.STEM_DOWN_END:
                draw_stem(d, sx, CY - G.P // 2, CY, ink)
        if flags:
            # the flag hangs from the stem end, three rows (24 px) beyond this cell's centre
            if up:
                nm = ["", "flag8thUp", "flag16thUp", "flag32ndUp"][flags]
                text_at(d, sx, CY - 3 * G.P, ch(nm), MUSIC, ink)
            else:
                nm = ["", "flag8thDown", "flag16thDown", "flag32ndDown"][flags]
                text_at(d, sx, CY + 3 * G.P, ch(nm), MUSIC, ink)
        if stem in (G.STEM_UP_END, G.STEM_DOWN_END) and (bl or br):
            th = G.S // 2                     # beam thickness 0.5 space
            gap = (3 * G.S) // 4              # beam spacing 0.75 space
            for lv in (1, 2):
                if up:
                    y0 = CY + (lv - 1) * gap
                else:
                    y0 = CY - th - (lv - 1) * gap
                if nostem:
                    if lv <= bl:
                        d.rectangle([0, y0, CW - 1, y0 + th - 1], fill=ink)
                    continue
                if lv <= bl:
                    d.rectangle([0, y0, sx + G.STEM_W - 1, y0 + th - 1], fill=ink)
                if lv <= br:
                    d.rectangle([sx, y0, CW - 1, y0 + th - 1], fill=ink)
        return im
    if kind == G.TIE:
        draw_tie(d, a, b, ink)
        return im
    if kind == G.TUPLET:
        g = ch("tuplet3")
        bb = MUSIC_SMALL.getbbox(g, anchor="ls")
        w = bb[2] - bb[0]
        text_at(d, G.HEAD_X0 + (G.HEAD_W - w) // 2, CY + (bb[3] - bb[1]) // 2 - bb[3], g, MUSIC_SMALL, ink)
        return im
    if kind == G.REST:
        rest, dot = a, dd
        nm = ["restWhole", "restHalf", "restQuarter", "rest8th", "rest16th", "rest32nd"][rest]
        w = glyph_width(nm)
        x = G.HEAD_X0 + (G.HEAD_W - w) // 2
        y = CY
        if rest >= 4:
            y = CY - G.P // 2
        text_at(d, x, y, ch(nm), MUSIC, ink)
        if dot:
            text_at(d, x + w + 3, CY - G.P // 2, ch("dot"), MUSIC, ink)
        return im
    return im

# ---------------------------------------------------------------------------------------------
# Marks (0.2.0): drawn above the staff lines, one widget per cell, same rectangle as the cell
# ---------------------------------------------------------------------------------------------
HEAD_GLYPHS = ["noteheadBlack", "noteheadHalf", "noteheadWhole"]

def head_x(head, place):
    if place == 0:
        return G.HEAD_X0
    return G.HEAD_X0 + 10 if head == 2 else G.DISP_X0

def head_box(head, place):
    """Bounding box (x0, y0, x1, y1) of a notehead glyph in a cell frame."""
    b = MUSIC.getbbox(ch(HEAD_GLYPHS[head]), anchor="ls")
    x = head_x(head, place)
    return (x + b[0], CY + b[1], x + b[2], CY + b[3])

def theme_trail(theme):
    """Opacity of a mark in a passed column: about what the played-note fade leaves of a head."""
    return {"day": 0.42, "night": 0.40}[theme]

DOT_SIZES = (8.0, 6.0, 7.0)     # dot diameter in px for black, half and whole heads

def mark_image(fr, theme):
    t = THEMES[theme]
    kind, trail, place, x3, x4 = fr
    S4 = 4                                            # draw at 4x and scale down for clean edges
    im = Image.new("RGBA", (CW, CH), (0, 0, 0, 0))
    if kind == G.MARK_EMPTY:
        return im
    if kind == G.MARK_DOT:
        head, colour = x3, G.PALETTE[x4][1]
        x0, y0, x1, y1 = head_box(head, place)
        cx, cy = (x0 + x1) / 2.0, (y0 + y1) / 2.0
        big = Image.new("RGBA", (CW * S4, CH * S4), (0, 0, 0, 0))
        d = ImageDraw.Draw(big)
        r = DOT_SIZES[head] / 2.0 * S4
        o = 1.0 * S4
        # a dark edge lets light colours read on light heads and paper; a light edge lets dark
        # ones (blue, navy) read on black heads and dark paper
        edge = (240, 240, 236, 230) if G.luminance(colour) < 0.25 else (16, 16, 18, 235)
        d.ellipse([cx * S4 - r - o, cy * S4 - r - o, cx * S4 + r + o, cy * S4 + r + o], fill=edge)
        d.ellipse([cx * S4 - r, cy * S4 - r, cx * S4 + r, cy * S4 + r], fill=rgba(colour))
        return big.resize((CW, CH), Image.LANCZOS)
    if kind == G.MARK_TINT:
        head, tint = x3, x4
        col = t["good"] if tint == 1 else t["missed"]
        d = ImageDraw.Draw(im)
        a = int(255 * (theme_trail(theme) + 0.12 if trail else 1.0))
        d.text((head_x(head, place), CY), ch(HEAD_GLYPHS[head]), font=MUSIC, fill=rgba(col, a), anchor="ls",
               stroke_width=1, stroke_fill=rgba(col, a))
        return im
    if kind == G.MARK_OTTAVA:
        sign, cont = x3, x4
        ink = rgba(t["ink"])
        d = ImageDraw.Draw(im)
        x = 0
        if not cont:
            name = ["", "ottavaAlta", "ottavaBassaVb", "quindicesimaAlta", "quindicesimaBassaMb"][sign]
            g = ch(name)
            bb = MUSIC_SMALL.getbbox(g, anchor="ls")
            d.text((2, CY + (bb[3] - bb[1]) // 2 - bb[3]), g, font=MUSIC_SMALL, fill=ink, anchor="ls")
            x = 2 + (bb[2] - bb[0]) + 4
        while x < CW:                                  # dashes on to the next column
            d.rectangle([x, CY - 1, min(CW - 1, x + 5), CY], fill=ink)
            x += 10
        return im
    if kind in (G.MARK_GHOST, G.MARK_GLEDGER):
        col = t["missed"]
        a = int(255 * (0.55 if trail else 0.92))
        d = ImageDraw.Draw(im)
        ledger = x3 if kind == G.MARK_GHOST else 1
        if ledger:
            d.rectangle([G.HEAD_X0 - 5, CY - 1, G.HEAD_X0 + G.HEAD_W + 4, CY], fill=rgba(col, a))
        if kind == G.MARK_GHOST:
            acc = x4
            d.text((G.HEAD_X0, CY), ch("noteheadBlack"), font=MUSIC, fill=rgba(col, a), anchor="ls")
            if acc:
                an = ["", "sharp", "flat", "natural"][acc]
                w = glyph_width(an)
                d.text((G.ACC_X1 - w, CY), ch(an), font=MUSIC, fill=rgba(col, a), anchor="ls")
        return im
    return im

def mark_strip(theme):
    frames = G.MARK_FRAMES
    strip = Image.new("RGBA", (CW, CH * len(frames)), (0, 0, 0, 0))
    for i, fr in enumerate(frames):
        strip.paste(mark_image(fr, theme), (0, i * CH))
    return strip

SWATCH_W = 34
def swatch_strip(theme):
    t = THEMES[theme]
    frames = []
    S4 = 4
    for name, colour in G.PALETTE:
        big = Image.new("RGBA", (SWATCH_W * S4, SWATCH_W * S4), (0, 0, 0, 0))
        d = ImageDraw.Draw(big)
        box = [2 * S4, 2 * S4, (SWATCH_W - 2) * S4, (SWATCH_W - 2) * S4]
        if colour is None:
            d.rounded_rectangle(box, 6 * S4, outline=rgba(t["text_dim"]), width=2 * S4)
            d.line([(box[0] + 6 * S4, box[3] - 6 * S4), (box[2] - 6 * S4, box[1] + 6 * S4)], fill=rgba(t["text_dim"]), width=2 * S4)
        else:
            d.rounded_rectangle(box, 6 * S4, fill=rgba(colour), outline=rgba(t["panel_line"]), width=2 * S4)
        frames.append(big.resize((SWATCH_W, SWATCH_W), Image.LANCZOS))
    return stack(frames)

def cell_strip(cls, theme):
    ink = rgba(THEMES[theme]["ink"])
    frames = G.CATALOGUES[cls]
    strip = Image.new("RGBA", (CW, CH * len(frames)), (0, 0, 0, 0))
    for i, k in enumerate(frames):
        strip.paste(draw_cell(k, cls, ink), (0, i * CH))
    return strip

# ---------------------------------------------------------------------------------------------
# Other strips
# ---------------------------------------------------------------------------------------------
OV_W = G.SLOT_W
OV_H = G.GRID_BOTTOM - G.GRID_TOP

def overlay_strip(theme):
    t = THEMES[theme]
    frames = []
    for name in G.OVERLAY_FRAMES:
        im = Image.new("RGBA", (OV_W, OV_H), (0, 0, 0, 0))
        d = ImageDraw.Draw(im)
        if name == "current":
            d.rectangle([0, 0, OV_W - 1, OV_H - 1], fill=rgba(t["accent"], t["current_a"]))
            d.rectangle([0, 0, OV_W - 1, 3], fill=rgba(t["accent"], 255))
            d.rectangle([0, OV_H - 4, OV_W - 1, OV_H - 1], fill=rgba(t["accent"], 255))
        elif name == "next":
            d.rectangle([0, 0, OV_W - 1, OV_H - 1], fill=rgba(t["accent"], t["next_a"]))
        elif name == "played":
            d.rectangle([0, 0, OV_W - 1, OV_H - 1], fill=rgba(t["paper"], t["played_a"]))
        elif name == "missed":
            d.rectangle([0, 0, OV_W - 1, OV_H - 1], fill=rgba(t["paper"], t["played_a"] - 40))
            d.rectangle([0, 0, OV_W - 1, OV_H - 1], fill=rgba(t["missed"], t["missed_a"]))
        elif name == "hidden":
            d.rectangle([0, 0, OV_W - 1, OV_H - 1], fill=rgba(t["paper"], 255))
        frames.append(im)
    return stack(frames)

def stack(frames):
    w = max(f.size[0] for f in frames)
    h = frames[0].size[1]
    out = Image.new("RGBA", (w, h * len(frames)), (0, 0, 0, 0))
    for i, f in enumerate(frames):
        out.paste(f, (0, i * h))
    return out

MID_Y = (G.TREBLE_LINES_Y[-1] + G.BASS_LINES_Y[0]) // 2

def staffmask_strip(theme, staff):
    t = THEMES[theme]
    y0, y1 = (G.GRID_TOP, MID_Y) if staff == 0 else (MID_Y, G.GRID_BOTTOM)
    w, h = G.SLOTS * G.SLOT_W, y1 - y0
    frames = []
    for name in G.STAFFMASK_FRAMES:
        im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        d = ImageDraw.Draw(im)
        if name == "dim":
            d.rectangle([0, 0, w - 1, h - 1], fill=rgba(t["paper"], 150))
        elif name == "hidden":
            d.rectangle([0, 0, w - 1, h - 1], fill=rgba(t["paper"], 235))
        frames.append(im)
    return stack(frames), (y0, h, w)

BAR_W = 18
BAR_Y0 = G.TREBLE_LINES_Y[0]
BAR_H = G.BASS_LINES_Y[-1] - BAR_Y0 + 1

def barline_strip(theme):
    ink = rgba(THEMES[theme]["ink"])
    frames = []
    cx = BAR_W // 2
    for name in G.BARLINE_FRAMES:
        im = Image.new("RGBA", (BAR_W, BAR_H), (0, 0, 0, 0))
        d = ImageDraw.Draw(im)
        def line(x, w=2):
            d.rectangle([x, 0, x + w - 1, BAR_H - 1], fill=ink)
        def dots(x):
            for staff_lines in (G.TREBLE_LINES_Y, G.BASS_LINES_Y):
                for p in (5, 3):   # spaces above and below the middle line
                    y = staff_lines[0] + (8 - p) * G.P - BAR_Y0
                    d.ellipse([x - 2, y - 2, x + 2, y + 2], fill=ink)
        if name == "single":
            line(cx - 1)
        elif name == "double":
            line(cx - 4); line(cx + 2)
        elif name == "final":
            line(cx - 6); line(cx - 1, 5)
        elif name == "repeat_start":
            line(cx - 6, 4); line(cx); dots(cx + 6)
        elif name == "repeat_end":
            dots(cx - 7); line(cx - 3); line(cx + 2, 4)
        frames.append(im)
    return stack(frames)

KS_W, KS_H = 72, 4 * G.S + 2 * 34
KS_X = 78                 # key signature widget x
KS_STEP = 9.5             # px between accidentals
TS_X = G.SLOTS_X0 - 40    # time signature widget x
def keysig_strip(theme, staff):
    ink = rgba(THEMES[theme]["ink"])
    lines = G.TREBLE_LINES_Y if staff == 0 else G.BASS_LINES_Y
    top = lines[0] - 34                 # widget top in tab coordinates
    sharp_pos = [8, 5, 9, 6, 3, 7, 4] if staff == 0 else [6, 3, 7, 4, 1, 5, 2]
    flat_pos = [4, 7, 3, 6, 2, 5, 1] if staff == 0 else [2, 5, 1, 4, 0, 3, -1]
    frames = []
    for sf in G.KEYSIG_FRAMES:
        im = Image.new("RGBA", (KS_W, KS_H), (0, 0, 0, 0))
        d = ImageDraw.Draw(im)
        n = abs(sf)
        for i in range(n):
            p = (sharp_pos if sf > 0 else flat_pos)[i]
            y = lines[-1] - p * G.P - top
            text_at(d, 1 + int(i * KS_STEP), y, ch("sharp" if sf > 0 else "flat"), MUSIC, ink)
        frames.append(im)
    return stack(frames), top

TS_W, TS_H = 40, 4 * G.S + 8
def timesig_strip(theme, staff):
    ink = rgba(THEMES[theme]["ink"])
    lines = G.TREBLE_LINES_Y if staff == 0 else G.BASS_LINES_Y
    top = lines[0] - 4
    frames = []
    for name in G.TIMESIG_FRAMES:
        im = Image.new("RGBA", (TS_W, TS_H), (0, 0, 0, 0))
        d = ImageDraw.Draw(im)
        mid = lines[2] - top
        if name == "C":
            g = ch("timeSigCommon"); w = glyph_width("timeSigCommon")
            text_at(d, (TS_W - w) // 2, mid, g, MUSIC, ink)
        elif name == "C|":
            g = ch("timeSigCut"); b = MUSIC.getbbox(g, anchor="ls")
            text_at(d, (TS_W - (b[2] - b[0])) // 2, mid, g, MUSIC, ink)
        elif "/" in name:
            num, den = name.split("/")
            for s, y in ((num, mid - G.S), (den, mid + G.S)):
                g = "".join(chr(U["timeSig0"] + int(c)) for c in s)
                b = MUSIC.getbbox(g, anchor="ls")
                text_at(d, (TS_W - (b[2] - b[0])) // 2, y, g, MUSIC, ink)
        frames.append(im)
    return stack(frames), top

def text_strip(words, theme, w, h, size, fnt="TitilliumWeb-SemiBold.ttf", color="text", align="center"):
    t = THEMES[theme]
    f = font(fnt, size)
    frames = []
    for word in words:
        im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        d = ImageDraw.Draw(im)
        if word:
            col = rgba(t[color]) if isinstance(color, str) else color
            if align == "center":
                d.text((w // 2, h // 2), word, font=f, fill=col, anchor="mm")
            elif align == "left":
                d.text((2, h // 2), word, font=f, fill=col, anchor="lm")
            else:
                d.text((w - 2, h // 2), word, font=f, fill=col, anchor="rm")
        frames.append(im)
    return stack(frames)

def flash_strip(theme, w, h):
    t = THEMES[theme]
    frames = []
    for name in G.FLASH_FRAMES:
        im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        d = ImageDraw.Draw(im)
        if name == "wrong":
            d.rectangle([0, 0, w - 1, h - 1], fill=rgba(t["flash"], t["flash_a"]))
        elif name == "good":
            d.rectangle([0, 0, w - 1, h - 1], fill=rgba(t["good"], t["good_a"]))
        frames.append(im)
    return stack(frames)

def bar_strip(theme, w, h):
    """Accuracy bars: frame k shows k*5 percent filled from the bottom."""
    t = THEMES[theme]
    frames = []
    for k in range(G.BAR_FRAMES):
        im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        d = ImageDraw.Draw(im)
        d.rectangle([0, 0, w - 1, h - 1], outline=rgba(t["panel_line"]), width=1)
        fill = int((h - 2) * k / (G.BAR_FRAMES - 1))
        if fill:
            col = t["missed"] if k < 12 else (t["accent"] if k < 18 else t["good"])
            d.rectangle([1, h - 1 - fill, w - 2, h - 2], fill=rgba(col))
        frames.append(im)
    return stack(frames)

def staff_image(theme):
    """Static staff: ten lines, the system bracket, brace and clefs (drawn above the overlays)."""
    t = THEMES[theme]
    ink = rgba(t["ink"])
    im = Image.new("RGBA", (G.TAB_W, G.GRID_BOTTOM), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    x0, x1 = 34, G.SLOTS_X0 + G.SLOTS * G.SLOT_W
    for y in G.TREBLE_LINES_Y + G.BASS_LINES_Y:
        d.rectangle([x0, y, x1, y + 1], fill=ink)
    d.rectangle([x0, G.TREBLE_LINES_Y[0], x0 + 1, G.BASS_LINES_Y[-1] + 1], fill=ink)
    # brace spanning both staves
    height = G.BASS_LINES_Y[-1] - G.TREBLE_LINES_Y[0]
    bf = font("Bravura.otf", height)
    d.text((x0 - 16, G.BASS_LINES_Y[-1] + 1), ch("brace"), font=bf, fill=ink, anchor="ls")
    # clefs: G clef on the G4 line (position 2), F clef on the F3 line (position 6)
    text_at(d, x0 + 8, G.TREBLE_LINES_Y[3] + 1, ch("gClef"), MUSIC, ink)
    text_at(d, x0 + 8, G.BASS_LINES_Y[1] + 1, ch("fClef"), MUSIC, ink)
    return im

def render_theme(theme, out):
    os.makedirs(out, exist_ok=True)
    meta = {}
    for cls in (G.SPACE, G.LINE, G.LEDGER):
        name = "%s_cells_%s.png" % (theme, G.CLASS_NAMES[cls])
        cell_strip(cls, theme).save(os.path.join(out, name), optimize=True)
        meta["cells_%s" % G.CLASS_NAMES[cls]] = (name, len(G.CATALOGUES[cls]))
    overlay_strip(theme).save(os.path.join(out, "%s_overlay.png" % theme), optimize=True)
    meta["overlay"] = ("%s_overlay.png" % theme, len(G.OVERLAY_FRAMES))
    for staff in (0, 1):
        img, (y0, h, w) = staffmask_strip(theme, staff)
        img.save(os.path.join(out, "%s_mask%d.png" % (theme, staff)), optimize=True)
        meta["mask%d" % staff] = ("%s_mask%d.png" % (theme, staff), len(G.STAFFMASK_FRAMES), y0, h, w)
    barline_strip(theme).save(os.path.join(out, "%s_barline.png" % theme), optimize=True)
    meta["barline"] = ("%s_barline.png" % theme, len(G.BARLINE_FRAMES))
    for staff in (0, 1):
        img, top = keysig_strip(theme, staff)
        img.save(os.path.join(out, "%s_keysig%d.png" % (theme, staff)), optimize=True)
        meta["keysig%d" % staff] = ("%s_keysig%d.png" % (theme, staff), len(G.KEYSIG_FRAMES), top)
        img, top = timesig_strip(theme, staff)
        img.save(os.path.join(out, "%s_timesig%d.png" % (theme, staff)), optimize=True)
        meta["timesig%d" % staff] = ("%s_timesig%d.png" % (theme, staff), len(G.TIMESIG_FRAMES), top)
    staff_image(theme).save(os.path.join(out, "%s_staff.png" % theme), optimize=True)
    mark_strip(theme).save(os.path.join(out, "%s_marks.png" % theme), optimize=True)
    meta["marks"] = ("%s_marks.png" % theme, len(G.MARK_FRAMES))
    return meta

if __name__ == "__main__":
    import sys
    out = sys.argv[1] if len(sys.argv) > 1 else "/tmp/skin_test"
    for th in THEMES:
        print(th, render_theme(th, out))

# ---------------------------------------------------------------------------------------------
# Header pictures and controls
# ---------------------------------------------------------------------------------------------
DIGIT_W, DIGIT_H = 17, 30
SMALL_DIGIT_W, SMALL_DIGIT_H = 9, 16
STATUS_W, STATUS_H = 128, 40
HAND_W, HAND_H = 70, 22
NOTE_W, NOTE_H = 60, 32
FINGER_W, FINGER_H = 22, 30
LINNSTATE_W, LINNSTATE_H = 300, 26
STATBAR_W, STATBAR_H = 52, 220
KNOB_SIZE, KNOB_FRAMES = 64, 64

def ui_images(theme, out):
    """Writes the header/controls images for a theme; returns {name: (file, frames, w, h)}."""
    t = THEMES[theme]
    meta = {}
    def save(name, img, frames, w, h):
        fn = "%s_%s.png" % (theme, name)
        img.save(os.path.join(out, fn), optimize=True)
        meta[name] = (fn, frames, w, h)
    save("digits", text_strip(G.DIGIT_FRAMES, theme, DIGIT_W, DIGIT_H, 28, "TitilliumWeb-SemiBold.ttf"), len(G.DIGIT_FRAMES), DIGIT_W, DIGIT_H)
    save("digits_small", text_strip(G.DIGIT_FRAMES, theme, SMALL_DIGIT_W, SMALL_DIGIT_H, 15, "TitilliumWeb-SemiBold.ttf", "text_dim"), len(G.DIGIT_FRAMES), SMALL_DIGIT_W, SMALL_DIGIT_H)
    save("status", text_strip(G.STATUS_FRAMES, theme, STATUS_W, STATUS_H, 26, "TitilliumWeb-Bold.ttf", rgba(t["hi"])), len(G.STATUS_FRAMES), STATUS_W, STATUS_H)
    save("hand", text_strip(G.HAND_FRAMES, theme, HAND_W, HAND_H, 17, "TitilliumWeb-SemiBold.ttf", "text_dim"), len(G.HAND_FRAMES), HAND_W, HAND_H)
    save("notename", text_strip(G.notename_frames(), theme, NOTE_W, NOTE_H, 27, "TitilliumWeb-Bold.ttf"), len(G.notename_frames()), NOTE_W, NOTE_H)
    save("notename_played", text_strip(G.notename_frames(), theme, NOTE_W, NOTE_H, 27, "TitilliumWeb-SemiBold.ttf", "text_dim"), len(G.notename_frames()), NOTE_W, NOTE_H)
    save("finger", text_strip(G.FINGER_FRAMES, theme, FINGER_W, FINGER_H, 26, "TitilliumWeb-Bold.ttf", rgba(t["hi"])), len(G.FINGER_FRAMES), FINGER_W, FINGER_H)
    save("linnstate", text_strip(G.LINNSTATE_FRAMES, theme, LINNSTATE_W, LINNSTATE_H, 17, "TitilliumWeb-SemiBold.ttf", "text_dim", "left"), len(G.LINNSTATE_FRAMES), LINNSTATE_W, LINNSTATE_H)
    save("statbar", bar_strip(theme, STATBAR_W, STATBAR_H), G.BAR_FRAMES, STATBAR_W, STATBAR_H)
    save("flash", flash_strip(theme, G.TAB_W, G.GRID_BOTTOM - G.GRID_TOP), len(G.FLASH_FRAMES), G.TAB_W, G.GRID_BOTTOM - G.GRID_TOP)
    save("knob", knob_strip(theme), KNOB_FRAMES, KNOB_SIZE, KNOB_SIZE)
    save("swatch", swatch_strip(theme), G.SWATCH_FRAMES, SWATCH_W, SWATCH_W)
    save("chevron", chevron_image(theme), 1, 16, 12)
    save("vrule", rule_image(theme, 1, 40), 1, 1, 40)
    return meta

def knob_strip(theme):
    import math
    t = THEMES[theme]
    frames = []
    S4 = KNOB_SIZE * 4
    for k in range(KNOB_FRAMES):
        v = k / (KNOB_FRAMES - 1.0)
        im = Image.new("RGBA", (S4, S4), (0, 0, 0, 0))
        d = ImageDraw.Draw(im)
        pad = 18
        box = [pad, pad, S4 - pad, S4 - pad]
        d.arc(box, 135, 405, fill=rgba(t["panel_line"]), width=22)
        if v > 0:
            d.arc(box, 135, 135 + 270 * v, fill=rgba(t["hi"]), width=22)
        inner = [pad + 46, pad + 46, S4 - pad - 46, S4 - pad - 46]
        d.ellipse(inner, fill=rgba(t["panel"]), outline=rgba(t["panel_line"]), width=6)
        ang = math.radians(135 + 270 * v)
        cx = cy = S4 / 2
        r0, r1 = 20, (S4 / 2) - pad - 60
        d.line([(cx + r0 * math.cos(ang), cy + r0 * math.sin(ang)), (cx + r1 * math.cos(ang), cy + r1 * math.sin(ang))], fill=rgba(t["text"]), width=14)
        frames.append(im.resize((KNOB_SIZE, KNOB_SIZE), Image.LANCZOS))
    return stack(frames)

def box_image(theme, w, h, on, style="segment"):
    """Transparent-centred highlight images for buttons drawn OVER their caption labels."""
    t = THEMES[theme]
    S2 = 2
    im = Image.new("RGBA", (w * S2, h * S2), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    r = 10 * S2
    if style == "row":
        if on:
            d.rounded_rectangle([0, 0, w * S2 - 1, h * S2 - 1], r, fill=rgba(t["hi"], 70), outline=rgba(t["hi"]), width=3 * S2)
        else:
            d.line([(8 * S2, h * S2 - 2), (w * S2 - 8 * S2, h * S2 - 2)], fill=rgba(t["panel_line"]), width=S2)
    elif style == "button":
        if on:
            d.rounded_rectangle([0, 0, w * S2 - 1, h * S2 - 1], r, fill=rgba(t["hi"], 120), outline=rgba(t["hi"]), width=3 * S2)
        else:
            d.rounded_rectangle([0, 0, w * S2 - 1, h * S2 - 1], r, fill=rgba(t["text"], 18), outline=rgba(t["panel_line"]), width=2 * S2)
    else:   # segment of a switch
        if on:
            d.rounded_rectangle([1, 1, w * S2 - 2, h * S2 - 2], r, fill=rgba(t["hi"], 110), outline=rgba(t["hi"]), width=2 * S2)
        else:
            d.rounded_rectangle([1, 1, w * S2 - 2, h * S2 - 2], r, fill=rgba(t["text"], 10), outline=rgba(t["panel_line"]), width=2 * S2)
    return im.resize((w, h), Image.LANCZOS)

def chevron_image(theme, w=16, h=12):
    """The title's drop-down mark (0.5.0)."""
    t = THEMES[theme]
    S = 4
    im = Image.new("RGBA", (w * S, h * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.line([(2 * S, 3 * S), (w * S / 2, (h - 3) * S), ((w - 2) * S, 3 * S)], fill=rgba(t["text_dim"]), width=2 * S, joint="curve")
    return im.resize((w, h), Image.LANCZOS)

def rule_image(theme, w, h):
    t = THEMES[theme]
    return Image.new("RGBA", (w, h), rgba(t["panel_line"]))

def select_strip(theme, w, h, n):
    """Row highlights (0.5.0): frame 0 nothing but a faint rule, 1 the loaded piece (an outline),
    2 (or 1 when n == 2) the selection (a filled box)."""
    t = THEMES[theme]
    S2 = 2
    frames = []
    for f in range(n):
        im = Image.new("RGBA", (w * S2, h * S2), (0, 0, 0, 0))
        d = ImageDraw.Draw(im)
        r = 8 * S2
        selected = f == n - 1 and f > 0
        if selected:
            d.rounded_rectangle([0, 0, w * S2 - 1, h * S2 - 1], r, fill=rgba(t["hi"], 70), outline=rgba(t["hi"]), width=3 * S2)
        elif f == 1:
            d.rounded_rectangle([S2, S2, w * S2 - 1 - S2, h * S2 - 1 - S2], r, fill=rgba(t["text"], 14), outline=rgba(t["text_dim"]), width=2 * S2)
        else:
            d.line([(8 * S2, h * S2 - 2), (w * S2 - 8 * S2, h * S2 - 2)], fill=rgba(t["panel_line"]), width=S2)
        frames.append(im.resize((w, h), Image.LANCZOS))
    return stack(frames)

def caption_button(theme, w, h, on, style, caption):
    """A button or switch segment with its caption drawn in (0.4.0): the text is part of the
    picture, so nothing can hide it, and the chosen segment is a solid fill with contrasting
    text instead of a tint over a separate label."""
    t = THEMES[theme]
    S2 = 2
    im = Image.new("RGBA", (w * S2, h * S2), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    r = 10 * S2
    box = [1, 1, w * S2 - 2, h * S2 - 2]
    if on:
        d.rounded_rectangle(box, r, fill=rgba(t["hi"]), outline=rgba(t["hi"]), width=2 * S2)
        col = rgba(t["paper"])
    else:
        d.rounded_rectangle(box, r, fill=rgba(t["text"], 16), outline=rgba(t["panel_line"]), width=2 * S2)
        col = rgba(t["text"])
    size = 21 if style == "button" else 19
    while size > 11:
        f = font("TitilliumWeb-SemiBold.ttf", size * S2)
        if d.textlength(caption, font=f) <= (w - 10) * S2:
            break
        size -= 1
    d.text((w * S2 / 2, h * S2 / 2), caption, font=f, fill=col, anchor="mm")
    return im.resize((w, h), Image.LANCZOS)

def readout_image(theme, text, w, h, size, max_lines, named=True):
    """One readout: "Name: what it does", the name brighter (named), wrapped to max_lines."""
    t = THEMES[theme]
    S2 = 2
    im = Image.new("RGBA", (w * S2, h * S2), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    bold = font("TitilliumWeb-SemiBold.ttf", size * S2)
    reg = font("TitilliumWeb-Regular.ttf", size * S2)
    head, sep, rest = text.partition(": ") if named else (text, "", "")
    words = ([(wd, True) for wd in (head + ":").split()] + [(wd, False) for wd in rest.split()]) if sep else [(wd, False) for wd in text.split()]
    lines, line, width = [], [], 0.0
    space = d.textlength(" ", font=reg)
    for wd, b in words:
        wl = d.textlength(wd, font=bold if b else reg)
        if line and width + space + wl > w * S2:
            lines.append(line); line, width = [], 0.0
        width += (space if line else 0) + wl
        line.append((wd, b, wl))
    if line:
        lines.append(line)
    if len(lines) > max_lines:
        raise ValueError("readout too long for %d line(s): %r" % (max_lines, text))
    lh = (size + 4) * S2
    y0 = (h * S2 - lh * len(lines)) / 2 + lh / 2
    for i, ln in enumerate(lines):
        x = 0.0
        for wd, b, wl in ln:
            d.text((x, y0 + i * lh), wd, font=bold if b else reg, fill=rgba(t["text"] if b else t["text_dim"]), anchor="lm")
            x += wl + space
    return im.resize((w, h), Image.LANCZOS)

def readout_strip(theme, texts, w, h, size, max_lines):
    """One frame per text, for a display parameter that mirrors the control."""
    return stack([readout_image(theme, tx, w, h, size, max_lines) for tx in texts]), len(texts)

def barline_cell_strip(theme):
    """Barlines drawn at the left edge of a slot-wide frame (for repeated slot columns)."""
    src = barline_strip(theme)
    n = len(G.BARLINE_FRAMES)
    h = src.size[1] // n
    out = Image.new("RGBA", (BAR_W, h * n), (0, 0, 0, 0))
    out.paste(src, (0, 0))
    return out
