#!/usr/bin/env python3
"""Generates the plugin's MPC skin and the C++ tables that must agree with it.

    python3 tools/gen_skin.py            # writes plugin/skins/ and plugin/src/sr/gen/

Outputs
  plugin/src/sr/gen/layout_gen.h   staff geometry and cell frame tables (tools/skin/geometry.py)
  plugin/src/sr/gen/params_gen.h   the parameter table (tools/skin/params.py)
  plugin/skins/TUI.json            touch screen: 9 pages; PLAY also in a Day version (LIGHT)
  plugin/skins/Q-Links.json        what the 16 knobs do on each page
  plugin/skins/*.png               every film strip and picture

Format: docs/ui-reference.md of Akai's MPC plugin SDK. Key order matters to the MPC's parser,
and the overlay widgets need two consecutive "version" keys, so this writes JSON by hand.

Pages (function keys):
  PLAY   the grand staff, counters, transport (Night); press its key again for LIGHT (Day)
  LOAD   library browser          GEN    exercise generator
  SETUP  practice settings        MORE   range, names, accompaniment, ramp
  SOUND  the error sound, metronome    COLOR  note colour dots, played-note marks
  LINN   LinnStrument lights      STATS  progress for the current piece
"""
import os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "skin"))
import geometry as G     # noqa: E402
import params as PR      # noqa: E402
import render as R       # noqa: E402
import readouts as RO    # noqa: E402

ROOT = os.path.dirname(HERE)
SKINS = os.path.join(ROOT, "plugin", "skins")
GEN_DIR = os.path.join(ROOT, "plugin", "src", "sr", "gen")
IDX = PR.INDEX

# ---------------------------------------------------------------------------------------------
# JSON with ordered (and duplicate) keys
# ---------------------------------------------------------------------------------------------
class O(list):
    """An ordered JSON object; a list of (key, value) pairs so keys may repeat."""

def o(*pairs):
    return O(pairs)

def dump(v):
    if isinstance(v, O):
        return "{" + ",".join('"%s":%s' % (k, dump(x)) for k, x in v) + "}"
    if isinstance(v, (list, tuple)):
        return "[" + ",".join(dump(x) for x in v) + "]"
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, int):
        return str(v)
    if isinstance(v, float):
        return ("%.4f" % v).rstrip("0").rstrip(".") if v != int(v) else "%.1f" % v
    s = str(v).replace("\\", "\\\\").replace('"', '\\"')
    return '"%s"' % s

def argb(rgb, a=255):
    return "%02x%02x%02x%02x" % (a, rgb[0], rgb[1], rgb[2])

def P(name):
    return "Parameter %d" % IDX[name]

# ---------------------------------------------------------------------------------------------
# Widgets
# ---------------------------------------------------------------------------------------------
def rect(x, y, w, h):
    return "%d %d %d %d" % (round(x), round(y), round(w), round(h))

def bounds(x, y, w, h, focus="No", show="Show", when="Always", invalid=None):
    pairs = [("version", 2 if invalid else 1), ("acceptsHWFocus", focus), ("showWhenDataModelInvalid", show),
             ("whenVisible", when), ("boundsType", "Absolute"), ("bounds", rect(x, y, w, h))]
    if invalid:
        pairs.append(("additionalInvalidatingHandles", invalid))
    return O(pairs)

def entry(name, typ, data, b, remap=()):
    return o(("version", 2),
             ("componentData", o(("version", 1), ("name", name), ("type", typ), ("data", data))),
             ("handle remapping", o(("version", 1), ("map", [o(("key", k), ("value", v)) for k, v in remap]))),
             ("bounds", b))

def strip(name, img, frames, handle, x, y, w, h, invalid=None):
    data = o(("version", 2), ("imageType", "FilmStrip"), ("colour", "0"), ("image", img), ("numFrames", frames), ("handleName", handle))
    return entry(name, "Image", data, bounds(x, y, w, h, invalid=invalid))

def picture(name, img, x, y, w, h, colour="0"):
    data = o(("version", 2), ("imageType", "Regular"), ("colour", colour), ("image", img))
    return entry(name, "Image", data, bounds(x, y, w, h))

def tstyle(size, colour, just="left verticallyCentred", style="Regular", case="Original", font="Titillium Web"):
    return o(("version", 1), ("font", o(("version", 1), ("name", font), ("style", style), ("height", float(size)))),
             ("colour", colour), ("justification", just), ("case", case))

def label(name, kind, handle, x, y, w, h, size, colour, just="left verticallyCentred", style="Regular", case="Original", text=None):
    pairs = [("version", 1), ("textStyle", tstyle(size, colour, just, style, case)), ("type", kind), ("handleName", handle)]
    if kind == "Text":
        pairs.append(("text", text))
    return entry(name, "Label", O(pairs), bounds(x, y, w, h))

