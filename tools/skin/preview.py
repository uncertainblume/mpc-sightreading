"""Composes a picture of the PLAY page from a display dump, using the same film strips and
coordinates as the skin. Lets the notation be checked on a computer, without an MPC.

    python3 tools/skin/preview.py dump.json out.png [--theme day|night] [--skin DIR]

dump.json (written by tests/sr_test --dump):
  {"cells": [[frame per row] per slot], "barlines": [frame per slot boundary (slots+1)],
   "overlays": [frame per slot], "keysig": sf, "timesig": frame, "masks": [f0, f1],
   "flash": f, "marks": [[mark frame per row] per slot] (optional)}
"""
import json, os, sys
from PIL import Image

import geometry as G
import render as R

def strip_frame(img, n, i):
    h = img.size[1] // n
    i = max(0, min(n - 1, i))
    return img.crop((0, i * h, img.size[0], (i + 1) * h))

def compose(dump, theme, skin_dir):
    t = R.THEMES[theme]
    os.makedirs(skin_dir, exist_ok=True)
    if not os.path.exists(os.path.join(skin_dir, "%s_staff.png" % theme)):
        R.render_theme(theme, skin_dir)
    load = lambda n: Image.open(os.path.join(skin_dir, n)).convert("RGBA")
    page = Image.new("RGBA", (G.TAB_W, G.TAB_H), R.rgba(t["paper"]))
    cells = {cls: load("%s_cells_%s.png" % (theme, G.CLASS_NAMES[cls])) for cls in (G.SPACE, G.LINE, G.LEDGER)}
    counts = {cls: len(G.CATALOGUES[cls]) for cls in cells}
    for s, col in enumerate(dump["cells"]):
        x = G.SLOTS_X0 + s * G.SLOT_W
        for r, fr in enumerate(col):
            if fr == 0:
                continue
            cls = G.row_class(r)
            im = strip_frame(cells[cls], counts[cls], fr)
            page.alpha_composite(im, (x, G.row_y(r) - G.CELL_H // 2))
    ov = load("%s_overlay.png" % theme)
    for s, fr in enumerate(dump.get("overlays", [])):
        if fr:
            page.alpha_composite(strip_frame(ov, len(G.OVERLAY_FRAMES), fr), (G.SLOTS_X0 + s * G.SLOT_W, G.GRID_TOP))
    for staff, fr in enumerate(dump.get("masks", [0, 0])):
        if fr:
            m = load("%s_mask%d.png" % (theme, staff))
            y0 = G.GRID_TOP if staff == 0 else R.MID_Y
            page.alpha_composite(strip_frame(m, len(G.STAFFMASK_FRAMES), fr), (G.SLOTS_X0, y0))
    page.alpha_composite(load("%s_staff.png" % theme), (0, 0))
    if dump.get("marks"):
        mk = load("%s_marks.png" % theme)
        for s, col in enumerate(dump["marks"]):
            x = G.SLOTS_X0 + s * G.SLOT_W
            for r, fr in enumerate(col):
                if fr:
                    page.alpha_composite(strip_frame(mk, len(G.MARK_FRAMES), fr), (x, G.row_y(r) - G.CELL_H // 2))
    bl = load("%s_barline.png" % theme)
    for s, fr in enumerate(dump.get("barlines", [])):
        if fr:
            page.alpha_composite(strip_frame(bl, len(G.BARLINE_FRAMES), fr), (G.SLOTS_X0 + s * G.SLOT_W - R.BAR_W // 2, R.BAR_Y0))
    for staff in (0, 1):
        ks = load("%s_keysig%d.png" % (theme, staff))
        top = (G.TREBLE_LINES_Y if staff == 0 else G.BASS_LINES_Y)[0] - 34
        page.alpha_composite(strip_frame(ks, len(G.KEYSIG_FRAMES), dump.get("keysig", 0) + 7), (R.KS_X, top))
        ts = load("%s_timesig%d.png" % (theme, staff))
        ttop = (G.TREBLE_LINES_Y if staff == 0 else G.BASS_LINES_Y)[0] - 4
        page.alpha_composite(strip_frame(ts, len(G.TIMESIG_FRAMES), dump.get("timesig", 0)), (R.TS_X, ttop))
    if dump.get("flash"):
        fl = R.flash_strip(theme, G.TAB_W, G.GRID_BOTTOM - G.GRID_TOP)
        page.alpha_composite(strip_frame(fl, len(G.FLASH_FRAMES), dump["flash"]), (0, G.GRID_TOP))
    return page

if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    theme = "night"
    skin = "/tmp/sr_skin_preview"
    for i, a in enumerate(sys.argv):
        if a == "--theme": theme = sys.argv[i + 1]
        if a == "--skin": skin = sys.argv[i + 1]
    dump = json.load(open(args[0]))
    screens = dump["screens"] if "screens" in dump else [dump]
    for k, scr in enumerate(screens):
        out = args[1] if len(screens) == 1 else args[1].replace(".png", "_%02d.png" % k)
        compose(scr, theme, skin).convert("RGB").save(out)
        print(out)
