#!/usr/bin/env python3
"""Does the LinnStrument take light messages at all? A test from a Mac with nothing to install.

    python3 linn_test_mac.py            # uses the MIDI destination with "LinnStrument" in its name
    python3 linn_test_mac.py "Linn"     # or the one whose name contains this text

Plug the LinnStrument into the Mac by USB (its Global Settings: MIDI I/O = USB) and quit
anything else that uses it. The script talks to macOS's own MIDI system (CoreMIDI) through
Python's built-in ctypes, so it needs no Xcode, no Homebrew packages and no pip installs: the
python3 that comes with the Command Line Tools (which Homebrew already installed) is enough.

It sends what Sight Reader sends, slowly, and says what should light:
  1. Cell paints (CC 20 column, CC 21 row, CC 22 colour): a row of red cells for 3 seconds,
     then set back to colour 7 (off). If your LinnStrument shows a custom light pattern, those
     cells may stay dark afterwards; switching it off and on puts the pattern back.
  2. Note On middle C on channel 1, then on channel 2: every middle-C cell in your Played
     colour, 3 seconds each (channel 1 answers in One Channel mode, channel 2 in MPE mode).
"""
import ctypes
import subprocess
import sys
import time

try:
    CM = ctypes.CDLL("/System/Library/Frameworks/CoreMIDI.framework/CoreMIDI")
    CF = ctypes.CDLL("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation")
except OSError:
    sys.exit("This needs macOS (CoreMIDI).")

UTF8 = 0x08000100                       # kCFStringEncodingUTF8
u32, ulong, vp = ctypes.c_uint32, ctypes.c_ulong, ctypes.c_void_p


def fn(lib, name, res, *args):
    f = getattr(lib, name)
    f.restype = res
    f.argtypes = list(args)
    return f


CFStringCreateWithCString = fn(CF, "CFStringCreateWithCString", vp, vp, ctypes.c_char_p, u32)
CFStringGetCString = fn(CF, "CFStringGetCString", ctypes.c_bool, vp, vp, ctypes.c_long, u32)
CFRelease = fn(CF, "CFRelease", None, vp)
MIDIClientCreate = fn(CM, "MIDIClientCreate", ctypes.c_int32, vp, vp, vp, ctypes.POINTER(u32))
MIDIOutputPortCreate = fn(CM, "MIDIOutputPortCreate", ctypes.c_int32, u32, vp, ctypes.POINTER(u32))
MIDIGetNumberOfDestinations = fn(CM, "MIDIGetNumberOfDestinations", ulong)
MIDIGetDestination = fn(CM, "MIDIGetDestination", u32, ulong)
MIDIObjectGetStringProperty = fn(CM, "MIDIObjectGetStringProperty", ctypes.c_int32, u32, vp, ctypes.POINTER(vp))
MIDIPacketListInit = fn(CM, "MIDIPacketListInit", vp, vp)
MIDIPacketListAdd = fn(CM, "MIDIPacketListAdd", vp, vp, ulong, vp, ctypes.c_uint64, ulong, vp)
MIDISend = fn(CM, "MIDISend", ctypes.c_int32, u32, u32, vp)
MIDIGetNumberOfSources = fn(CM, "MIDIGetNumberOfSources", ulong)
MIDIGetNumberOfDevices = fn(CM, "MIDIGetNumberOfDevices", ulong)
MIDIGetDevice = fn(CM, "MIDIGetDevice", u32, ulong)
MIDIObjectGetIntegerProperty = fn(CM, "MIDIObjectGetIntegerProperty", ctypes.c_int32, u32, vp, ctypes.POINTER(ctypes.c_int32))
CFRunLoopRunInMode = fn(CF, "CFRunLoopRunInMode", ctypes.c_int32, vp, ctypes.c_double, ctypes.c_bool)
kMIDIPropertyDisplayName = vp.in_dll(CM, "kMIDIPropertyDisplayName").value
kMIDIPropertyName = vp.in_dll(CM, "kMIDIPropertyName").value
kMIDIPropertyOffline = vp.in_dll(CM, "kMIDIPropertyOffline").value
kCFRunLoopDefaultMode = vp.in_dll(CF, "kCFRunLoopDefaultMode").value


def cfstr(text):
    return CFStringCreateWithCString(None, text.encode("utf-8"), UTF8)


def name_of(obj):
    for prop in (kMIDIPropertyDisplayName, kMIDIPropertyName):
        s = vp()
        if MIDIObjectGetStringProperty(obj, prop, ctypes.byref(s)) != 0 or not s.value:
            continue
        buf = ctypes.create_string_buffer(512)
        ok = CFStringGetCString(s.value, buf, 512, UTF8)
        CFRelease(s.value)
        if ok and buf.value:
            return buf.value.decode("utf-8", "replace")
    return "(no name)"


