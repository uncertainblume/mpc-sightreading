"""Draws a page of a TUI.json the way the MPC would (approximately), from a dump of the
plugin's parameter values and texts. For checking layouts on a computer.

    python3 tools/skin/render_page.py plugin/skins/TUI.json state.json PLAY 0 out.png

state.json (tests/plugin_test writes one): {"values": [...], "text": {"index": "display"}, "name": {"index": "name"}}
Not handled: overlays, focus rings, the MPC's own chrome (tab buttons, header). Entries hidden by
IndexedEnabling are left out. --set INDEX=VALUE (normalised) overrides a value, e.g. to draw the
piece list open.
"""
import json, os, sys
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
FONTS = os.path.join(HERE, "fonts")

def font(style, size):
    f = {"Bold": "TitilliumWeb-Bold.ttf", "SemiBold": "TitilliumWeb-SemiBold.ttf"}.get(style, "TitilliumWeb-Regular.ttf")
    return ImageFont.truetype(os.path.join(FONTS, f), max(6, int(size)))

def colour(s):
    if not s or s == "0":
        return (0, 0, 0, 0)
    v = int(s, 16)
    return ((v >> 16) & 255, (v >> 8) & 255, v & 255, (v >> 24) & 255)

class Ctx:
    def __init__(self, tui, state, skin_dir):
        self.defs = {d["key"]: d["value"] for d in tui["pageData"]["componentDefinitions"]["localComponentDefinitions"]}
        self.values = state["values"]
        self.text = {int(k): v for k, v in state.get("text", {}).items()}
        self.name = {int(k): v for k, v in state.get("name", {}).items()}
        self.repl = {r["key"]: r["value"] for r in tui["pageData"].get("replacementStrings", [])}
        self.dir = skin_dir
        self.cache = {}
    def img(self, name):
        if name not in self.cache:
            p = os.path.join(self.dir, name)
            self.cache[name] = Image.open(p).convert("RGBA") if os.path.exists(p) else None
        return self.cache[name]
    def param(self, handle, remap):
        h = handle
        for _ in range(4):
            if h in remap:
                h = remap[h]
            else:
                break
        if h.startswith("Parameter "):
            return int(h.split()[1])
        return None

def strip_frame(im, n, v):
    h = im.size[1] // n
    k = max(0, min(n - 1, int(v * (n - 1) + 1e-4)))
    return im.crop((0, k * h, im.size[0], (k + 1) * h))

def draw_text(page, box, txt, ts):
    if not txt:
        return
    x, y, w, h = box
    f = font(ts["font"]["style"], ts["font"]["height"] * 0.95)
    if ts.get("case") == "Upper Case":
        txt = txt.upper()
    just = ts.get("justification", "left")
    d = ImageDraw.Draw(page)
    tw = d.textlength(txt, font=f)
    while tw > w and len(txt) > 1:
        txt = txt[:-2] + "~"; tw = d.textlength(txt, font=f)
    tx = x + (w - tw) / 2 if "horizontallyCentred" in just else (x + w - tw if "right" in just else x)
    ty = y + h / 2 if "verticallyCentred" in just else (y if "top" in just else y + h)
    anchor = "lm" if "verticallyCentred" in just else ("lt" if "top" in just else "ls")
    d.text((tx, ty), txt, font=f, fill=colour(ts["colour"]), anchor=anchor)

def visible(ctx, b):
    """additionalInvalidatingHandles: IndexedEnabling/<i>/<n>/<handle> hides the entry unless the
    handle is at step i of n."""
    for h in b.get("additionalInvalidatingHandles", []):
        parts = h.split("/", 3)
        if parts[0] == "IndexedEnabling" and len(parts) == 4 and parts[3].startswith("Parameter "):
            i, n, p = int(parts[1]), int(parts[2]), int(parts[3].split()[1])
            if int(round(ctx.values[p] * (n - 1))) != i:
                return False
    return True

