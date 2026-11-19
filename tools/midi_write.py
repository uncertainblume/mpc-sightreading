"""Minimal Standard MIDI File writer shared by the drill and library tools (no dependencies)."""
import struct

def vlq(v):
    out = [v & 0x7F]
    v >>= 7
    while v:
        out.append(0x80 | (v & 0x7F))
        v >>= 7
    return bytes(reversed(out))

def meta(t, kind, data):
    return vlq(t) + bytes([0xFF, kind]) + vlq(len(data)) + data

def write_midi(path, tracks, ppq=96, bpm=90, ts=(4, 4), key=0, minor=False, title=""):
    """tracks: list of (name, channel, [(start_tick, end_tick, pitch, velocity), ...])."""
    def chunk(body):
        return b"MTrk" + struct.pack(">I", len(body)) + body
    den_pow = {1: 0, 2: 1, 4: 2, 8: 3, 16: 4}[ts[1]]
    us = int(60000000 / bpm)
    conductor = (meta(0, 0x03, title.encode("utf-8")[:120]) +
                 meta(0, 0x51, struct.pack(">I", us)[1:]) +
                 meta(0, 0x58, bytes([ts[0], den_pow, 24, 8])) +
                 meta(0, 0x59, struct.pack(">bB", key, 1 if minor else 0)) +
                 meta(0, 0x2F, b""))
    body = [chunk(conductor)]
    for name, ch, notes in tracks:
        evs = []
        for (a, b, p, v) in notes:
            evs.append((b, 0, bytes([0x80 | ch, p, 64])))
            evs.append((a, 1, bytes([0x90 | ch, p, v])))
        evs.sort(key=lambda e: (e[0], e[1]))
        data = meta(0, 0x03, name.encode("utf-8"))
        last = 0
        for (t, _, msg) in evs:
            data += vlq(t - last) + msg
            last = t
        data += meta(0, 0x2F, b"")
        body.append(chunk(data))
    header = b"MThd" + struct.pack(">IHHH", 6, 1, len(body), ppq)
    with open(path, "wb") as f:
        f.write(header + b"".join(body))