def paragraph(name, text, x, y, w, size, colour, line_h=None):
    """Wrapped help text as one Label per line, measured with the same font the MPC uses."""
    from PIL import ImageFont
    f = ImageFont.truetype(os.path.join(HERE, "skin", "fonts", "TitilliumWeb-Regular.ttf"), int(size))
    line_h = line_h or int(size * 1.35)
    out, line = [], ""
    for word in text.split():
        trial = (line + " " + word).strip()
        if f.getlength(trial) > w - 8 and line:
            out.append(line); line = word
        else:
            line = trial
    if line:
        out.append(line)
    return [label("%s%d" % (name, i), "Text", "Invalid", x, y + i * line_h, w, line_h, size, colour, "left verticallyCentred", text=t)
            for i, t in enumerate(out)]

def button(name, on, off, handle, bid, n, x, y, w, h, focus="No"):
    data = o(("version", 2), ("onImage", on), ("offImage", off), ("buttonId", bid), ("numButtonsInGroup", n),
             ("handleName", handle), ("gestureBehaviour", "Instant"))
    return entry(name, "Button", data, bounds(x, y, w, h, focus=focus))

def colourbar(name, handle, x, y, w, h, back, fill):
    data = o(("version", 2), ("sliderType", "ColourBar"), ("direction", "Horizontal"), ("backgroundColour", back),
             ("outlineColour", "0"), ("valueColour", fill), ("isBipolar", False), ("handleName", handle))
    return entry(name, "Slider", data, bounds(x, y, w, h))

def container(children, bg="0", actions=(), version=2):
    return o(("version", version), ("actions", list(actions)),
             ("backgroundData", o(("version", 1), ("focussed", o(("version", 1), ("colour", bg), ("image", ""))),
                                  ("unfocussed", o(("version", 1), ("colour", bg), ("image", ""))))),
             ("ignoreMousePresses", False), ("disableCoarseDataWheel", False), ("componentsData", list(children)))

def action(on, handler, handle="Data", extra=""):
    return o(("version", 1), ("onAction", on), ("handler", handler), ("handleName", handle), ("additionalData", extra))

OVERLAY_ACTIONS = [o(("version", 1), ("onAction", "Focus Lost"), ("handler", "Focus Lost"), ("handleName", "Focus Lost"), ("additionalData", ""))]

def overlay_defs():
    def overlay(name, typ, data):
        comp = o(("version", 2), ("actions", []),
                 ("backgroundData", o(("version", 1), ("focussed", o(("version", 1), ("colour", "0"), ("image", ""))),
                                      ("unfocussed", o(("version", 1), ("colour", "0"), ("image", ""))))),
                 ("ignoreMousePresses", True), ("disableCoarseDataWheel", False),
                 ("componentsData", [entry("overlay", typ, data, bounds(0, 0, 1280, 800, focus="Yes"))]))
        return o(("key", name), ("value", comp))
    knob = O([("version", 2), ("version", 1), ("actions", OVERLAY_ACTIONS), ("overlayType", "Knob With Header"),
              ("scaleType", "Linear"), ("polarityType", "Unipolar"), ("showMinusSign", True), ("handleName", "Data")])
    menu = O([("version", 1), ("version", 1), ("actions", OVERLAY_ACTIONS), ("withNumbers", False), ("handleName", "Data")])
    return [overlay("knob overlay", "KnobOverlay", knob), overlay("menu overlay", "MenuOverlay", menu)]

# ---------------------------------------------------------------------------------------------
# Control slots (a block of the 4 x 4 layout)
# ---------------------------------------------------------------------------------------------
SLOT_W, SLOT_H = 290, 96

class Theme:
    def __init__(self, name, img):
        t = R.THEMES[name]
        self.name = name
        self.img = img                   # image metadata: name -> (file, frames, w, h)
        self.bg = argb(t["paper"])
        self.panel = argb(t["panel"])
        self.text = argb(t["text"])
        self.dim = argb(t["text_dim"])
        self.hi = argb(t["hi"])
        self.line = argb(t["panel_line"])