def offline(obj):
    v = ctypes.c_int32(0)
    return MIDIObjectGetIntegerProperty(obj, kMIDIPropertyOffline, ctypes.byref(v)) == 0 and v.value != 0


def usb_report():
    """What the Mac's USB sees, whatever MIDI thinks: the device tree from ioreg."""
    try:
        out = subprocess.run(["ioreg", "-p", "IOUSB", "-w0"], capture_output=True, text=True, timeout=10).stdout
    except Exception as e:                # noqa: BLE001
        return "  (could not run ioreg: %s)" % e
    lines = [ln.split("+-o", 1)[1].split("<", 1)[0].strip() for ln in out.splitlines() if "+-o" in ln]
    lines = [ln for ln in lines if ln and not ln.startswith("Root")]
    return "\n".join("  - " + ln for ln in lines) if lines else "  (no USB devices listed)"


def main():
    want = (sys.argv[1] if len(sys.argv) > 1 else "linnstrument").lower()
    client, port = u32(), u32()
    err = MIDIClientCreate(cfstr("LinnStrument light test"), None, None, ctypes.byref(client))
    if err:
        sys.exit("MIDIClientCreate failed (%d)" % err)
    err = MIDIOutputPortCreate(client.value, cfstr("out"), ctypes.byref(port))
    if err:
        sys.exit("MIDIOutputPortCreate failed (%d)" % err)
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, False)   # let the MIDI server finish its setup

    devices = [MIDIGetDevice(i) for i in range(MIDIGetNumberOfDevices())]
    print("MIDI devices macOS knows (as in Audio MIDI Setup > MIDI Studio):")
    for d in devices:
        print("  - %s%s" % (name_of(d), "   [not connected now]" if offline(d) else ""))
    if not devices:
        print("  (none)")
    dests = [MIDIGetDestination(i) for i in range(MIDIGetNumberOfDestinations())]
    names = [name_of(d) for d in dests]
    print("MIDI destinations (what can be sent to): %d" % len(dests))
    for n in names:
        print("  -", n)
    print("MIDI sources (what can send): %d" % MIDIGetNumberOfSources())
    found = [(d, n) for d, n in zip(dests, names) if want in n.lower()]
    if not found:
        print("\nUSB devices the Mac sees:")
        print(usb_report())
        sys.exit("\nNo MIDI destination with %r in its name. If the LinnStrument is not in the USB list "
                 "either, the Mac is not talking to it over USB: try another cable (some only charge), "
                 "plug it straight into the Mac, and look for an 'Allow accessory to connect?' prompt "
                 "(System Settings > Privacy & Security). If it is in the USB list but not in MIDI, "
                 "copy all of this and send it over." % want)
    dest, dest_name = found[0]
    print("\nSending to: %s\n" % dest_name)

    def send(*msg):
        buf = ctypes.create_string_buffer(256)
        pkt = MIDIPacketListInit(buf)
        data = (ctypes.c_ubyte * len(msg))(*msg)
        if not MIDIPacketListAdd(buf, 256, pkt, 0, len(msg), data):
            sys.exit("Could not build a MIDI packet")
        e = MIDISend(port.value, dest, buf)
        if e:
            print("  MIDISend error %d" % e)
        time.sleep(0.004)                 # slow and steady, one message at a time

    row = 3
    try:
        print("1. Cell paints: row %d of the surface turns RED for 3 seconds..." % (row + 1))
        for col in range(1, 26):
            send(0xB0, 20, col)
            send(0xB0, 21, row)
            send(0xB0, 22, 1)             # red
        time.sleep(3)
        for col in range(1, 26):
            send(0xB0, 20, col)
            send(0xB0, 21, row)
            send(0xB0, 22, 7)             # off
        time.sleep(1)
        for ch in (1, 2):
            print("2. Note On middle C, channel %d: the middle-C cells light in your Played colour for 3 seconds..." % ch)
            send(0x90 | (ch - 1), 60, 100)
            time.sleep(3)
            send(0x80 | (ch - 1), 60, 0)
            time.sleep(1)
    except KeyboardInterrupt:
        for ch in (1, 2):
            send(0x80 | (ch - 1), 60, 0)
        print("\nStopped.")
        return
    print("\nDone. Which lit: the red row (1), middle C on channel 1 or 2 (2), both, or nothing?")


if __name__ == "__main__":
    main()