def render(ctx, comps, page, ox, oy, remap):
    for e in comps:
        cd = e["componentData"]; typ = cd["type"]; data = cd["data"]
        b = e["bounds"]
        if not visible(ctx, b):
            continue
        x, y, w, h = [float(v) for v in b["bounds"].split()]
        x += ox; y += oy
        local = dict(remap)
        for m in e.get("handle remapping", {}).get("map", []):
            local[m["key"]] = remap.get(m["value"], m["value"])
        if typ in ctx.defs:
            sub = ctx.defs[typ]
            bg = colour(sub["backgroundData"]["unfocussed"]["colour"])
            if bg[3]:
                ImageDraw.Draw(page).rectangle([x, y, x + w - 1, y + h - 1], fill=bg)
            render(ctx, sub["componentsData"], page, x, y, local)
            continue
        if typ == "Image":
            im = ctx.img(data["image"])
            if im is None:
                continue
            if data.get("imageType") == "FilmStrip":
                p = ctx.param(data["handleName"], local)
                v = ctx.values[p] if p is not None else 0
                im = strip_frame(im, data["numFrames"], v)
            page.alpha_composite(im.resize((int(w), int(h))) if im.size != (int(w), int(h)) else im, (int(x), int(y)))
        elif typ == "Knob" and data.get("knobType") == "FilmStrip":
            im = ctx.img(data["filmStrip"]); p = ctx.param(data["handleName"], local)
            if im is not None:
                page.alpha_composite(strip_frame(im, data["numFrames"], ctx.values[p] if p is not None else 0), (int(x), int(y)))
        elif typ == "Button":
            p = ctx.param(data["handleName"], local)
            v = ctx.values[p] if p is not None else 0
            n = max(2, data["numButtonsInGroup"])
            step = int(round(v * (n - 1)))
            on = step == data["buttonId"]
            im = ctx.img(data["onImage"] if on else data["offImage"])
            if im is not None:
                page.alpha_composite(im, (int(x + (w - im.size[0]) / 2), int(y + (h - im.size[1]) / 2)))
        elif typ == "Label":
            p = ctx.param(data["handleName"], local)
            if data["type"] == "Text":
                txt = data.get("text", "")
            elif data["type"] == "Name":
                txt = ctx.name.get(p, "")
            else:
                key = "Parameter %d" % p if p is not None else None
                if key in ctx.repl:
                    vals = ctx.repl[key]
                    txt = vals[int(round(ctx.values[p] * (len(vals) - 1)))]
                else:
                    txt = ctx.text.get(p, "")
            draw_text(page, (x, y, w, h), txt, data["textStyle"])
        elif typ == "Slider" and data.get("sliderType") == "ColourBar":
            p = ctx.param(data["handleName"], local)
            v = ctx.values[p] if p is not None else 0
            d = ImageDraw.Draw(page)
            d.rectangle([x, y, x + w - 1, y + h - 1], fill=colour(data["backgroundColour"]))
            if v > 0:
                d.rectangle([x, y, x + w * v - 1, y + h - 1], fill=colour(data["valueColour"]))

def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--set")]
    tui_path, state_path, tab, sub, out = args[:5]
    tui = json.load(open(tui_path))
    state = json.load(open(state_path))
    for i, a in enumerate(sys.argv):
        if a == "--set":
            k, v = sys.argv[i + 1].split("=")
            state["values"][int(k)] = float(v)
    args = [a for a in args if "=" not in a]
    ctx = Ctx(tui, state, os.path.dirname(os.path.abspath(tui_path)))
    t = next(t for t in tui["pageData"]["tabs"] if t["tabName"] == tab and t["fnKeySubIndex"] == int(sub))
    comp = t["componentDefinition"]
    page = Image.new("RGBA", (1280, 628), colour(comp["backgroundData"]["unfocussed"]["colour"]))
    render(ctx, comp["componentsData"], page, 0, 0, {})
    page.convert("RGB").save(out)
    print(out)

if __name__ == "__main__":
    main()