def control_slot(th, pid, x, y, kind):
    """kind: knob, menu, switch, button, toggle, or pair (pid = two ids: two half-width buttons).
    Returns entries (absolute positions)."""
    if kind == "pair":
        out = []
        gap = 8
        w = (SLOT_W - 20 - gap) // 2
        for k, p in enumerate(pid):
            caption = next(c for c in PR.CONTROL if c[0] == p)[1].upper()
            bx = x + 10 + k * (w + gap)
            on, off = caption_images(th, w, 56, "button", caption)
            out.append(button(p + "_btn", on, off, P(p), 1, 1, bx, y + 26, w, 56))
        return out, None
    idx = IDX[pid]
    handle = "Parameter %d" % idx
    defn = next(p for p in PR.CONTROL if p[0] == pid)
    out = []
    # name above
    out.append(label(pid + "_name", "Name", handle, x + 10, y + 2, SLOT_W - 20, 22, 15, th.dim, "left verticallyCentred", "SemiBold", "Upper Case"))
    if kind in ("knob", "menu"):
        kfile, kframes, kw, kh = th.img["knob"]
        kids = [
            entry("focus", "Focus", o(("version", 1), ("backgroundColour", "19ffffff"), ("outlineColour", th.hi), ("backgroundInset", 2.0), ("outlineThickness", 2.0)),
                  bounds(0, 0, SLOT_W, SLOT_H - 26, when="WhenFocussed")),
            entry("knob", "Knob", o(("version", 3), ("knobType", "FilmStrip"), ("filmStrip", kfile), ("numFrames", kframes), ("invert", False),
                                    ("dragOrientation", "Vertical"), ("handleName", "Data")), bounds(8, 2, kw, kh)),
            label("value", "Value", "Data", 80, 8, SLOT_W - 90, 50, 26 if kind == "knob" else 23, th.text, "left verticallyCentred", "SemiBold"),
        ]
        overlay = "knob overlay" if kind == "knob" else "menu overlay"
        comp = container(kids, actions=[action("Mouse Down", "Q-Link"), action("Double Click", "Show Overlay", "Data", overlay),
                                        action("Enter Pressed", "Show Overlay", "Data", overlay)])
        out.append(o(("version", 2), ("componentData", o(("version", 1), ("name", pid), ("type", "%s_slot_%s" % (kind, th.name)),
                                                          ("data", o(("version", 1), ("handleName", "Data"))))),
                     ("handle remapping", o(("version", 1), ("map", [o(("key", "Data"), ("value", handle))]))),
                     ("bounds", bounds(x, y + 24, SLOT_W, SLOT_H - 26, focus="Yes"))))
        return out, comp
    if kind == "switch":
        choices = defn[3][0] if defn[2] == "choice" else ["Off", "On"]
        n = len(choices)
        gap = 6
        w = (SLOT_W - 20 - gap * (n - 1)) / n
        for k, c in enumerate(choices):
            bx = x + 10 + k * (w + gap)
            on, off = caption_images(th, int(w), 50, "segment", c)
            out.append(button("%s_seg%d" % (pid, k), on, off, handle, k, n, bx, y + 30, w, 50))
        return out, None
    if kind in ("button", "toggle"):
        on, off = caption_images(th, SLOT_W - 20, 56, "button", defn[1].upper())
        out = [button(pid + "_btn", on, off, handle, 1, 1, x + 10, y + 26, SLOT_W - 20, 56)]
        return out, None
    raise ValueError(kind)

_cap_cache = {}
def caption_images(th, w, h, style, caption):
    """On and off pictures of a button or switch segment with its caption drawn in."""
    key = (th.name, w, h, style, caption)
    if key not in _cap_cache:
        slug = "".join(ch if ch.isalnum() else "_" for ch in caption.lower())
        on = "%s_%s_%dx%d_%s_on.png" % (th.name, style, w, h, slug)
        off = "%s_%s_%dx%d_%s_off.png" % (th.name, style, w, h, slug)
        R.caption_button(th.name, w, h, True, style, caption).save(os.path.join(SKINS, on), optimize=True)
        R.caption_button(th.name, w, h, False, style, caption).save(os.path.join(SKINS, off), optimize=True)
        _cap_cache[key] = (on, off)
    return _cap_cache[key]

_ro_cache = {}
def readout(th, pid, x, y, w, short=False):
    """The readout under a control: a strip bound to the control's parameter (one text per value)
    or a fixed picture. Returns entries ([] when the control has no readout)."""
    table = RO.SHORT if short else RO.READOUTS
    if pid not in table:
        return []
    texts = table[pid]
    size, lines, h = (13, 1, 22) if short else (14, 2, 38)
    key = (th.name, pid, w, short)
    if key not in _ro_cache:
        fn = "%s_ro_%s%s_%d.png" % (th.name, pid, "_s" if short else "", w)
        if isinstance(texts, str):
            R.readout_image(th.name, texts, w, h, size, lines, named=False).save(os.path.join(SKINS, fn), optimize=True)
            _ro_cache[key] = (fn, 0)
        else:
            img, n = R.readout_strip(th.name, texts, w, h, size, lines)
            img.save(os.path.join(SKINS, fn), optimize=True)
            _ro_cache[key] = (fn, n)
    fn, n = _ro_cache[key]
    if n == 0:
        return [picture("ro_" + pid, fn, x, y, w, h)]
    return [strip("ro_" + pid, fn, n, "Parameter %d" % (IDX["_ro"] + PR.readout_index(pid)), x, y, w, h)]

_seg_cache = {}
def seg_images(th, w, h, style):
    key = (th.name, w, h, style)
    if key not in _seg_cache:
        on = "%s_%s_%dx%d_on.png" % (th.name, style, w, h)
        off = "%s_%s_%dx%d_off.png" % (th.name, style, w, h)
        R.box_image(th.name, w, h, True, style).save(os.path.join(SKINS, on), optimize=True)
        R.box_image(th.name, w, h, False, style).save(os.path.join(SKINS, off), optimize=True)
        _seg_cache[key] = (on, off)
    return _seg_cache[key]

COLS_X = [16, 332, 648, 964]

def grid_page(th, layout, top=20, row_h=150, extra=(), short=False):
    """layout: up to 4 rows of 4 (pid, kind) or None. Returns (children, local defs, q-link map).
    Each control gets its readout underneath (one line when short, else up to two)."""
    kids, defs = [], {}
    qmap = {}
    groups = []
    for r, row in enumerate(layout):
        group = []
        for c, cell in enumerate(row):
            if not cell:
                continue
            pid, kind = cell
            ents, comp = control_slot(th, pid, COLS_X[c], top + r * row_h, kind)
            if not isinstance(pid, tuple):
                # short: on the name line, right of the name; else under the control. Either way
                # inside the row's Q-Link rectangle.
                if short:
                    ents = ents + readout(th, pid, COLS_X[c] + 80, top + r * row_h + 2, SLOT_W - 90, True)
                else:
                    ents = ents + readout(th, pid, COLS_X[c] + 10, top + r * row_h + 98, SLOT_W - 20)
            kids.extend(ents)
            group.extend(ents)
            if comp is not None:
                defs["%s_slot_%s" % (kind, th.name)] = comp
            qmap[13 + c - 4 * r] = IDX[pid[0] if isinstance(pid, tuple) else pid]
        groups.append(group)
    kids.extend(extra)
    grid_page.last_rects = fit_rects(groups)
    return kids, defs, qmap

def entry_box(e):
    b = dict(e)["bounds"]
    x, y, w, h = [float(v) for v in dict(b)["bounds"].split()]
    return x, y, x + w, y + h

def fit_rects(groups, pad=8):
    """One Q-Link rectangle per group of entries: their union with the same gap on every side."""
    out = []
    for ents in groups:
        if not ents:
            out.append(rect(0, 0, 0, 0)); continue
        boxes = [entry_box(e) for e in ents]
        x0 = min(b[0] for b in boxes); y0 = min(b[1] for b in boxes)
        x1 = max(b[2] for b in boxes); y1 = max(b[3] for b in boxes)
        out.append(rect(x0 - pad, y0 - pad, x1 - x0 + 2 * pad, y1 - y0 + 2 * pad))
    while len(out) < 4:
        out.append(rect(0, 0, 0, 0))
    return out

# ---------------------------------------------------------------------------------------------
# Pages
# ---------------------------------------------------------------------------------------------
def play_page(th):
    img = th.img
    kids = []
    # staff layers, bottom to top: cells, overlays, masks, staff lines, barlines, signatures, flash
    for s in range(G.SLOTS):
        x = G.SLOTS_X0 + s * G.SLOT_W
        for r in range(G.NUM_ROWS):
            cls = G.row_class(r)
            f, n = img["cells_%s" % G.CLASS_NAMES[cls]][:2]
            kids.append(strip("c", f, n, "Parameter %d" % (IDX["_cell"] + s * G.NUM_ROWS + r), x, G.row_y(r) - G.CELL_H // 2, G.SLOT_W, G.CELL_H))
    for s in range(G.SLOTS):
        f, n = img["overlay"][:2]
        kids.append(strip("ov", f, n, "Parameter %d" % (IDX["_overlay"] + s), G.SLOTS_X0 + s * G.SLOT_W, G.GRID_TOP, G.SLOT_W, R.OV_H))
    for staff in (0, 1):
        f, n, y0, h, w = img["mask%d" % staff]
        kids.append(strip("mask", f, n, "Parameter %d" % (IDX["_mask"] + staff), G.SLOTS_X0, y0, w, h))
    kids.append(picture("staff", img["staff"][0], 0, 0, G.TAB_W, G.GRID_BOTTOM))
    # marks: colour dots, green/red heads, wrong-note ghosts (above the staff lines and the fade)
    mf, mn = img["marks"][:2]
    for s in range(G.SLOTS):
        x = G.SLOTS_X0 + s * G.SLOT_W
        for r in range(G.NUM_ROWS):
            kids.append(strip("m", mf, mn, "Parameter %d" % (IDX["_mark"] + s * G.NUM_ROWS + r), x, G.row_y(r) - G.CELL_H // 2, G.SLOT_W, G.CELL_H))
    for i in range(G.SLOTS + 1):
        f, n = img["barline"][:2]
        kids.append(strip("bar", f, n, "Parameter %d" % (IDX["_barline"] + i), G.SLOTS_X0 + i * G.SLOT_W - R.BAR_W // 2, R.BAR_Y0, R.BAR_W, R.BAR_H))
    for staff in (0, 1):
        f, n, top = img["keysig%d" % staff]
        kids.append(strip("key", f, n, P("_keysig"), R.KS_X, top, R.KS_W, R.KS_H))
        f, n, top = img["timesig%d" % staff]
        kids.append(strip("time", f, n, P("_timesig"), R.TS_X, top, R.TS_W, R.TS_H))
    # measure numbers above each half
    sd = img["digits_small"]
    for h in range(2):
        x0 = G.SLOTS_X0 + h * (G.SLOTS // 2) * G.SLOT_W + 2
        for d in range(3):
            kids.append(strip("mnum", sd[0], sd[1], "Parameter %d" % (IDX["_mnum"] + h * 3 + d), x0 + d * R.SMALL_DIGIT_W, G.TREBLE_LINES_Y[0] - 30, R.SMALL_DIGIT_W, R.SMALL_DIGIT_H))
    f, n = img["flash"][:2]
    kids.append(strip("flash", f, n, P("_flash"), 0, G.GRID_TOP, G.TAB_W, G.GRID_BOTTOM - G.GRID_TOP))
    # tap the music to start / pause (on top, so it gets the touch)
    kids.append(entry("tap", "%s_tap" % th.name, o(("version", 1), ("handleName", "Data")), bounds(G.SLOTS_X0, G.GRID_TOP, G.SLOTS * G.SLOT_W, G.GRID_BOTTOM - G.GRID_TOP),
                      [("Data", P("play"))]))
    # header
    kids.append(label("title", "Value", P("text_title"), 16, 2, 490, 34, 25, th.text, "left verticallyCentred", "SemiBold"))
    kids.append(label("msg", "Value", P("text_message"), 16, 34, 490, 24, 16, th.dim))
    st = img["status"]
    kids.append(strip("status", st[0], st[1], P("_status"), 514, 2, R.STATUS_W, R.STATUS_H))
    hd = img["hand"]
    kids.append(strip("hand", hd[0], hd[1], P("_handicon"), 554, 37, R.HAND_W, R.HAND_H))
    dg = img["digits"]
    def digits(group, n, x, y, cap):
        kids.append(label(group + "_cap", "Text", "Invalid", x, y - 18, 80, 18, 13, th.dim, "left verticallyCentred", "SemiBold", "Upper Case", cap))
        for i in range(n):
            kids.append(strip(group, dg[0], dg[1], "Parameter %d" % (IDX["_" + group] + i), x + i * R.DIGIT_W, y, R.DIGIT_W, R.DIGIT_H))
    digits("d_bar", 3, 678, 24, "Bar")
    kids.append(label("slash", "Text", "Invalid", 678 + 3 * R.DIGIT_W, 24, 14, 30, 22, th.dim, "horizontallyCentred verticallyCentred", "Regular", text="/"))
    for i in range(3):
        kids.append(strip("d_bars", dg[0], dg[1], "Parameter %d" % (IDX["_d_bars"] + i), 678 + 3 * R.DIGIT_W + 14 + i * R.DIGIT_W, 24, R.DIGIT_W, R.DIGIT_H))
    digits("d_acc", 3, 802, 24, "Acc %")
    digits("d_err", 3, 870, 24, "Miss")
    digits("d_bpm", 3, 938, 24, "BPM")
    nn = img["notename"]
    kids.append(label("next_cap", "Text", "Invalid", 1000, 6, 70, 18, 13, th.dim, "left verticallyCentred", "SemiBold", "Upper Case", "Next R"))
    kids.append(strip("next", nn[0], nn[1], P("_nextname"), 1000, 24, R.NOTE_W, R.NOTE_H))
    kids.append(label("nextl_cap", "Text", "Invalid", 1072, 6, 70, 18, 13, th.dim, "left verticallyCentred", "SemiBold", "Upper Case", "Next L"))
    kids.append(strip("nextl", nn[0], nn[1], P("_nextname_lh"), 1072, 24, R.NOTE_W, R.NOTE_H))
    pn = img["notename_played"]
    kids.append(label("played_cap", "Text", "Invalid", 1146, 6, 70, 18, 13, th.dim, "left verticallyCentred", "SemiBold", "Upper Case", "Played"))
    kids.append(strip("played", pn[0], pn[1], P("_playedname"), 1146, 24, R.NOTE_W, R.NOTE_H))
    fg = img["finger"]
    kids.append(label("fing_cap", "Text", "Invalid", 1220, 6, 60, 18, 13, th.dim, "left verticallyCentred", "SemiBold", "Upper Case", "R  L"))
    kids.append(strip("fr", fg[0], fg[1], "Parameter %d" % IDX["_finger"], 1220, 24, R.FINGER_W, R.FINGER_H))
    kids.append(strip("fl", fg[0], fg[1], "Parameter %d" % (IDX["_finger"] + 1), 1246, 24, R.FINGER_W, R.FINGER_H))
    kids.append(colourbar("progress", P("_progress"), 0, 60, G.TAB_W, 4, th.line, th.hi))
    # footer: two rows of controls
    layout = [
        [("mode", "switch"), ("tempo", "knob"), ("hands", "switch"), ("view", "menu")],
        [("play", "toggle"), (("listen", "restart"), "pair"), ("loop", "toggle"), ("loop_from", "knob")],
    ]
    fk, defs, qmap = grid_page(th, layout, top=G.FOOTER_Y, row_h=118, short=True)
    kids.extend(fk)
    # the lower two knob rows have no on-screen controls on this page
    for q, pid in zip([5, 6, 7, 8], ["loop_bars", "timing", "pitch_tol", "chord_tol"]):
        qmap[q] = IDX[pid]
    for q, pid in zip([1, 2, 3, 4], ["snd_vol", "metro", "count_in", "mask"]):
        qmap[q] = IDX[pid]
    return kids, defs, qmap, grid_page.last_rects

def load_page(th):
    kids = []
    defs = {}
    left = [[("folder", "knob")], [("scroll", "knob")], [("sort", "switch")]]
    group = []
    for r, row in enumerate(left):
        ents, comp = control_slot(th, row[0][0], 12, 14 + r * 150, row[0][1])
        ents = ents + readout(th, row[0][0], 22, 14 + r * 150 + 98, SLOT_W - 20)
        kids.extend(ents)
        group.extend(ents)
        if comp is not None:
            defs["%s_slot_%s" % (row[0][1], th.name)] = comp
    # the list
    x0, y0, w, h = 324, 14, 640, 58
    for r in range(8):
        kids.append(label("row%d" % r, "Value", P("text_row%d" % r), x0 + 14, y0 + r * h, w - 20, h, 21, th.text, "left verticallyCentred", "Regular"))
    on, off = seg_images(th, w, h - 4, "row")
    for r in range(8):
        kids.append(button("rowbtn%d" % r, on, off, P("row"), r, 8, x0, y0 + r * h + 2, w, h - 4, focus="Yes"))
    # details and actions
    for i in range(4):
        kids.append(label("info%d" % i, "Value", P("text_info%d" % i), 980, 14 + i * 40, 290, 40, 18 if i else 21, th.text if i == 0 else th.dim, "left verticallyCentred", "SemiBold" if i == 0 else "Regular"))
    for pid, yy in (("load", 200), ("rescan", 320)):
        ents, _ = control_slot(th, pid, 976, yy, "button")
        kids.extend(ents)
    kids.append(label("msg", "Value", P("text_message"), 324, 486, 640, 30, 17, th.dim))
    kids.extend(paragraph("hint", "Tap a row, then LOAD. Q-Links: folder, scroll, row, sort. Your own files go in "
                          "SightReader/Library/<folder>/ on this drive; tools/library.py fills it over SSH. Press RESCAN after copying.", 324, 520, 940, 15, th.dim))
    qmap = {13: IDX["folder"], 14: IDX["scroll"], 15: IDX["row"], 16: IDX["sort"]}
    return kids, defs, qmap, fit_rects([group])

def gen_page(th):
    layout = [
        [("gen_key", "menu"), ("gen_scale", "menu"), ("gen_rhythm", "knob"), ("gen_leaps", "knob")],
        [("gen_hands", "switch"), ("gen_bars", "switch"), ("gen_meter", "switch"), ("gen_range", "switch")],
        [("gen_lh", "switch"), ("gen_go", "button"), ("gen_save", "button"), None],
    ]
    kids, defs, qmap = grid_page(th, layout, top=10, row_h=150)
    kids.append(label("desc", "Value", P("text_gen_desc"), 16, 470, 1248, 32, 20, th.text, "left verticallyCentred", "SemiBold"))
    kids.append(label("msg", "Value", P("text_message"), 16, 506, 1248, 30, 17, th.dim))
    return kids, defs, qmap, grid_page.last_rects

def setup_page(th):
    layout = [
        [("mode", "switch"), ("tempo", "knob"), ("hands", "switch"), ("view", "menu")],
        [("timing", "knob"), ("pitch_tol", "switch"), ("chord_tol", "switch"), ("hold", "menu")],
        [("wrong", "switch"), ("count_in", "switch"), ("metro", "switch"), ("start_mode", "menu")],
        [("err_blip", "switch"), ("remind", "menu"), ("remind_max", "knob"), ("flash", "switch")],
    ]
    kids, defs, qmap = grid_page(th, layout, top=10, row_h=150)
    return kids, defs, qmap, grid_page.last_rects

def more_page(th):
    layout = [
        [("range_lo", "knob"), ("range_hi", "knob"), ("out_range", "switch"), ("mask", "switch")],
        [("names", "switch"), ("split", "knob"), ("transpose", "knob"), ("loop_bars", "knob")],
        [("ramp", "menu"), ("goal", "knob"), None, None],
        [("listen_at", "menu"), ("listen_hands", "switch"), ("accomp", "switch"), ("out_ch", "knob")],
    ]
    kids, defs, qmap = grid_page(th, layout, top=10, row_h=150)
    return kids, defs, qmap, grid_page.last_rects

def sound_page(th):
    layout = [
        [("snd_type", "menu"), ("snd_vol", "knob"), ("snd_pitch", "knob"), ("snd_test", "button")],
        [("snd_attack", "knob"), ("snd_decay", "knob"), ("snd_sustain", "knob"), ("snd_release", "knob")],
        [("snd_length", "knob"), ("snd_remind", "knob"), ("err_blip", "switch"), ("blip_vol", "knob")],
        [("click_type", "switch"), ("click_vol", "knob"), ("metro", "switch"), ("count_in", "switch")],
    ]
    kids, defs, qmap = grid_page(th, layout, top=10, row_h=150)
    return kids, defs, qmap, grid_page.last_rects

def color_page(th):
    cols = [("col_%s" % pid, "menu") for (pid, _, _) in G.NOTE_COLOUR_DEFAULTS]
    layout = [
        [("note_colors", "switch"), ("feedback", "switch"), ("flash", "switch"), ("col_reset", "button")],
        cols[0:4], cols[4:8], cols[8:12],
    ]
    top, row_h = 10, 150
    kids, defs, qmap = grid_page(th, layout, top=top, row_h=row_h)
    sw = th.img["swatch"]
    for i in range(12):
        r, c = 1 + i // 4, i % 4
        x = COLS_X[c] + SLOT_W - 50
        y = top + r * row_h + 24 + 18
        kids.append(strip("sw%d" % i, sw[0], sw[1], "Parameter %d" % (IDX["_swatch"] + i), x, y, R.SWATCH_W, R.SWATCH_W))
    kids.extend(paragraph("help", "Note Colors puts a dot of each note's colour in every notehead still to be played. Feedback: "
                          "notes you play turn green, missed notes red, and a wrong note shows as a red note where you played it. "
                          "The colours match the LinnStrument's; black keys get the hue between their neighbours (D# a deeper blue than D).", 16, 572, 1248, 15, th.dim, 20))
    return kids, defs, qmap, grid_page.last_rects

def linn_page(th):
    layout = [
        [("linn", "switch"), ("linn_guide", "menu"), ("linn_gcol", "menu"), ("linn_finger", "switch")],
        [("linn_sect", "button"), ("linn_scol", "menu"), ("linn_clear", "button"), ("linn_key", "menu")],
        [("linn_kcol", "menu"), ("linn_sync", "button"), ("linn_size", "switch"), ("linn_rowoff", "menu")],
        [("linn_low", "knob"), ("linn_ch", "knob"), ("linn_left", "switch"), None],
    ]
    kids, defs, qmap = grid_page(th, layout, top=6, row_h=150)
    ls = th.img["linnstate"]
    x0, y0 = COLS_X[3] + 10, 6 + 3 * 150
    kids.append(label("linn_cap", "Text", "Invalid", x0, y0 + 2, SLOT_W - 20, 22, 15, th.dim, "left verticallyCentred", "SemiBold", "Upper Case", "Status"))
    kids.append(strip("linnstate", ls[0], ls[1], P("_linnstate"), x0, y0 + 28, R.LINNSTATE_W, R.LINNSTATE_H))
    kids.append(label("linn_status", "Value", P("text_linn_status"), x0, y0 + 58, SLOT_W - 20, 26, 15, th.dim))
    kids.append(label("mpc_key", "Value", P("text_mpc_key"), x0, y0 + 86, SLOT_W - 20, 26, 15, th.text))
    return kids, defs, qmap, grid_page.last_rects

def stats_page(th):
    kids = []
    kids.append(label("title", "Value", P("text_title"), 16, 6, 900, 36, 25, th.text, "left verticallyCentred", "SemiBold"))
    dg = th.img["digits"]
    def digits(group, n, x, y, cap):
        kids.append(label(group + "_cap", "Text", "Invalid", x, y - 20, 160, 20, 15, th.dim, "left verticallyCentred", "SemiBold", "Upper Case", cap))
        for i in range(n):
            kids.append(strip(group, dg[0], dg[1], "Parameter %d" % (IDX["_" + group] + i), x + i * R.DIGIT_W, y, R.DIGIT_W, R.DIGIT_H))
    digits("d_best", 3, 16, 80, "Best %")
    digits("d_last", 3, 176, 80, "Last %")
    digits("d_passes", 3, 336, 80, "Passes")
    digits("d_minutes", 4, 496, 80, "Minutes")
    digits("d_streak", 3, 656, 80, "Day streak")
    sb = th.img["statbar"]
    for i in range(16):
        kids.append(strip("sbar", sb[0], sb[1], "Parameter %d" % (IDX["_statbar"] + i), 16 + i * 62, 140, R.STATBAR_W, R.STATBAR_H))
        kids.append(label("sbar_cap%d" % i, "Text", "Invalid", 16 + i * 62, 362, R.STATBAR_W, 18, 13, th.dim, "horizontallyCentred verticallyCentred", text=str(i + 1)))
    for i in range(4):
        kids.append(label("stats%d" % i, "Value", P("text_stats%d" % i), 16, 392 + i * 34, 1248, 32, 18, th.text if i < 2 else th.dim))
    return kids, {}, {}, [rect(0, 0, 0, 0)] * 4

# (tab name per theme, builder, themes). Only PLAY has a Day version: every extra tab shows on the
# MPC as its own button, and the settings pages read fine in the MPC's own dark style.
PAGES = [(("PLAY", "LIGHT"), play_page, ["night", "day"]), (("LOAD",), load_page, ["night"]), (("GEN",), gen_page, ["night"]),
         (("SETUP",), setup_page, ["night"]), (("MORE",), more_page, ["night"]), (("SOUND",), sound_page, ["night"]),
         (("COLOR",), color_page, ["night"]), (("LINN",), linn_page, ["night"]), (("STATS",), stats_page, ["night"])]
THEME_ORDER = ["night", "day"]

# ---------------------------------------------------------------------------------------------
def main():
    os.makedirs(SKINS, exist_ok=True)
    os.makedirs(GEN_DIR, exist_ok=True)
    G.write_cpp_header(os.path.join(GEN_DIR, "layout_gen.h"))
    PR.write_cpp_header(os.path.join(GEN_DIR, "params_gen.h"))
    for f in os.listdir(SKINS):
        if f.endswith(".png") or f.endswith(".json"):
            os.remove(os.path.join(SKINS, f))
    themes = {}
    for name in THEME_ORDER:
        meta = R.render_theme(name, SKINS)
        meta["staff"] = ("%s_staff.png" % name,)
        meta.update(R.ui_images(name, SKINS))
        themes[name] = Theme(name, meta)

    local = overlay_defs()
    defs_all = {}
    tabs, qentries = [], []
    for ti, (names, builder, page_themes) in enumerate(PAGES):
        for si, thname in enumerate(page_themes):
            th = themes[thname]
            tname = names[min(si, len(names) - 1)]
            kids, defs, qmap, rects = builder(th)
            defs_all.update(defs)
            if builder is play_page:
                defs_all["%s_tap" % thname] = container([], actions=[action("Mouse Down", "Toggle Switch", "Data")])
            bg = th.bg if builder is play_page else (th.bg if thname == "day" else th.panel)
            comp = container(kids, bg=bg)
            tabs.append(o(("version", 1), ("tabName", tname), ("fnKeyIndex", ti), ("fnKeySubIndex", si),
                          ("componentDefinition", comp), ("qlinkBoundsData", rects)))
            ql = O([("Q-Link %d" % q, qmap.get(q, -1)) for q in range(1, 17)])
            qentries.append(o(("Tab", ti + 1), ("SubTab", si + 1), ("Bank Direction", "RowTopDown"), ("Q-Links", ql)))
    for k, v in sorted(defs_all.items()):
        local.append(o(("key", k), ("value", v)))
    # discrete steps for every choice and switch parameter
    repl = []
    for (pid, name, kind, rng, unit) in PR.CONTROL:
        if kind == "choice":
            repl.append(o(("key", P(pid)), ("value", list(rng[0]))))
        elif kind in ("bool", "trigger"):
            repl.append(o(("key", P(pid)), ("value", ["Off", "On"])))
    tui = o(("pageData", o(("version", 1),
                           ("componentDefinitions", o(("version", 2), ("importFiles", []), ("localComponentDefinitions", local))),
                           ("replacementStrings", repl),
                           ("info", o(("version", 1), ("type", "CompleteDescription"))),
                           ("tabs", tabs))))
    with open(os.path.join(SKINS, "TUI.json"), "w") as f:
        f.write(dump(tui))
    program = ["tempo", "mode", "hands", "view", "loop_from", "loop_bars", "loop", "play",
               "timing", "pitch_tol", "chord_tol", "hold", "snd_vol", "metro", "count_in", "mask"]
    # Program mode: Q-Link 13..16 top row, like the screen
    order = [13, 14, 15, 16, 9, 10, 11, 12, 5, 6, 7, 8, 1, 2, 3, 4]
    pm = O([("Q-Link %d" % q, IDX[program[order.index(q)]]) for q in range(1, 17)])
    ql = o(("version", 4), ("info", o(("version", 1), ("type", "CompleteDescription"))),
           ("Screen Mode Q-Links", o(("version", 4), ("map", qentries))),
           ("Program Mode Q-Links", pm))
    with open(os.path.join(SKINS, "Q-Links.json"), "w") as f:
        f.write(dump(ql))
    # images no page uses (a theme's copy of a page that has no such theme)
    tui_text = open(os.path.join(SKINS, "TUI.json")).read()
    for f in os.listdir(SKINS):
        if f.endswith(".png") and f not in tui_text:
            os.remove(os.path.join(SKINS, f))
    total = sum(os.path.getsize(os.path.join(SKINS, f)) for f in os.listdir(SKINS))
    print("skins: %d files, %.1f KB (TUI.json %.0f KB), %d parameters" % (len(os.listdir(SKINS)), total / 1024,
          os.path.getsize(os.path.join(SKINS, "TUI.json")) / 1024, IDX["_total"]))

if __name__ == "__main__":
    main()
