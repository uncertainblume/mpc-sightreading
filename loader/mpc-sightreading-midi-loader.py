#!/usr/bin/env python3
"""mpc-sightreading-midi-loader: put your MIDI files on the MPC for Sight Reader, from a page in your browser.

    python3 loader/mpc-sightreading-midi-loader.py      (on a Mac you can double-click the .command file)

It opens a page where you drop .mid files into collections. The library lives on this computer
(by default the library/cache folder that tools/library.py uses too), and Sync makes the MPC's
SightReader/Library match it over SSH: new and changed pieces are copied, pieces you removed
here are removed there. All the work happens on this computer; the MPC only sees an SSH login,
one archive to unpack and a few files to delete. Sight Reader 0.6.0 and later notice the change
by themselves within a couple of seconds. The page also restarts the MPC application.

The page is served at http://127.0.0.1:<port>/, which only this computer can open. The MPC's
address and the other settings are remembered in your user settings folder, never in the
repository:
    macOS    ~/Library/Application Support/mpc-sightreading-midi-loader/
    Linux    ~/.config/mpc-sightreading-midi-loader/
    Windows  %APPDATA%\\mpc-sightreading-midi-loader\\

Options:
    --port N        the page's port on this computer (default 47823; remembered)
    --library DIR   use this folder as the library (remembered; also settable on the page)
    --no-browser    do not open the browser

The MPC must be in developer mode, which starts its SSH server (see the README). Needs Python 3.8
or later and the ssh command, which macOS, Linux and Windows 10/11 have. Standard library only.

SPDX-License-Identifier: MIT
"""
import argparse
import hashlib
import hmac
import http.server
import io
import json
import os
import re
import secrets
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
import unicodedata
import urllib.parse
import urllib.request
import webbrowser

APP = "mpc-sightreading-midi-loader"
VERSION = "0.6.0"
DEFAULT_PORT = 47823
MPC_SERVICE = "acvs"                # the systemd unit that runs the MPC application
MAX_MIDI = 8 << 20                  # Sight Reader reads files up to 8 MB
RESERVED = ("generated", "playlist")  # folder names Sight Reader's LOAD list uses already
AUTO_RESCAN_SINCE = (0, 6, 0)       # the plugin version that watches Library/.rescan
# Where MPCs mount their drives. The library goes on the drive Sight Reader is installed on,
# because that is where the plugin looks (<drive>/SightReader/Library).
DRIVES = os.environ.get("MPC_SR_DRIVES") or "/nvme /media/* /run/media/*/* /mnt/*"
INTERNAL = ("nvme", "az01-internal", "acvs-content", "acvs-synths")
HERE = os.path.dirname(os.path.abspath(__file__))


# ----------------------------------------------------------------------------------------------
# Names, index files and MIDI files
# ----------------------------------------------------------------------------------------------

_FOLD = {"ß": "ss", "æ": "ae", "Æ": "AE", "ø": "o", "Ø": "O", "œ": "oe", "Œ": "OE", "ł": "l", "Ł": "L",
         "đ": "d", "Đ": "D", "þ": "th", "Þ": "Th", "ð": "d", "ı": "i", "‘": "'", "’": "'", "‚": "'",
         "“": '"', "”": '"', "„": '"', "–": "-", "—": "-", "…": "...", "♯": "#", "♭": "b", "×": "x"}
_UNSAFE = re.compile(r"[^A-Za-z0-9 ._()&,'#+-]+")


def fold(text):
    """Plain ASCII for the MPC's screen: accents go (Für Elise -> Fur Elise), other symbols are left out."""
    text = "".join(_FOLD.get(c, c) for c in str(text))
    text = unicodedata.normalize("NFKD", text)
    return "".join(c for c in text if 32 <= ord(c) < 127)


def clean_field(text, limit=80):
    return re.sub(r"\s+", " ", fold(text)).strip()[:limit]


def safe_name(name, limit=64):
    """A file or folder name that is safe everywhere: letters, digits and a few signs."""
    s = _UNSAFE.sub("_", fold(name))
    s = re.sub(r"_+", "_", re.sub(r"\s+", " ", s)).strip(" ._")
    return s[:limit].rstrip(" .")


def midi_file_name(name):
    stem, ext = os.path.splitext(os.path.basename(name))
    ext = ext.lower() if ext.lower() in (".mid", ".midi") else ".mid"
    return (safe_name(stem, 72) or "Untitled") + ext


def is_midi_name(name):
    return not name.startswith(".") and name.lower().endswith((".mid", ".midi"))


def title_from(name):
    return clean_field(os.path.splitext(os.path.basename(name))[0].replace("_", " "))


def is_midi(data):
    return data[:4] == b"MThd" or (data[:4] == b"RIFF" and b"MThd" in data[:64])


def md5_of(data):
    try:
        return hashlib.md5(data, usedforsecurity=False).hexdigest()
    except TypeError:                       # Python 3.8
        return hashlib.md5(data).hexdigest()


def read_index_text(text):
    """index.tsv rows by file name: [file, title, composer, level, source] (the format library.py writes)."""
    rows = {}
    for line in (text or "").splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        f = (line.rstrip("\r").split("\t") + [""] * 5)[:5]
        rows[f[0]] = f
    return rows


def index_text(rows):
    out = ["# file\ttitle\tcomposer\tlevel\tsource"]
    for name in sorted(rows):
        r = (list(rows[name]) + [""] * 5)[:5]
        r[0] = name
        out.append("\t".join(re.sub(r"[\t\r\n]+", " ", str(x)) for x in r))
    return "\n".join(out) + "\n"


def same_index(a, b):
    return read_index_text(a) == read_index_text(b)


NOTE_NAMES = ["C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"]


def note_name(n):
    return "%s%d" % (NOTE_NAMES[n % 12], n // 12 - 1)


def midi_info(data):
    """What a Standard MIDI File holds: tracks, notes, length and range. ValueError if it is not one."""
    if data[:4] == b"RIFF":                  # RMID: a MIDI file inside a RIFF wrapper
        k = data.find(b"MThd", 0, 64)
        data = data[k:] if k >= 0 else b""
    if data[:4] != b"MThd" or len(data) < 14:
        raise ValueError("not a MIDI file")
    be16 = lambda i: (data[i] << 8) | data[i + 1]                                         # noqa: E731
    be32 = lambda i: (data[i] << 24) | (data[i + 1] << 16) | (data[i + 2] << 8) | data[i + 3]  # noqa: E731
    fmt, div = be16(8), be16(12)
    pos, n = 8 + be32(4), len(data)
    tempos, names, chans = [], [], set()
    notes, lo, hi, end, tracks, problem = 0, 128, -1, 0, 0, ""
    while pos + 8 <= n:
        cid, clen = data[pos:pos + 4], be32(pos + 4)
        i, pos = pos + 8, pos + 8 + clen
        if cid != b"MTrk":
            continue
        tracks += 1
        stop = min(pos, n)
        if pos > n:
            problem = "the file is shorter than its header says"
        tick, status, name = 0, 0, ""
        try:
            while i < stop:
                d = 0
                while True:
                    c = data[i]; i += 1
                    d = (d << 7) | (c & 0x7F)
                    if c < 0x80:
                        break
                tick += d
                if data[i] >= 0x80:
                    status = data[i]; i += 1
                elif status == 0:
                    raise IndexError
                if status == 0xFF or status in (0xF0, 0xF7):
                    mtype = data[i] if status == 0xFF else -1
                    if status == 0xFF:
                        i += 1
                    ln = 0
                    while True:
                        c = data[i]; i += 1
                        ln = (ln << 7) | (c & 0x7F)
                        if c < 0x80:
                            break
                    payload = data[i:i + ln]; i += ln
                    if mtype == 0x51 and ln == 3:
                        tempos.append((tick, (payload[0] << 16) | (payload[1] << 8) | payload[2]))
                    elif mtype == 0x03 and not name:
                        name = payload.decode("latin-1").strip()
                    elif mtype == 0x2F:
                        break
                    status = 0                  # meta and sysex events end running status
                    continue
                kind = status & 0xF0
                if kind in (0xC0, 0xD0):
                    i += 1
                else:
                    key, vel = data[i], data[i + 1]; i += 2
                    if kind == 0x90 and vel > 0:
                        notes += 1
                        lo, hi = min(lo, key), max(hi, key)
                        chans.add(status & 0x0F)
                end = max(end, tick)
        except IndexError:
            problem = problem or "a track is cut short"
        names.append(clean_field(name, 40))
    if div & 0x8000:                         # SMPTE time
        fps, tpf = 256 - (div >> 8), div & 0xFF
        seconds = end / float(fps * tpf) if fps > 0 and tpf else 0.0
    else:
        tpq, seconds, last, us = (div or 480), 0.0, 0, 500000
        for t, u in sorted(tempos):
            if t > end:
                break
            seconds += (t - last) * us / 1e6 / tpq
            last, us = t, u
        seconds += (end - last) * us / 1e6 / tpq
    return {"format": fmt, "tracks": tracks, "notes": notes, "seconds": round(seconds, 1),
            "low": note_name(lo) if hi >= 0 else "", "high": note_name(hi) if hi >= 0 else "",
            "drums_only": bool(chans) and chans == {9}, "names": [x for x in names if x][:6], "problem": problem}


def version_tuple(text):
    m = re.match(r"\s*(\d+)\.(\d+)(?:\.(\d+))?", text or "")
    return tuple(int(x or 0) for x in m.groups()) if m else ()


# ----------------------------------------------------------------------------------------------
# Settings (remembered in the user's settings folder, never next to the program)
# ----------------------------------------------------------------------------------------------

def config_dir():
    if os.environ.get("MPC_SR_LOADER_HOME"):
        return os.environ["MPC_SR_LOADER_HOME"]
    home = os.path.expanduser("~")
    if sys.platform == "darwin":
        base = os.path.join(home, "Library", "Application Support")
    elif os.name == "nt":
        base = os.environ.get("APPDATA") or home
    else:
        base = os.environ.get("XDG_CONFIG_HOME") or os.path.join(home, ".config")
    return os.path.join(base, APP)


def read_json(path, default):
    try:
        with open(path, encoding="utf-8") as f:
            v = json.load(f)
        return v if isinstance(v, type(default)) else default
    except (OSError, ValueError):
        return default


def write_json(path, value):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(value, f, indent=1, sort_keys=True)
    try:
        os.chmod(tmp, 0o600)
    except OSError:
        pass
    os.replace(tmp, path)


DEFAULTS = {"address": "", "user": "root", "ssh_port": 22, "drive": "", "library": "", "port": DEFAULT_PORT,
            "last_collection": ""}


def default_library():
    """Next to tools/library.py when run from the repository (they share the library), else ~/Music."""
    repo = os.path.dirname(HERE)
    if os.path.isfile(os.path.join(repo, "tools", "library.py")):
        return os.path.join(repo, "library", "cache")
    return os.path.join(os.path.expanduser("~"), "Music", "Sight Reader Library")


# ----------------------------------------------------------------------------------------------
# The library on this computer: <root>/<collection>/*.mid plus an index.tsv of titles per folder
# ----------------------------------------------------------------------------------------------

class LibraryError(Exception):
    pass


class Library:
    def __init__(self, root):
        self.root = root
        self._cache = {}                    # path -> ((size, mtime), md5, ok, info)

    def _check_coll(self, name, must_exist=True):
        if not name or name != os.path.basename(name) or name.startswith(".") or "\\" in name or "/" in name:
            raise LibraryError("That is not a collection name.")
        p = os.path.join(self.root, name)
        if must_exist and not os.path.isdir(p):
            raise LibraryError("There is no collection called '%s'." % name)
        return p

    def _check_file(self, coll, file):
        d = self._check_coll(coll)
        if not file or file != os.path.basename(file) or file.startswith(".") or "\\" in file:
            raise LibraryError("That is not a file name.")
        p = os.path.join(d, file)
        if not os.path.isfile(p):
            raise LibraryError("'%s' is not in '%s' any more." % (file, coll))
        return d, p

    def _digest(self, path, st):
        key = (st.st_size, st.st_mtime_ns)
        c = self._cache.get(path)
        if c and c[0] == key:
            return c[1], c[2], c[3]
        with open(path, "rb") as f:
            data = f.read()
        ok, info = is_midi(data), None
        if ok:
            try:
                info = midi_info(data)
            except (ValueError, IndexError) as e:
                info = {"problem": str(e)}
        self._cache[path] = (key, md5_of(data), ok, info)
        return self._cache[path][1:]

    def scan(self):
        """Every collection and piece, with what the plugin would make of it."""
        out = {"root": self.root, "exists": os.path.isdir(self.root), "collections": [], "stray": []}
        if not out["exists"]:
            return out
        try:
            names = sorted(os.listdir(self.root), key=str.lower)
        except OSError as e:
            out["error"] = str(e)
            return out
        for name in names:
            p = os.path.join(self.root, name)
            if name.startswith("."):
                continue
            if not os.path.isdir(p):
                if is_midi_name(name):
                    out["stray"].append(name)
                continue
            rows = {}
            try:
                with open(os.path.join(p, "index.tsv"), encoding="utf-8", errors="replace") as f:
                    rows = read_index_text(f.read())
            except OSError:
                pass
            pieces = []
            try:
                files = sorted(os.listdir(p), key=str.lower)
            except OSError:
                files = []
            for file in files:
                fp = os.path.join(p, file)
                if not is_midi_name(file) or not os.path.isfile(fp):
                    continue
                try:
                    st = os.stat(fp)
                    md5, ok, info = self._digest(fp, st)
                except OSError:
                    continue
                row = rows.get(file)
                problem = ""
                if not ok:
                    problem = "This is not a MIDI file."
                elif st.st_size > MAX_MIDI:
                    problem = "Too big for Sight Reader (over 8 MB)."
                elif len(file.encode("utf-8")) > 100 or len(name.encode("utf-8")) > 150:
                    problem = "The name is too long to copy: rename it."
                pieces.append({"file": file, "rel": name + "/" + file, "size": st.st_size, "md5": md5,
                               "title": (row[1] if row and row[1] else title_from(file)),
                               "composer": row[2] if row else "", "level": row[3] if row else "",
                               "source": row[4] if row else "", "row": row, "info": info, "problem": problem})
            out["collections"].append({"name": name, "pieces": pieces})
        return out

    # -- changes made from the page ---------------------------------------------------------
    def _rows(self, d):
        try:
            with open(os.path.join(d, "index.tsv"), encoding="utf-8", errors="replace") as f:
                return read_index_text(f.read())
        except OSError:
            return {}

    def _write_rows(self, d, rows):
        tmp = os.path.join(d, ".index.tsv.tmp")
        with open(tmp, "w", encoding="utf-8", newline="\n") as f:
            f.write(index_text(rows))
        os.replace(tmp, os.path.join(d, "index.tsv"))

    def new_collection(self, name):
        n = safe_name(name, 48)
        if not n:
            raise LibraryError("Give the collection a name (letters and numbers).")
        if n.lower() in RESERVED:
            raise LibraryError("Sight Reader uses '%s' for its own list already: pick another name." % n)
        for existing in (os.listdir(self.root) if os.path.isdir(self.root) else []):
            if existing.lower() == n.lower() and os.path.isdir(os.path.join(self.root, existing)):
                return existing
        os.makedirs(os.path.join(self.root, n), exist_ok=True)
        return n

    def collection_for(self, name):
        """An existing collection by its exact name (whatever signs Finder allowed in it), else a new one."""
        if name and name == os.path.basename(name) and not name.startswith(".") and os.path.isdir(os.path.join(self.root, name)):
            return name
        return self.new_collection(name)

    def add(self, coll, filename, data, title=None, composer="", level=""):
        if len(data) > MAX_MIDI:
            raise LibraryError("'%s' is over 8 MB, more than Sight Reader reads." % filename)
        if not is_midi(data):
            raise LibraryError("'%s' is not a MIDI file." % filename)
        coll = self.collection_for(coll)
        d = self._check_coll(coll)
        fname = midi_file_name(filename)
        stem, ext = os.path.splitext(fname)
        k = 2
        while os.path.exists(os.path.join(d, fname)):
            with open(os.path.join(d, fname), "rb") as f:
                if md5_of(f.read()) == md5_of(data):
                    return {"collection": coll, "file": fname, "already": True}
            fname = "%s (%d)%s" % (stem, k, ext)
            k += 1
        tmp = os.path.join(d, "." + fname + ".part")
        with open(tmp, "wb") as f:
            f.write(data)
        os.replace(tmp, os.path.join(d, fname))
        rows = self._rows(d)
        rows[fname] = [fname, clean_field(title) if title else title_from(filename), clean_field(composer),
                       str(level or ""), "added with the MIDI loader"]
        self._write_rows(d, rows)
        return {"collection": coll, "file": fname, "already": False}

    def update(self, coll, file, title, composer, level):
        d, _ = self._check_file(coll, file)
        rows = self._rows(d)
        old = rows.get(file) or [file, "", "", "", "added by you"]
        lv = str(level).strip()
        if lv and not lv.isdigit():
            raise LibraryError("The level is a number (1 easiest).")
        rows[file] = [file, clean_field(title) or title_from(file), clean_field(composer), lv, old[4]]
        self._write_rows(d, rows)

    def _trash(self, path):
        """Into the computer's Trash where there is one (so a mistake can be undone), else .trash/."""
        name = os.path.basename(path)
        mac_trash = os.path.join(os.path.expanduser("~"), ".Trash")
        targets = [mac_trash] if sys.platform == "darwin" and os.path.isdir(mac_trash) else []
        targets.append(os.path.join(self.root, ".trash", time.strftime("%Y-%m-%d %H.%M.%S")))
        for t in targets:
            try:
                os.makedirs(t, exist_ok=True)
                dest, stem, ext, k = os.path.join(t, name), os.path.splitext(name)[0], os.path.splitext(name)[1], 2
                while os.path.exists(dest):
                    dest = os.path.join(t, "%s %d%s" % (stem, k, ext))
                    k += 1
                shutil.move(path, dest)
                return dest
            except OSError:
                continue
        raise LibraryError("Could not move '%s' to the Trash." % name)

    def delete(self, coll, file):
        d, p = self._check_file(coll, file)
        self._trash(p)
        rows = self._rows(d)
        if rows.pop(file, None) is not None:
            self._write_rows(d, rows)

    def rename_collection(self, old, new):
        src = self._check_coll(old)
        n = safe_name(new, 48)
        if not n:
            raise LibraryError("Give the collection a name (letters and numbers).")
        if n.lower() in RESERVED:
            raise LibraryError("Sight Reader uses '%s' for its own list already: pick another name." % n)
        if n == old:
            return n
        if os.path.exists(os.path.join(self.root, n)) and n.lower() != old.lower():
            raise LibraryError("There is a collection called '%s' already." % n)
        os.rename(src, os.path.join(self.root, n))
        return n

    def delete_collection(self, name):
        self._trash(self._check_coll(name))

    def import_piece(self, coll, file, data, row):
        """A piece copied back from the MPC, under the same names it has there."""
        os.makedirs(self.root, exist_ok=True)
        d = self._check_coll(coll, must_exist=False)
        os.makedirs(d, exist_ok=True)
        if file != os.path.basename(file) or file.startswith("."):
            raise LibraryError("Bad file name from the MPC: %r" % file)
        tmp = os.path.join(d, "." + file + ".part")
        with open(tmp, "wb") as f:
            f.write(data)
        os.replace(tmp, os.path.join(d, file))
        if row:
            rows = self._rows(d)
            rows[file] = (list(row) + [""] * 5)[:5]
            self._write_rows(d, rows)


# ----------------------------------------------------------------------------------------------
# SSH to the MPC (the computer's own ssh command, as the SDK's mpc_remote.py does it)
# ----------------------------------------------------------------------------------------------

class SshError(Exception):
    def __init__(self, kind, detail=""):
        Exception.__init__(self, detail or kind)
        self.kind = kind                    # noclient | dns | unreachable | refused | auth | closed | timeout | other
        self.detail = detail


def classify_ssh_error(err):
    e = err.lower()
    if "permission denied" in e or "authentication failed" in e or "too many authentication" in e:
        return "auth"
    if "refused" in e:
        return "refused"
    if "could not resolve" in e or "name or service not known" in e or "nodename nor servname" in e:
        return "dns"
    if "timed out" in e or "no route" in e or "unreachable" in e or "host is down" in e:
        return "unreachable"
    if "closed by remote" in e or "connection reset" in e or "kex_exchange" in e or "banner" in e:
        return "closed"
    return "other"


DEV_MODE = ("Switch on developer mode on the MPC: Preferences > Info, tap the version value 3 times, "
            "then the serial number value 2 times, then the firmware version once. That starts its SSH server.")


def explain(kind, host, detail=""):
    """What went wrong, in words, for the page: (title, [lines])."""
    if kind == "noclient":
        return ("This computer has no ssh command",
                ["macOS and Linux have it built in. On Windows 10/11: Settings > Apps > Optional features > OpenSSH Client."])
    if kind == "dns":
        return ("Can't find '%s'" % host, ["Use the MPC's IP address instead. The MPC shows it in Preferences: "
                                           "the Wi-Fi tab (hold Shift to reach it) or the Ethernet tab."])
    if kind == "unreachable":
        return ("Nothing answers at %s" % host,
                ["Is the MPC switched on, and on the same network as this computer?",
                 "Is the address right? The MPC shows it in Preferences: the Wi-Fi tab (hold Shift) or the Ethernet tab.",
                 "Guest Wi-Fi and some routers stop devices talking to each other."])
    if kind == "refused":
        return ("The MPC is there, but its SSH server is off", [DEV_MODE])
    if kind == "auth":
        return ("The MPC turned down the login",
                ["If your MPC needs a password, enter it in Settings (it is kept only until the loader stops).",
                 "The user is normally root."])
    if kind == "closed":
        return ("The connection closed straight away",
                ["The MPC's SSH server may still be starting: wait ten seconds and try again.",
                 "If it keeps happening, switch developer mode off and on again on the MPC."])
    if kind == "timeout":
        return ("The MPC stopped answering", ["Check the network and try again."])
    return ("SSH problem", [detail or "unknown error"])


class Remote:
    def __init__(self, host, user="root", port=22, password=None):
        self.host, self.user, self.port, self.password = host, user or "root", int(port or 22), password
        self.ssh = os.environ.get("MPC_SSH_BIN") or shutil.which("ssh")
        self._tmp = self._askpass = self._control = None

    def _ensure_tmp(self):
        if self._tmp is None:
            base = "/tmp" if os.name != "nt" and os.path.isdir("/tmp") else None   # short control socket path
            self._tmp = tempfile.mkdtemp(prefix="srml-", dir=base)
            os.chmod(self._tmp, 0o700)
        return self._tmp

    def _askpass_script(self):
        if self._askpass is None:
            d = self._ensure_tmp()
            if os.name == "nt":
                self._askpass = os.path.join(d, "askpass.bat")
                with open(self._askpass, "w") as f:
                    f.write("@echo off\r\necho.%MPC_ASKPASS_PW%\r\n")
            else:
                self._askpass = os.path.join(d, "askpass.sh")
                with open(self._askpass, "w") as f:
                    f.write("#!/bin/sh\nprintf '%s\\n' \"$MPC_ASKPASS_PW\"\n")
                os.chmod(self._askpass, 0o700)
        return self._askpass

    def _command(self, remote_cmd):
        if not self.ssh:
            raise SshError("noclient")
        null = "NUL" if os.name == "nt" else "/dev/null"
        # The MPC makes a new host key from time to time, so known_hosts is not used (as mpc_remote.py).
        cmd = [self.ssh, "-p", str(self.port), "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=" + null,
               "-o", "LogLevel=ERROR", "-o", "ConnectTimeout=8", "-o", "ServerAliveInterval=15",
               "-o", "ServerAliveCountMax=4", "-o", "NumberOfPasswordPrompts=1"]
        if self.password is None:
            cmd += ["-o", "BatchMode=yes"]
        if os.name != "nt" and not os.environ.get("MPC_SSH_BIN"):
            if self._control is None:
                self._control = os.path.join(self._ensure_tmp(), "cm")
            cmd += ["-o", "ControlMaster=auto", "-o", "ControlPath=" + self._control, "-o", "ControlPersist=120"]
        return cmd + ["%s@%s" % (self.user, self.host), remote_cmd]

    def _env(self):
        env = dict(os.environ)
        if self.password is not None:
            env["MPC_ASKPASS_PW"] = self.password
            env["SSH_ASKPASS"] = self._askpass_script()
            env["SSH_ASKPASS_REQUIRE"] = "force"
            env.setdefault("DISPLAY", ":0")
        return env

    def run(self, script, stdin=None, timeout=60):
        """Runs a shell script on the MPC: (exit code, stdout bytes, stderr text)."""
        cmd = self._command("sh -c " + shlex.quote(script))
        try:
            p = subprocess.run(cmd, input=stdin, stdin=None if stdin is not None else subprocess.DEVNULL,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout, env=self._env(),
                               start_new_session=(os.name != "nt" and self.password is not None))
        except FileNotFoundError:
            raise SshError("noclient")
        except subprocess.TimeoutExpired:
            raise SshError("timeout")
        err = p.stderr.decode("utf-8", "replace").strip()
        if p.returncode == 255:
            raise SshError(classify_ssh_error(err), err)
        return p.returncode, p.stdout, err

    def close(self):
        if self._control and self.ssh and os.path.exists(self._control):
            try:
                subprocess.run([self.ssh, "-S", self._control, "-O", "exit", "x"], stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL, timeout=5)
            except Exception:                 # noqa: BLE001 - best effort on the way out
                pass
        if self._tmp:
            shutil.rmtree(self._tmp, ignore_errors=True)
            self._tmp = self._askpass = self._control = None


# One look at the MPC: its drives, where Sight Reader is installed (and which version), and the
# library on each such drive with file sizes and checksums. Plain sh, so it runs on any MPC.
SURVEY = r'''
echo "@@ok"
echo "@@id $(cat /etc/machine-id 2>/dev/null)"
echo "@@model $(tr -d '\000' < /proc/device-tree/model 2>/dev/null)"
m=""; command -v md5sum >/dev/null 2>&1 && m=1 && echo "@@hasmd5"
lst() {
  echo "@@lib $1/SightReader/Library"
  cd "$1/SightReader/Library" 2>/dev/null || return 0
  echo "@@libok"
  for c in ./*; do [ -d "$c" ] && echo "@@coll $c"; done
  wc -c ./*/* 2>/dev/null | sed 's/^/@@size /'
  [ -n "$m" ] && md5sum ./*/* 2>/dev/null | sed 's/^/@@md5 /'
  for i in ./*/index.tsv; do [ -f "$i" ] && { echo "@@index $i"; sed 's/^/|/' "$i"; echo; }; done
  cd /
}
for d in {DRIVES}; do
  [ -d "$d" ] || continue
  [ -d "$d/VST2 Plugins" ] || [ -d "$d/SightReader" ] || continue
  echo "@@drive $d"
  for q in "$d/VST2 Plugins/"*"Sight Reader"; do
    [ -d "$q" ] || continue
    echo "@@plugin $q"
    echo "@@version $(sed -n 's/.*"version"[^"]*"\([^"]*\)".*/\1/p' "$q/plugin.json" 2>/dev/null | head -n 1)"
  done
  lst "$d"
done
echo "@@end"
'''


def _rel(path, depth):
    """'./Coll/file.mid' -> 'Coll/file.mid' when it is exactly `depth` levels deep, else None."""
    p = path[2:] if path.startswith("./") else None
    if not p or p.count("/") != depth - 1 or any(part in ("", ".", "..") for part in p.split("/")):
        return None
    return p


def parse_survey(out):
    info = {"ok": False, "id": "", "model": "", "md5": False, "drives": []}
    drive = lib = None
    index_coll, index_lines = None, None
    for raw in out.decode("utf-8", "replace").split("\n"):
        line = raw.rstrip("\r")
        if index_lines is not None:
            if line.startswith("|"):
                index_lines.append(line[1:])
                continue
            lib["index"][index_coll] = "\n".join(index_lines) + "\n"
            index_lines = None
        if not line.startswith("@@"):
            continue
        tag, _, rest = line[2:].partition(" ")
        if tag == "ok":
            info["ok"] = True
        elif tag == "id":
            info["id"] = rest.strip()
        elif tag == "model":
            info["model"] = clean_field(rest, 60)
        elif tag == "hasmd5":
            info["md5"] = True
        elif tag == "drive":
            drive = {"path": rest, "plugins": [], "lib": None}
            info["drives"].append(drive)
            lib = None
        elif tag == "plugin" and drive is not None:
            drive["plugins"].append({"path": rest, "version": ""})
        elif tag == "version" and drive is not None and drive["plugins"]:
            drive["plugins"][-1]["version"] = rest.strip()
        elif tag == "lib" and drive is not None:
            lib = {"path": rest, "exists": False, "colls": [], "files": {}, "index": {}}
            drive["lib"] = lib
        elif lib is None:
            continue
        elif tag == "libok":
            lib["exists"] = True
        elif tag == "coll":
            c = _rel(rest, 1)
            if c:
                lib["colls"].append(c)
        elif tag == "size":
            m = re.match(r"\s*(\d+) (.*)$", rest)
            rel = _rel(m.group(2), 2) if m else None
            if rel:
                lib["files"].setdefault(rel, {})["size"] = int(m.group(1))
        elif tag == "md5":
            m = re.match(r"([0-9a-fA-F]{32}) [ *](.*)$", rest)
            rel = _rel(m.group(2), 2) if m else None
            if rel:
                lib["files"].setdefault(rel, {})["md5"] = m.group(1).lower()
        elif tag == "index":
            rel = _rel(rest, 2)
            if rel:
                index_coll, index_lines = rel.split("/")[0], []
    if index_lines is not None and lib is not None:
        lib["index"][index_coll] = "\n".join(index_lines) + "\n"
    for d in info["drives"]:              # only what Sight Reader reads: MIDI files and the titles
        if d["lib"]:
            d["lib"]["files"] = {r: v for r, v in d["lib"]["files"].items()
                                 if "size" in v and (is_midi_name(r.split("/")[1]) or r.split("/")[1] == "index.tsv")}
    return info


def choose_drive(survey, wanted=""):
    """The drive whose SightReader/Library the plugin reads: where it is installed. (drive, problem)"""
    drives = survey["drives"]
    if wanted:
        for d in drives:
            if d["path"].rstrip("/") == wanted.rstrip("/"):
                return d, ""
        return None, "The MPC has no drive '%s' with Sight Reader on it (Settings > Drive)." % wanted
    with_plugin = [d for d in drives if d["plugins"]]
    if with_plugin:
        internal = [d for d in with_plugin if os.path.basename(d["path"].rstrip("/")).startswith(INTERNAL)]
        return (internal or with_plugin)[0], ""
    with_lib = [d for d in drives if d["lib"] and d["lib"]["exists"]]
    if with_lib:
        return with_lib[0], ""
    return None, ("Sight Reader is not installed on this MPC (no 'VST2 Plugins/...Sight Reader' folder on its drives). "
                  "Install it first: see the README.")


# ----------------------------------------------------------------------------------------------
# What a sync does: computed here, from the two listings and what this computer put there before
# ----------------------------------------------------------------------------------------------

def make_plan(scan, lib, manifest, remove=True):
    """local scan + MPC library listing + {rel: md5} this computer put there -> the changes.

    Pieces here are copied when the MPC lacks them or has other contents. A piece on the MPC that
    this computer put there and that is gone here is removed (unless remove is False). Everything
    else on the MPC is left alone: it is listed as only on the MPC.
    """
    local = {p["rel"]: p for c in scan["collections"] for p in c["pieces"]}
    remote = lib["files"] if lib else {}
    add, replace, same, removals, mpc_only = [], [], [], [], []
    for rel, p in local.items():
        if p["problem"]:
            continue
        r = remote.get(rel)
        if r is None:
            add.append(rel)
        elif r.get("md5"):
            (same if r["md5"] == p["md5"] else replace).append(rel)
        elif manifest.get(rel) == p["md5"] and r.get("size") == p["size"]:
            same.append(rel)                # an MPC without md5sum: trust what was put there, if the size holds
        else:
            replace.append(rel)
    for rel, r in remote.items():
        if rel in local or rel.endswith("/index.tsv"):
            continue
        ours = manifest.get(rel)
        if ours and (not r.get("md5") or r["md5"] == ours):
            removals.append(rel)
        else:
            mpc_only.append(rel)
    keep = mpc_only + ([] if remove else removals)
    gone = removals if remove else []
    # the titles: index.tsv on the MPC lists the pieces that will be there, titles from here first
    colls = {c["name"] for c in scan["collections"] if any(not p["problem"] for p in c["pieces"])}
    colls |= {rel.split("/")[0] for rel in gone}
    index_put, index_rm, rmdirs = {}, [], []
    remote_index = lib["index"] if lib else {}
    for coll in sorted(colls):
        rows = {}
        for p in (c for c in scan["collections"] if c["name"] == coll):
            for piece in p["pieces"]:
                if not piece["problem"] and piece["row"]:
                    rows[piece["file"]] = list(piece["row"])
        theirs = read_index_text(remote_index.get(coll, ""))
        for rel in keep:
            c, f = rel.split("/", 1)
            if c == coll and f in theirs:
                rows[f] = theirs[f]
        old = remote_index.get(coll)
        if rows:
            if old is None or not same_index(old, index_text(rows)):
                index_put[coll] = index_text(rows)
        elif old is not None:
            index_rm.append(coll)
        left = [r for r in remote if r.split("/")[0] == coll and r not in gone and not r.endswith("/index.tsv")]
        if not left and not any(p["rel"].split("/")[0] == coll for c in scan["collections"] for p in c["pieces"]
                                if not p["problem"]):
            rmdirs.append(coll)
    upload = sum(local[r]["size"] for r in add + replace) + sum(len(t) for t in index_put.values())
    return {"add": sorted(add), "replace": sorted(replace), "same": sorted(same), "remove": sorted(removals),
            "mpc_only": sorted(mpc_only), "index_put": index_put, "index_rm": index_rm, "rmdirs": rmdirs,
            "upload": upload, "removing": bool(remove),
            "empty": not (add or replace or gone or index_put or index_rm)}


def _tar_bytes(tar, name, data, mtime=None, is_dir=False):
    ti = tarfile.TarInfo(name)
    ti.uid = ti.gid = 0
    ti.uname = ti.gname = "root"
    ti.mtime = int(mtime if mtime is not None else time.time())
    if is_dir:
        ti.type, ti.mode = tarfile.DIRTYPE, 0o755
        tar.addfile(ti)
    else:
        ti.size, ti.mode = len(data), 0o644
        tar.addfile(ti, io.BytesIO(data))


def marker_command():
    """Tells Sight Reader 0.6.0+ that the library changed: it watches this file."""
    token = "%d %s" % (time.time_ns(), APP)
    return "printf '%%s\\n' %s > .rescan.tmp && mv -f .rescan.tmp .rescan" % shlex.quote(token)


def human_size(n):
    for unit in ("bytes", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return ("%d %s" % (n, unit)) if unit == "bytes" else ("%.1f %s" % (n, unit))
        n /= 1024.0


# ----------------------------------------------------------------------------------------------
# Jobs: one thing at a time talks to the MPC; the page follows its steps
# ----------------------------------------------------------------------------------------------

class Busy(Exception):
    pass


class Job:
    def __init__(self, kind, title):
        self.id = secrets.token_hex(4)
        self.kind, self.title = kind, title
        self.state, self.ok = "running", None
        self.steps, self.message, self.lines = [], "", []
        self.started, self.finished = time.time(), None

    def step(self, text):
        self.steps.append(text)

    def done(self, message, lines=()):
        self.state, self.ok, self.message, self.lines, self.finished = "done", True, message, list(lines), time.time()

    def fail(self, message, lines=()):
        self.state, self.ok, self.message, self.lines, self.finished = "done", False, message, list(lines), time.time()

    def snapshot(self):
        return {"id": self.id, "kind": self.kind, "title": self.title, "state": self.state, "ok": self.ok,
                "steps": list(self.steps), "message": self.message, "lines": list(self.lines),
                "elapsed": round((self.finished or time.time()) - self.started, 1)}


# ----------------------------------------------------------------------------------------------
# The application: settings, library, the MPC, and what the page asks for
# ----------------------------------------------------------------------------------------------

class App:
    def __init__(self, cfg_dir=None):
        self.cfg_dir = cfg_dir or config_dir()
        self.settings_path = os.path.join(self.cfg_dir, "settings.json")
        self.synced_path = os.path.join(self.cfg_dir, "synced.json")
        stored = read_json(self.settings_path, {})
        self.settings = dict(DEFAULTS)
        self.settings.update({k: v for k, v in stored.items() if k in DEFAULTS})
        self.synced = read_json(self.synced_path, {})
        self.password = None                  # only in memory, never written anywhere
        self.lock = threading.RLock()
        self.library = Library(self.library_root())
        self._remote, self._remote_key = None, None
        self.mpc = None                       # what the last look at the MPC found
        self.job, self._thread = None, None
        self.token = secrets.token_urlsafe(24)
        self.nonce = secrets.token_urlsafe(16)
        self.quit = threading.Event()

    # -- settings -------------------------------------------------------------------------------
    def library_root(self):
        return os.path.abspath(os.path.expanduser(self.settings.get("library") or default_library()))

    def save_settings(self, changes):
        with self.lock:
            old_target = (self.settings["address"], self.settings["user"], self.settings["ssh_port"], self.settings["drive"])
            for k, v in changes.items():
                if k == "password":
                    self.password = v if v else None
                    continue
                if k not in DEFAULTS:
                    continue
                if k == "ssh_port" or k == "port":
                    try:
                        v = int(v)
                    except (TypeError, ValueError):
                        raise LibraryError("The port is a number (SSH uses 22).")
                    if not 0 < v < 65536:
                        raise LibraryError("The port is a number from 1 to 65535.")
                elif k == "address":
                    v = str(v).strip()
                    if v and not re.match(r"^[A-Za-z0-9.:\-\[\]%]+$", v):
                        raise LibraryError("That does not look like an address: use the MPC's IP address.")
                elif k == "user":
                    v = str(v).strip() or "root"
                    if not re.match(r"^[A-Za-z0-9._-]+$", v):
                        raise LibraryError("That is not a user name (the MPC's is root).")
                elif k == "drive":
                    v = str(v).strip()
                    if v and not v.startswith("/"):
                        raise LibraryError("A drive is a folder on the MPC, like /nvme. Leave it empty to find it.")
                elif k == "library":
                    v = os.path.abspath(os.path.expanduser(str(v).strip())) if str(v).strip() else ""
                    if v and os.path.exists(v) and not os.path.isdir(v):
                        raise LibraryError("The library has to be a folder.")
                self.settings[k] = v
            write_json(self.settings_path, self.settings)
            self.library = Library(self.library_root())
            if (self.settings["address"], self.settings["user"], self.settings["ssh_port"], self.settings["drive"]) != old_target:
                self.mpc = None

    def remote(self):
        key = (self.settings["address"], self.settings["user"], self.settings["ssh_port"], self.password)
        if self._remote is None or self._remote_key != key:
            if self._remote:
                self._remote.close()
            self._remote = Remote(self.settings["address"], self.settings["user"], self.settings["ssh_port"], self.password)
            self._remote_key = key
        return self._remote

    def close(self):
        if self._remote:
            self._remote.close()

    # -- the MPC ----------------------------------------------------------------------------------
    def device_key(self, survey, lib_path):
        ident = survey.get("id") or ("address " + self.settings["address"])
        return hashlib.sha256(ident.encode("utf-8")).hexdigest()[:16] + "|" + lib_path

    def manifest(self):
        if not self.mpc or not self.mpc.get("lib"):
            return {}
        return dict(self.synced.get(self.mpc["key"], {}).get("files", {}))

    def save_manifest(self, files):
        self.synced[self.mpc["key"]] = {"files": files, "when": time.strftime("%Y-%m-%d %H:%M:%S")}
        write_json(self.synced_path, self.synced)

    def look(self, job=None):
        """Lists the MPC's drives and library. Sets self.mpc (or raises SshError / LibraryError)."""
        if not self.settings["address"]:
            raise LibraryError("Enter the MPC's address in Settings first.")
        if job:
            job.step("Looking at the MPC")
        rc, out, err = self.remote().run(SURVEY.replace("{DRIVES}", DRIVES), timeout=45)
        survey = parse_survey(out)
        if not survey["ok"]:
            raise LibraryError("The MPC did not answer as expected: %s" % (err or "no output"))
        drive, problem = choose_drive(survey, self.settings["drive"])
        lib = drive["lib"] if drive else None
        lib_path = lib["path"] if lib else ""
        plugin = drive["plugins"][0] if drive and drive["plugins"] else None
        with self.lock:
            self.mpc = {"when": time.time(), "model": survey["model"], "md5": survey["md5"],
                        "drives": [d["path"] for d in survey["drives"]], "drive": drive["path"] if drive else "",
                        "plugin": plugin, "problem": problem, "lib": lib, "lib_path": lib_path,
                        "key": self.device_key(survey, lib_path) if lib_path else "",
                        "auto_rescan": bool(plugin) and version_tuple(plugin["version"]) >= AUTO_RESCAN_SINCE}
        return self.mpc

    def _need_lib(self):
        if not self.mpc or not self.mpc.get("lib_path"):
            raise LibraryError((self.mpc or {}).get("problem") or "The MPC's library was not found.")
        return self.mpc["lib_path"]

    def _rescan_note(self):
        if self.mpc and self.mpc.get("auto_rescan"):
            return "Sight Reader shows the changes within a few seconds."
        return "On the MPC, press RESCAN on Sight Reader's LOAD page (from 0.6.0 on, it rescans by itself)."

    # -- jobs -------------------------------------------------------------------------------------
    def start(self, kind, title, fn, *args):
        with self.lock:
            if self.job and self.job.state == "running":
                raise Busy("The loader is busy with '%s'. Try again when it has finished." % self.job.title)
            job = self.job = Job(kind, title)

        def run():
            try:
                fn(job, *args)
                if job.state == "running":
                    job.done("Done.")
            except SshError as e:
                t, lines = explain(e.kind, self.settings["address"], e.detail)
                job.fail(t, lines)
                if kind == "look":
                    with self.lock:
                        self.mpc = {"when": time.time(), "error": {"title": t, "lines": lines}}
            except (LibraryError, OSError, ValueError, tarfile.TarError) as e:
                job.fail(str(e))
                if kind == "look":
                    with self.lock:
                        self.mpc = {"when": time.time(), "error": {"title": str(e), "lines": []}}
            except Exception as e:            # noqa: BLE001 - shown on the page rather than lost in the terminal
                job.fail("Something went wrong: %s" % e)
                print("error in %s:" % kind, repr(e), file=sys.stderr)
        self._thread = threading.Thread(target=run, daemon=True)
        self._thread.start()
        return job

    def wait(self, timeout=120):
        if self._thread:
            self._thread.join(timeout)
        return self.job

    def job_look(self, job):
        mpc = self.look(job)
        if mpc.get("problem"):
            job.fail(mpc["problem"])
        else:
            job.done("Connected.")

    def job_sync(self, job, remove=True):
        self.look(job)
        lib_path = self._need_lib()
        with self.lock:
            scan = self.library.scan()
            manifest = self.manifest()
            plan = make_plan(scan, self.mpc["lib"], manifest, remove)
            local = {p["rel"]: p for c in scan["collections"] for p in c["pieces"]}
            if plan["empty"]:
                job.done("The MPC has all your pieces already.")
                return
            send = plan["add"] + plan["replace"]
            buf = io.BytesIO()
            with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as tar:
                for coll in sorted({r.split("/")[0] for r in send} | set(plan["index_put"])):
                    _tar_bytes(tar, coll, b"", is_dir=True)
                for rel in send:
                    path = os.path.join(self.library.root, *rel.split("/"))
                    with open(path, "rb") as f:
                        data = f.read()
                    if md5_of(data) != local[rel]["md5"]:
                        raise LibraryError("'%s' changed while copying: sync again." % rel)
                    _tar_bytes(tar, rel, data, os.path.getmtime(path))
                for coll, text in plan["index_put"].items():
                    _tar_bytes(tar, coll + "/index.tsv", text.encode("utf-8"))
        parts = ["set -e", "mkdir -p %s" % shlex.quote(lib_path), "cd %s" % shlex.quote(lib_path)]
        if send or plan["index_put"]:
            parts.append("tar -xf -")
        gone = (plan["remove"] if remove else []) + [c + "/index.tsv" for c in plan["index_rm"]]
        if gone:
            parts.append("rm -f " + " ".join(shlex.quote("./" + r) for r in gone))
        for c in plan["rmdirs"]:
            parts.append("rmdir %s 2>/dev/null || true" % shlex.quote("./" + c))
        parts += [marker_command(), "echo @@synced"]
        what = []
        if plan["add"]:
            what.append("%d new" % len(plan["add"]))
        if plan["replace"]:
            what.append("%d changed" % len(plan["replace"]))
        if send:
            job.step("Copying %s (%s)" % (" and ".join(what), human_size(plan["upload"])))
        elif plan["index_put"]:
            job.step("Updating titles")
        if remove and plan["remove"]:
            job.step("Removing %d piece%s you removed here" % (len(plan["remove"]), "" if len(plan["remove"]) == 1 else "s"))
        rc, out, err = self.remote().run("\n".join(parts), stdin=buf.getvalue() if (send or plan["index_put"]) else None,
                                         timeout=600)
        if rc != 0 or b"@@synced" not in out:
            raise LibraryError("The MPC could not take the files: %s" % (err.splitlines()[-1] if err else "exit %d" % rc))
        files = {r: local[r]["md5"] for r in plan["add"] + plan["replace"] + plan["same"]}
        if not remove:
            files.update({r: manifest[r] for r in plan["remove"] if r in manifest})
        with self.lock:
            self.save_manifest(files)
        job.step("Checking the result")
        self.look()
        summary = []
        if plan["add"]:
            summary.append("%d added" % len(plan["add"]))
        if plan["replace"]:
            summary.append("%d updated" % len(plan["replace"]))
        if remove and plan["remove"]:
            summary.append("%d removed" % len(plan["remove"]))
        if not summary:
            summary.append("titles updated")
        job.done("Synced: %s." % ", ".join(summary), [self._rescan_note()])

    def job_pull(self, job, rels):
        self.look(job)
        lib_path = self._need_lib()
        with self.lock:
            plan = make_plan(self.library.scan(), self.mpc["lib"], self.manifest())
        wanted = [r for r in rels if r in plan["mpc_only"]]
        if not wanted:
            job.fail("Those pieces are not only on the MPC any more.")
            return
        job.step("Copying %d piece%s from the MPC" % (len(wanted), "" if len(wanted) == 1 else "s"))
        rc, out, err = self.remote().run("cd %s && tar -cf - %s" % (shlex.quote(lib_path), " ".join(
            shlex.quote("./" + r) for r in wanted)), timeout=600)
        if rc != 0:
            raise LibraryError("The MPC could not send them: %s" % (err.splitlines()[-1] if err else "exit %d" % rc))
        theirs = self.mpc["lib"]["index"]
        got = {}
        with tarfile.open(fileobj=io.BytesIO(out), mode="r:") as tar:
            for m in tar.getmembers():
                name = m.name
                while name.startswith("./"):
                    name = name[2:]
                if m.isfile() and name in wanted and name not in got:
                    got[name] = tar.extractfile(m).read()
        with self.lock:
            manifest = self.manifest()
            for rel, data in got.items():
                coll, file = rel.split("/", 1)
                row = read_index_text(theirs.get(coll, "")).get(file)
                self.library.import_piece(coll, file, data, row)
                manifest[rel] = md5_of(data)
            self.save_manifest(manifest)
        missing = len(wanted) - len(got)
        job.done("Copied %d piece%s to this computer." % (len(got), "" if len(got) == 1 else "s"),
                 ["%d could not be read on the MPC." % missing] if missing else
                 ["They are in your library now, and Sync keeps them on the MPC."])

    def job_remote_delete(self, job, rels):
        self.look(job)
        lib_path = self._need_lib()
        with self.lock:
            plan = make_plan(self.library.scan(), self.mpc["lib"], self.manifest())
        doomed = [r for r in rels if r in plan["mpc_only"] or r in plan["remove"]]
        if not doomed:
            job.fail("Those pieces are not on the MPC any more.")
            return
        lib = self.mpc["lib"]
        buf = io.BytesIO()
        puts, rms, rmdirs = 0, [], []
        with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as tar:
            for coll in sorted({r.split("/")[0] for r in doomed}):
                rows = read_index_text(lib["index"].get(coll, ""))
                for r in doomed:
                    if r.split("/")[0] == coll:
                        rows.pop(r.split("/", 1)[1], None)
                left = [r for r in lib["files"] if r.split("/")[0] == coll and r not in doomed and not r.endswith("/index.tsv")]
                if coll in lib["index"]:
                    if rows and left:
                        _tar_bytes(tar, coll + "/index.tsv", index_text(rows).encode("utf-8"))
                        puts += 1
                    elif not left or not rows:
                        rms.append(coll + "/index.tsv")
                if not left:
                    rmdirs.append(coll)
        parts = ["set -e", "cd %s" % shlex.quote(lib_path),
                 "rm -f " + " ".join(shlex.quote("./" + r) for r in doomed + rms)]
        if puts:
            parts.append("tar -xf -")
        for c in rmdirs:
            parts.append("rmdir %s 2>/dev/null || true" % shlex.quote("./" + c))
        parts += [marker_command(), "echo @@deleted"]
        job.step("Deleting %d piece%s on the MPC" % (len(doomed), "" if len(doomed) == 1 else "s"))
        rc, out, err = self.remote().run("\n".join(parts), stdin=buf.getvalue() if puts else None, timeout=120)
        if rc != 0 or b"@@deleted" not in out:
            raise LibraryError("The MPC could not delete them: %s" % (err.splitlines()[-1] if err else "exit %d" % rc))
        with self.lock:
            manifest = self.manifest()
            for r in doomed:
                manifest.pop(r, None)
            self.save_manifest(manifest)
        self.look()
        job.done("Deleted %d piece%s from the MPC." % (len(doomed), "" if len(doomed) == 1 else "s"), [self._rescan_note()])

    def job_restart(self, job):
        if not self.settings["address"]:
            raise LibraryError("Enter the MPC's address in Settings first.")
        r = self.remote()
        unit = shlex.quote(MPC_SERVICE)
        job.step("Asking the MPC to restart its app")
        rc, out, err = r.run("systemctl cat %s >/dev/null 2>&1" % unit, timeout=30)
        if rc != 0:
            job.fail("This MPC has no '%s' service, so the loader can't restart it." % MPC_SERVICE,
                     ["Switch the MPC off and on instead."])
            return
        rc, out, err = r.run("systemctl restart %s" % unit, timeout=120)
        if rc != 0:
            job.fail("The restart did not work: %s" % (err.splitlines()[-1] if err else "exit %d" % rc))
            return
        job.step("Waiting for the MPC app to start again")
        state = ""
        for _ in range(90):
            rc, out, err = r.run("systemctl is-active %s" % unit, timeout=20)
            state = out.decode("utf-8", "replace").strip()
            if state == "active":
                break
            time.sleep(1)
        if state != "active":
            job.fail("The MPC app has not come back yet (%s)." % (state or "no answer"),
                     ["Give it another minute. If the screen stays dark, switch the MPC off and on."])
            return
        job.done("The MPC app is running again.", ["It takes a few more seconds before the screen and projects are ready."])

    # -- what the page shows ------------------------------------------------------------------------
    def state(self):
        with self.lock:
            scan = self.library.scan()
            mpc = self.mpc
            plan = make_plan(scan, mpc["lib"], self.manifest()) if mpc and mpc.get("lib") else None
            status = {}
            if plan:
                for k in ("add", "replace", "same"):
                    for rel in plan[k]:
                        status[rel] = k
            colls = []
            for c in scan["collections"]:
                pieces = []
                for p in c["pieces"]:
                    pieces.append({k: p[k] for k in ("file", "rel", "size", "title", "composer", "level", "source", "info", "problem")})
                    pieces[-1]["status"] = "problem" if p["problem"] else status.get(p["rel"], "unknown")
                colls.append({"name": c["name"], "pieces": pieces})
            out = {"version": VERSION,
                   "settings": {k: self.settings[k] for k in ("address", "user", "ssh_port", "drive", "last_collection")},
                   "has_password": self.password is not None,
                   "library": {"root": scan["root"], "exists": scan["exists"], "collections": colls,
                               "stray": scan["stray"], "error": scan.get("error", ""), "default": default_library()},
                   "mpc": None, "plan": None,
                   "job": self.job.snapshot() if self.job else None,
                   "can_choose_folder": sys.platform == "darwin" or bool(shutil.which("zenity"))}
            if mpc:
                m = {"when": mpc["when"]}
                if mpc.get("error"):
                    m["error"] = mpc["error"]
                else:
                    lib = mpc.get("lib")
                    titles = {}
                    for coll, text in (lib["index"].items() if lib else []):
                        for f, row in read_index_text(text).items():
                            titles[coll + "/" + f] = row[1]
                    m.update({"model": mpc["model"], "drive": mpc["drive"], "drives": mpc["drives"],
                              "plugin": mpc["plugin"], "problem": mpc["problem"], "lib_path": mpc["lib_path"],
                              "auto_rescan": mpc["auto_rescan"],
                              "pieces": len([r for r in (lib["files"] if lib else {}) if not r.endswith("/index.tsv")])})
                    if plan:
                        def item(rel):
                            f = rel.split("/", 1)[1]
                            return {"rel": rel, "coll": rel.split("/")[0], "file": f,
                                    "title": titles.get(rel) or title_from(f), "size": lib["files"][rel]["size"]}
                        m["mpc_only"] = [item(r) for r in plan["mpc_only"]]
                        out["plan"] = {"add": len(plan["add"]), "replace": len(plan["replace"]), "same": len(plan["same"]),
                                       "remove": [item(r) for r in plan["remove"]], "titles": len(plan["index_put"]) + len(plan["index_rm"]),
                                       "upload": plan["upload"], "upload_text": human_size(plan["upload"]),
                                       "empty": plan["empty"] and not plan["remove"]}
                out["mpc"] = m
            return out

    def choose_folder(self):
        start = self.library_root()
        try:
            if sys.platform == "darwin":
                script = ['activate', 'POSIX path of (choose folder with prompt "Choose the folder for your Sight Reader library"'
                          + (' default location (POSIX file %s)' % json.dumps(start, ensure_ascii=False) if os.path.isdir(start) else "") + ')']
                r = subprocess.run(["osascript"] + sum((["-e", s] for s in script), []), capture_output=True, text=True, timeout=600)
            elif shutil.which("zenity"):
                r = subprocess.run(["zenity", "--file-selection", "--directory", "--filename", start + "/"],
                                   capture_output=True, text=True, timeout=600)
            else:
                return None
        except (OSError, subprocess.SubprocessError):
            return None
        path = r.stdout.strip()
        return path.rstrip("/") if r.returncode == 0 and path else None

    def reveal(self):
        root = self.library_root()
        os.makedirs(root, exist_ok=True)
        try:
            if sys.platform == "darwin":
                subprocess.Popen(["open", root])
            elif os.name == "nt":
                os.startfile(root)            # noqa: S606 - opens Explorer on the user's own folder
            else:
                subprocess.Popen(["xdg-open", root], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except OSError as e:
            raise LibraryError("Could not open the folder: %s" % e)


# ----------------------------------------------------------------------------------------------
# The local web server: 127.0.0.1 only, and every call carries this run's token
# ----------------------------------------------------------------------------------------------

class Handler(http.server.BaseHTTPRequestHandler):
    server_version = "%s/%s" % (APP, VERSION)
    app = None                                # set by serve()

    def log_message(self, fmt, *args):        # keep the terminal quiet
        pass

    def _send(self, code, body, ctype="application/json; charset=utf-8", extra=None):
        data = body if isinstance(body, bytes) else json.dumps(body).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(data)

    def _ours(self):
        """Only the page this loader served: right Host (no DNS rebinding), right Origin, right token."""
        port = self.server.server_address[1]
        hosts = ("127.0.0.1:%d" % port, "localhost:%d" % port)
        if self.headers.get("Host", "") not in hosts:
            return False
        origin = self.headers.get("Origin")
        if origin is not None and origin not in ["http://" + h for h in hosts]:
            return False
        return hmac.compare_digest(self.headers.get("X-Loader-Token", ""), self.app.token)

    def do_GET(self):
        port = self.server.server_address[1]
        if self.headers.get("Host", "") not in ("127.0.0.1:%d" % port, "localhost:%d" % port):
            return self._send(403, {"ok": False, "error": "wrong host"})
        path = urllib.parse.urlparse(self.path).path
        if path == "/":
            page = PAGE.replace("__TOKEN__", self.app.token).replace("__NONCE__", self.app.nonce).replace("__VERSION__", VERSION)
            csp = ("default-src 'none'; script-src 'nonce-%s'; style-src 'nonce-%s'; img-src data:; connect-src 'self'; "
                   "base-uri 'none'; form-action 'none'; frame-ancestors 'none'" % (self.app.nonce, self.app.nonce))
            return self._send(200, page.encode("utf-8"), "text/html; charset=utf-8",
                              {"Content-Security-Policy": csp, "X-Frame-Options": "DENY"})
        if path == "/api/ping":
            return self._send(200, {"app": APP, "version": VERSION})
        if not self._ours():
            return self._send(403, {"ok": False, "error": "This page is not the one the loader served. Reload it."})
        if path == "/api/state":
            return self._send(200, self.app.state())
        if path == "/api/job":
            return self._send(200, {"job": self.app.job.snapshot() if self.app.job else None})
        return self._send(404, {"ok": False, "error": "not found"})

    def do_POST(self):
        if not self._ours():
            return self._send(403, {"ok": False, "error": "This page is not the one the loader served. Reload it."})
        url = urllib.parse.urlparse(self.path)
        try:
            length = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            length = -1
        if length < 0 or length > MAX_MIDI + (1 << 16):
            return self._send(413, {"ok": False, "error": "That is more than Sight Reader reads (8 MB)."})
        body = self.rfile.read(length) if length else b""
        app = self.app
        try:
            if url.path == "/api/upload":
                q = dict(urllib.parse.parse_qsl(url.query))
                with app.lock:
                    r = app.library.add(q.get("collection", ""), q.get("name", "piece.mid"), body)
                    app.settings["last_collection"] = r["collection"]
                    write_json(app.settings_path, app.settings)
                return self._send(200, dict(ok=True, **r))
            args = json.loads(body.decode("utf-8") or "{}") if body else {}
            if not isinstance(args, dict):
                raise LibraryError("bad request")
            if url.path == "/api/settings":
                app.save_settings(args)
                return self._send(200, {"ok": True})
            if url.path == "/api/look":
                return self._send(200, {"ok": True, "job": app.start("look", "Looking at the MPC", app.job_look).snapshot()})
            if url.path == "/api/sync":
                return self._send(200, {"ok": True, "job": app.start("sync", "Syncing with the MPC", app.job_sync,
                                                                     bool(args.get("remove", True))).snapshot()})
            if url.path == "/api/pull":
                rels = [str(r) for r in args.get("rels", [])]
                return self._send(200, {"ok": True, "job": app.start("pull", "Copying from the MPC", app.job_pull, rels).snapshot()})
            if url.path == "/api/remote-delete":
                rels = [str(r) for r in args.get("rels", [])]
                return self._send(200, {"ok": True, "job": app.start("delete", "Deleting on the MPC",
                                                                     app.job_remote_delete, rels).snapshot()})
            if url.path == "/api/restart":
                return self._send(200, {"ok": True, "job": app.start("restart", "Restarting the MPC app", app.job_restart).snapshot()})
            with app.lock:
                if url.path == "/api/piece":
                    app.library.update(args.get("collection", ""), args.get("file", ""), args.get("title", ""),
                                       args.get("composer", ""), args.get("level", ""))
                    return self._send(200, {"ok": True})
                if url.path == "/api/piece/delete":
                    app.library.delete(args.get("collection", ""), args.get("file", ""))
                    return self._send(200, {"ok": True})
                if url.path == "/api/collection":
                    os.makedirs(app.library.root, exist_ok=True)
                    name = app.library.new_collection(args.get("name", ""))
                    app.settings["last_collection"] = name
                    write_json(app.settings_path, app.settings)
                    return self._send(200, {"ok": True, "name": name})
                if url.path == "/api/collection/rename":
                    name = app.library.rename_collection(args.get("name", ""), args.get("new", ""))
                    return self._send(200, {"ok": True, "name": name})
                if url.path == "/api/collection/delete":
                    app.library.delete_collection(args.get("name", ""))
                    return self._send(200, {"ok": True})
            if url.path == "/api/choose-folder":
                path = app.choose_folder()
                if path:
                    app.save_settings({"library": path})
                return self._send(200, {"ok": True, "path": path or ""})
            if url.path == "/api/reveal":
                app.reveal()
                return self._send(200, {"ok": True})
            if url.path == "/api/quit":
                self._send(200, {"ok": True})
                app.quit.set()
                threading.Thread(target=self.server.shutdown, daemon=True).start()
                return None
            return self._send(404, {"ok": False, "error": "not found"})
        except Busy as e:
            return self._send(409, {"ok": False, "error": str(e)})
        except (LibraryError, OSError, ValueError) as e:
            return self._send(400, {"ok": False, "error": str(e)})


def already_running(port):
    try:
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        with opener.open("http://127.0.0.1:%d/api/ping" % port, timeout=1.5) as r:
            return json.loads(r.read().decode("utf-8")).get("app") == APP
    except Exception:                         # noqa: BLE001 - anything else on that port is not us
        return False


def serve(app, port):
    """The server on 127.0.0.1:port (another free port if that one is taken)."""
    handler = type("BoundHandler", (Handler,), {"app": app})
    try:
        httpd = http.server.ThreadingHTTPServer(("127.0.0.1", port), handler)
    except OSError:
        httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    httpd.daemon_threads = True
    return httpd


def main(argv=None):
    ap = argparse.ArgumentParser(prog=APP, description="Put MIDI files on the MPC for Sight Reader, from a page in your browser.")
    ap.add_argument("--port", type=int, help="the page's port on this computer (default %d; remembered)" % DEFAULT_PORT)
    ap.add_argument("--library", help="the library folder on this computer (remembered)")
    ap.add_argument("--no-browser", action="store_true", help="do not open the browser")
    ap.add_argument("--version", action="version", version="%s %s" % (APP, VERSION))
    args = ap.parse_args(argv)
    app = App()
    if args.library:
        app.save_settings({"library": args.library})
    if args.port:
        app.save_settings({"port": args.port})
    port = args.port if args.port is not None else int(app.settings.get("port") or DEFAULT_PORT)
    if port and already_running(port):
        url = "http://127.0.0.1:%d/" % port
        print("The loader is running already: %s" % url, flush=True)
        if not args.no_browser:
            webbrowser.open(url)
        return 0
    httpd = serve(app, port)
    url = "http://127.0.0.1:%d/" % httpd.server_address[1]
    print("Sight Reader MIDI Loader %s" % VERSION)
    print("  Page:     %s   (only this computer can open it)" % url)
    print("  Library:  %s" % app.library_root())
    print("  Settings: %s" % app.cfg_dir)
    print("Leave this window open while you use the page. Stop: Quit on the page, or Ctrl+C here.", flush=True)
    if not args.no_browser:
        threading.Timer(0.4, webbrowser.open, (url,)).start()
    try:
        httpd.serve_forever(poll_interval=0.5)
    except KeyboardInterrupt:
        print()
    finally:
        httpd.server_close()
        app.close()
    print("Stopped.", flush=True)
    return 0


PAGE = r"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="loader-token" content="__TOKEN__">
<title>Sight Reader MIDI Loader</title>
<link rel="icon" href="data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 64 64'%3E%3Crect width='64' height='64' rx='14' fill='%230f766e'/%3E%3Cpath d='M41 13v27a8 8 0 1 1-4-6.9V22.6l-13 3.3V45a8 8 0 1 1-4-6.9V19z' fill='%23fff'/%3E%3C/svg%3E">
<style nonce="__NONCE__">
:root {
  color-scheme: light dark;
  --bg: #f4f5f7; --card: #ffffff; --ink: #1a2130; --muted: #5f6878; --line: #e2e5ea; --soft: #f1f3f6;
  --accent: #0f766e; --accent-ink: #ffffff; --accent-soft: #e2f2ef; --accent-line: #a7d8d1;
  --warn: #9a5a06; --warn-soft: #fdf2de; --bad: #b42318; --bad-soft: #fdeceb; --ok: #12704f; --ok-soft: #e2f4ec;
  --shadow: 0 1px 2px rgba(16, 24, 40, .05), 0 2px 8px rgba(16, 24, 40, .06);
  --radius: 12px;
  --font: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif;
  --mono: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
}
@media (prefers-color-scheme: dark) {
  :root {
    --bg: #0e1116; --card: #161a21; --ink: #e8eaef; --muted: #9ba4b4; --line: #282e39; --soft: #1c212a;
    --accent: #2fb5a5; --accent-ink: #04211e; --accent-soft: #12332f; --accent-line: #22665e;
    --warn: #f1b45c; --warn-soft: #36290f; --bad: #f28b82; --bad-soft: #3a1c1a; --ok: #63d3a7; --ok-soft: #133226;
    --shadow: none;
  }
}
* { box-sizing: border-box; }
html, body { margin: 0; }
body { background: var(--bg); color: var(--ink); font: 15px/1.45 var(--font); padding-bottom: 92px; }
button, input, select { font: inherit; color: inherit; }
h1, h2, h3 { margin: 0; line-height: 1.2; }
h1 { font-size: 18px; font-weight: 650; letter-spacing: -.01em; }
h2 { font-size: 15px; font-weight: 650; }
h3 { font-size: 14px; font-weight: 650; }
p { margin: 0; }
.muted { color: var(--muted); }
.small { font-size: 13px; }
.mono { font-family: var(--mono); font-size: 12.5px; }
[hidden] { display: none !important; }

/* top bar */
.top { position: sticky; top: 0; z-index: 5; display: flex; align-items: center; gap: 14px; padding: 12px 20px;
  background: var(--bg); background: color-mix(in srgb, var(--bg) 88%, transparent); backdrop-filter: blur(8px); border-bottom: 1px solid var(--line); }
.brand { display: flex; align-items: center; gap: 10px; margin-right: auto; min-width: 0; }
.brand svg { flex: none; }
.brand .sub { font-size: 12.5px; color: var(--muted); }
.pill { display: inline-flex; align-items: center; gap: 8px; padding: 6px 12px; border-radius: 999px; font-size: 13px;
  background: var(--card); border: 1px solid var(--line); white-space: nowrap; max-width: 46vw; overflow: hidden; text-overflow: ellipsis; }
.pill .dot { width: 8px; height: 8px; border-radius: 50%; background: var(--muted); flex: none; }
.pill.ok .dot { background: var(--ok); }
.pill.warn .dot { background: var(--warn); }
.pill.bad .dot { background: var(--bad); }
.pill.busy .dot { background: var(--accent); animation: pulse 1s infinite ease-in-out; }
@keyframes pulse { 50% { opacity: .25; } }

/* buttons */
.btn { display: inline-flex; align-items: center; justify-content: center; gap: 6px; border: 1px solid var(--line); background: var(--card);
  border-radius: 9px; padding: 7px 13px; cursor: pointer; font-weight: 550; font-size: 14px; white-space: nowrap; }
.btn:hover { background: var(--soft); }
.btn:focus-visible, .piece-main:focus-visible, input:focus-visible, select:focus-visible, summary:focus-visible { outline: 2px solid var(--accent); outline-offset: 2px; }
.btn.primary { background: var(--accent); border-color: var(--accent); color: var(--accent-ink); }
.btn.primary:hover { filter: brightness(1.06); }
.btn.danger { background: var(--bad); border-color: var(--bad); color: #fff; }
.btn.danger-ghost { color: var(--bad); }
.btn.ghost { background: transparent; border-color: transparent; }
.btn.ghost:hover { background: var(--soft); }
.btn.small { padding: 4px 9px; font-size: 13px; border-radius: 7px; }
.btn:disabled { opacity: .45; cursor: default; filter: none; }

/* layout */
main { max-width: 1240px; margin: 0 auto; padding: 18px 20px; display: grid; gap: 18px; }
.cols { display: grid; grid-template-columns: minmax(0, 1.65fr) minmax(0, 1fr); gap: 18px; align-items: start; }
@media (max-width: 900px) { .cols { grid-template-columns: 1fr; } .top { flex-wrap: wrap; } .pill { max-width: 100%; } }
.card { background: var(--card); border: 1px solid var(--line); border-radius: var(--radius); box-shadow: var(--shadow); }
.card-head { display: flex; align-items: center; gap: 10px; flex-wrap: wrap; padding: 14px 16px; border-bottom: 1px solid var(--line); }
.card-head .grow { flex: 1; min-width: 0; }
.card-body { padding: 14px 16px; }
.path { display: block; color: var(--muted); font: 12px/1.4 var(--mono); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }

/* settings */
.settings form { display: grid; gap: 14px; }
.grid { display: grid; grid-template-columns: repeat(4, minmax(0, 1fr)); gap: 12px 14px; }
.grid .wide { grid-column: span 2; }
.grid .full { grid-column: 1 / -1; }
@media (max-width: 900px) { .grid { grid-template-columns: 1fr 1fr; } .grid .wide, .grid .full { grid-column: 1 / -1; } }
label { display: grid; grid-template-rows: 1fr auto; gap: 5px; font-size: 13px; font-weight: 600; }
label .lbl { align-self: end; }
label .opt { font-weight: 400; color: var(--muted); }
input[type=text], input[type=password], input:not([type]), select {
  width: 100%; padding: 8px 10px; border: 1px solid var(--line); border-radius: 8px; background: var(--bg); font-weight: 400; font-size: 14px; }
.row { display: flex; gap: 8px; align-items: center; }
.row > input { flex: 1; min-width: 0; }
.form-actions { display: flex; gap: 10px; align-items: center; flex-wrap: wrap; }
details.help { background: var(--soft); border-radius: 9px; padding: 10px 12px; font-size: 13.5px; }
details.help summary { cursor: pointer; font-weight: 600; }
details.help ol { margin: 8px 0 2px; padding-left: 20px; display: grid; gap: 5px; }

/* collections and pieces */
.colls { display: grid; gap: 12px; padding: 14px 16px 16px; }
.coll { border: 1px solid var(--line); border-radius: 10px; background: var(--card); transition: border-color .12s, background .12s; }
.coll > header { display: flex; align-items: center; gap: 8px; padding: 9px 10px 9px 14px; border-bottom: 1px solid var(--line); }
.coll > header h3 { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.coll > header .count { color: var(--muted); font-size: 13px; margin-right: auto; white-space: nowrap; }
.coll.over { border-color: var(--accent); background: var(--accent-soft); }
.pieces { list-style: none; margin: 0; padding: 4px 0; }
.piece { display: flex; align-items: center; gap: 10px; padding: 0 12px 0 4px; }
.piece + .piece { border-top: 1px solid var(--soft); }
.piece-main { flex: 1; min-width: 0; display: grid; gap: 1px; text-align: left; border: 0; background: none; padding: 7px 10px; border-radius: 8px; cursor: pointer; }
.piece-main:hover { background: var(--soft); }
.piece-main .t { font-weight: 560; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.piece-main .m, .piece-main .i { font-size: 12.5px; color: var(--muted); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.piece-main .i.problem { color: var(--bad); }
.badge { flex: none; font-size: 12px; font-weight: 600; padding: 2px 9px; border-radius: 999px; background: var(--soft); color: var(--muted); white-space: nowrap; }
.badge.new { background: var(--accent-soft); color: var(--accent); }
.badge.changed { background: var(--warn-soft); color: var(--warn); }
.badge.same { background: var(--ok-soft); color: var(--ok); }
.badge.bad { background: var(--bad-soft); color: var(--bad); }
.drop-hint { margin: 0 16px 16px; padding: 18px; border: 1.5px dashed var(--line); border-radius: 10px; text-align: center; color: var(--muted); font-size: 13.5px; }
body.dragging .drop-hint { border-color: var(--accent); color: var(--accent); background: var(--accent-soft); }
.empty { padding: 26px 16px; text-align: center; color: var(--muted); }
.empty b { color: var(--ink); display: block; margin-bottom: 4px; font-size: 15px; }

/* the MPC */
.facts { display: grid; grid-template-columns: auto minmax(0, 1fr); gap: 6px 14px; margin: 0; font-size: 14px; }
.facts dt { color: var(--muted); }
.facts dd { margin: 0; overflow-wrap: anywhere; }
.alert { border-radius: 9px; padding: 10px 12px; font-size: 13.5px; display: grid; gap: 4px; background: var(--soft); }
.alert b { font-weight: 650; }
.alert.bad { background: var(--bad-soft); color: var(--bad); }
.alert.warn { background: var(--warn-soft); color: var(--warn); }
.alert.ok { background: var(--ok-soft); color: var(--ok); }
.alert p { color: var(--ink); }
.stack { display: grid; gap: 12px; }
.mpc-only { list-style: none; margin: 0; padding: 0; border: 1px solid var(--line); border-radius: 9px; max-height: 300px; overflow: auto; }
.mpc-only li + li { border-top: 1px solid var(--soft); }
.mpc-only label { display: flex; align-items: center; gap: 10px; font-weight: 400; padding: 7px 10px; cursor: pointer; font-size: 14px; }
.mpc-only label .m { color: var(--muted); font-size: 12.5px; margin-left: auto; white-space: nowrap; }
.mpc-only .t { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.mpc-actions { display: flex; flex-wrap: wrap; gap: 8px; }
.spin { width: 14px; height: 14px; border-radius: 50%; border: 2px solid var(--line); border-top-color: var(--accent); animation: rot .8s linear infinite; flex: none; }
@keyframes rot { to { transform: rotate(360deg); } }

/* the sync bar */
.syncbar { position: fixed; left: 0; right: 0; bottom: 0; z-index: 6; background: var(--card); border-top: 1px solid var(--line);
  box-shadow: 0 -4px 16px rgba(16, 24, 40, .06); }
.syncbar .inner { max-width: 1240px; margin: 0 auto; padding: 12px 20px; display: flex; align-items: center; gap: 14px; }
.syncbar .summary { margin-right: auto; display: flex; flex-wrap: wrap; gap: 6px 14px; align-items: center; min-width: 0; }
.syncbar .summary .n { font-weight: 650; }
.syncbar .btn.primary { padding: 10px 20px; font-size: 15px; }
.syncbar .quiet { font-size: 13px; }

/* dialogs */
.modal { position: fixed; inset: 0; z-index: 20; display: grid; place-items: center; padding: 20px; background: rgba(10, 14, 20, .45); }
.dialog { width: min(560px, 100%); max-height: calc(100vh - 40px); overflow: auto; background: var(--card); border: 1px solid var(--line);
  border-radius: 14px; box-shadow: 0 20px 50px rgba(0, 0, 0, .25); padding: 20px; display: grid; gap: 14px; }
.dialog h2 { font-size: 17px; }
.dialog .actions { display: flex; gap: 8px; justify-content: flex-end; flex-wrap: wrap; }
.dialog .actions .left { margin-right: auto; }
.dialog ul.plain { margin: 0; padding-left: 20px; display: grid; gap: 4px; }
.dialog .scroll { max-height: 180px; overflow: auto; border: 1px solid var(--line); border-radius: 9px; padding: 8px 10px 8px 28px; margin: 0; font-size: 13.5px; }
.check { display: flex; gap: 8px; align-items: flex-start; font-weight: 500; font-size: 14px; }
.check input { margin-top: 3px; }
.steps { list-style: none; margin: 0; padding: 0; display: grid; gap: 8px; }
.steps li { display: flex; gap: 10px; align-items: center; }
.steps li.done::before { content: "✓"; color: var(--ok); font-weight: 700; width: 14px; text-align: center; }
.steps li.now::before { content: ""; width: 14px; height: 14px; border-radius: 50%; border: 2px solid var(--line); border-top-color: var(--accent); animation: rot .8s linear infinite; }

.dropmask { position: fixed; inset: 0; z-index: 15; pointer-events: none; border: 3px solid var(--accent); border-radius: 6px; }
.dropmask span { position: absolute; left: 50%; top: 18px; transform: translateX(-50%); background: var(--accent); color: var(--accent-ink); padding: 7px 16px;
  border-radius: 999px; font-weight: 600; font-size: 14px; box-shadow: var(--shadow); }
.toast { position: fixed; left: 50%; bottom: 96px; transform: translateX(-50%); z-index: 25; max-width: min(640px, calc(100vw - 32px));
  background: var(--ink); color: var(--bg); padding: 10px 16px; border-radius: 10px; font-size: 14px; box-shadow: 0 8px 24px rgba(0, 0, 0, .2); }
.stopped { position: fixed; inset: 0; z-index: 40; display: grid; place-items: center; background: var(--bg); padding: 24px; text-align: center; }
.stopped > div { max-width: 520px; display: grid; gap: 10px; }
</style>
</head>
<body>
<header class="top">
  <div class="brand">
    <svg width="34" height="34" viewBox="0 0 64 64" aria-hidden="true"><rect width="64" height="64" rx="14" fill="#0f766e"/><path d="M41 13v27a8 8 0 1 1-4-6.9V22.6l-13 3.3V45a8 8 0 1 1-4-6.9V19z" fill="#fff"/></svg>
    <div><h1>Sight Reader MIDI Loader</h1><p class="sub">Your MIDI files, from this computer to Sight Reader on the MPC</p></div>
  </div>
  <span class="pill" id="pill" role="status"><span class="dot"></span><span id="pill-text">Starting…</span></span>
  <button class="btn ghost" id="btn-settings" type="button">Settings</button>
</header>

<main>
  <section class="card settings" id="settings" hidden aria-labelledby="settings-title">
    <div class="card-head"><div class="grow"><h2 id="settings-title">Settings</h2>
      <p class="muted small" id="settings-lede">Where your MPC is, and where the library is kept. Remembered on this computer only.</p></div>
      <button class="btn ghost small" type="button" data-act="close-settings" id="btn-close-settings">Close</button></div>
    <div class="card-body">
      <form id="settings-form" autocomplete="off" novalidate>
        <div class="grid">
          <label class="wide"><span class="lbl">MPC address <span class="opt">its IP address</span></span>
            <input name="address" id="f-address" spellcheck="false" autocapitalize="off" placeholder="the MPC's IP address"></label>
          <label><span class="lbl">SSH user</span><input name="user" spellcheck="false" autocapitalize="off" placeholder="root"></label>
          <label><span class="lbl">SSH port</span><input name="ssh_port" inputmode="numeric" placeholder="22"></label>
          <label class="wide"><span class="lbl">Password <span class="opt">only if your MPC asks for one; kept until the loader stops, never saved</span></span>
            <input name="password" type="password" autocomplete="new-password"></label>
          <label class="wide"><span class="lbl">Drive on the MPC <span class="opt">leave empty: the drive Sight Reader is installed on</span></span>
            <input name="drive" spellcheck="false" autocapitalize="off" placeholder="found by itself"></label>
          <label class="full"><span class="lbl">Library folder on this computer</span>
            <span class="row"><input name="library" spellcheck="false" autocapitalize="off">
              <button class="btn" type="button" data-act="choose" id="btn-choose">Choose…</button>
              <button class="btn" type="button" data-act="reveal">Show</button></span></label>
        </div>
        <details class="help">
          <summary>Setting up the MPC</summary>
          <ol>
            <li>MPC OS 3.10.1 or later, with Preferences &gt; Enhancements (Beta) &gt; Enable Enhancements and External Plugin Support on, and Sight Reader installed (see the README).</li>
            <li>Developer mode on, which starts the MPC's SSH server: Preferences &gt; Info, tap the version value 3 times, then the serial number value 2 times, then the firmware version once.</li>
            <li>The MPC and this computer on the same network. The MPC shows its IP address in Preferences: the Wi-Fi tab (hold Shift to reach it) or the Ethernet tab. Your router's device list has it too.</li>
          </ol>
        </details>
        <div class="form-actions">
          <button class="btn primary" type="submit">Save and connect</button>
          <span class="muted small" id="settings-note"></span>
        </div>
      </form>
    </div>
  </section>

  <div class="cols">
    <section class="card" id="local" aria-labelledby="local-title">
      <div class="card-head">
        <div class="grow"><h2 id="local-title">On this computer</h2><span class="path" id="lib-path"></span></div>
        <button class="btn primary" type="button" data-act="add">Add MIDI files…</button>
        <button class="btn" type="button" data-act="new-coll">New collection</button>
        <button class="btn ghost" type="button" data-act="reveal">Show folder</button>
      </div>
      <div id="collections" class="colls"></div>
      <div class="drop-hint" id="drop-hint">Drop .mid files anywhere on this page, or onto a collection to put them in it.</div>
    </section>

    <section class="card" id="mpc" aria-labelledby="mpc-title">
      <div class="card-head">
        <div class="grow"><h2 id="mpc-title">On the MPC</h2></div>
        <button class="btn small" type="button" data-act="look" id="btn-look">Check again</button>
      </div>
      <div class="card-body stack" id="mpc-body"></div>
      <div class="card-body stack">
        <div class="mpc-actions">
          <button class="btn danger-ghost" type="button" data-act="restart" id="btn-restart">Restart the MPC app…</button>
        </div>
        <p class="muted small">Restarting closes and reopens the MPC application (about a minute), as from the power button but without switching off. Anything unsaved is lost.</p>
      </div>
    </section>
  </div>
</main>

<footer class="syncbar">
  <div class="inner">
    <div class="summary" id="sync-summary"></div>
    <button class="btn ghost quiet" type="button" data-act="quit">Quit loader</button>
    <button class="btn primary" type="button" data-act="sync" id="btn-sync" disabled>Sync to the MPC</button>
  </div>
</footer>

<div class="modal" id="modal" hidden><div class="dialog" role="dialog" aria-modal="true" id="dialog"></div></div>
<div class="dropmask" id="dropmask" hidden><span>Drop to add to your library</span></div>
<div class="toast" id="toast" role="status" hidden></div>
<input type="file" id="file-input" multiple accept=".mid,.midi,audio/midi,audio/x-midi" hidden>
<div class="stopped" id="stopped" hidden><div><h2 id="stopped-title">The loader has stopped</h2>
  <p class="muted" id="stopped-text">You can close this tab. To start it again, double-click mpc-sightreading-midi-loader.command in the loader folder (or run python3 loader/mpc-sightreading-midi-loader.py), then reload this page.</p>
  <p><button class="btn" type="button" id="btn-reload">Reload</button></p></div></div>

<script nonce="__NONCE__">
"use strict";
const TOKEN = document.querySelector('meta[name="loader-token"]').content;
const $ = (s, el) => (el || document).querySelector(s);
const $$ = (s, el) => Array.from((el || document).querySelectorAll(s));
const esc = s => String(s == null ? "" : s).replace(/[&<>"']/g, c => ({"&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;"}[c]));
const plural = (n, one, many) => n + " " + (n === 1 ? one : (many || one + "s"));
let S = null, lastJson = "", pollTimer = null, stopped = false, formDirty = false, jobOpen = null, firstLook = false;
const picked = new Set();

async function api(path, body, raw) {
  const opt = {method: body === undefined ? "GET" : "POST", headers: {"X-Loader-Token": TOKEN}};
  if (body !== undefined) {
    if (raw) { opt.body = body; opt.headers["Content-Type"] = "application/octet-stream"; }
    else { opt.body = JSON.stringify(body); opt.headers["Content-Type"] = "application/json"; }
  }
  let r;
  try { r = await fetch(path, opt); }
  catch (e) { const err = new Error("The loader is not answering. Is its window still open?"); err.offline = true; throw err; }
  let j = {};
  try { j = await r.json(); } catch (e) { /* not JSON */ }
  if (!r.ok || j.ok === false) throw new Error(j.error || ("The loader answered " + r.status));
  return j;
}

// ---- state -------------------------------------------------------------------------------------
async function refresh() {
  if (stopped) return;
  try {
    const st = await api("/api/state");
    $("#stopped").hidden = true;
    // redraw when something shown changed (a running job's clock only moves its own dialog)
    const j = st.job;
    const json = JSON.stringify(Object.assign({}, st, {job: j ? [j.id, j.state, j.kind] : null}));
    S = st;
    if (json !== lastJson) { lastJson = json; render(); }
    followJob();
    if (!firstLook) {
      firstLook = true;
      if (S.settings.address && !S.mpc && !(S.job && S.job.state === "running")) look(true);
    }
  } catch (e) {
    if (e.offline) showStopped("Can't reach the loader", "Its window may have been closed. Start it again (double-click mpc-sightreading-midi-loader.command, or run python3 loader/mpc-sightreading-midi-loader.py), then reload this page.");
  }
}
function schedule() {
  clearTimeout(pollTimer);
  if (stopped) return;
  const running = S && S.job && S.job.state === "running";
  pollTimer = setTimeout(async () => { await refresh(); schedule(); }, running ? 450 : (document.hidden ? 8000 : 2500));
}
document.addEventListener("visibilitychange", () => { if (!document.hidden) { refresh(); schedule(); } });
const busy = () => !!(S && S.job && S.job.state === "running");

// ---- drawing -----------------------------------------------------------------------------------
function render() {
  if (!S) return;
  renderPill(); renderSettings(); renderLibrary(); renderMpc(); renderSync();
}
function renderPill() {
  const pill = $("#pill"), m = S.mpc, j = S.job;
  let cls = "", text;
  if (!S.settings.address) text = "MPC not set up";
  else if (j && j.state === "running" && j.kind === "look") { cls = "busy"; text = "Looking at the MPC…"; }
  else if (j && j.state === "running") { cls = "busy"; text = j.title + "…"; }
  else if (!m) { cls = "busy"; text = "Not checked yet"; }
  else if (m.error) { cls = "bad"; text = "Can't reach the MPC"; }
  else if (!m.plugin) { cls = "warn"; text = "MPC found, Sight Reader missing"; }
  else { cls = "ok"; text = (m.model || "MPC") + " · Sight Reader " + (m.plugin.version || ""); }
  pill.className = "pill " + cls;
  $("#pill-text").textContent = text;
  pill.title = S.settings.address ? "MPC at " + S.settings.address : "";
}
function renderSettings() {
  const f = $("#settings-form");
  const first = !S.settings.address;
  if (first) $("#settings").hidden = false;
  $("#btn-close-settings").hidden = first;
  $("#settings-lede").textContent = first
    ? "First, where is your MPC? Enter its IP address. It is remembered on this computer only, so you enter it once."
    : "Where your MPC is, and where the library is kept. Remembered on this computer only.";
  $("#btn-choose").hidden = !S.can_choose_folder;
  if (formDirty) return;
  f.address.value = S.settings.address;
  f.user.value = S.settings.user;
  f.ssh_port.value = S.settings.ssh_port;
  f.drive.value = S.settings.drive;
  f.library.value = S.library.root;
  f.password.value = "";
  f.password.placeholder = S.has_password ? "(entered; leave empty to keep it)" : "";
}
function infoLine(p) {
  if (p.problem) return p.problem;
  const i = p.info;
  if (!i || i.problem) return i && i.problem ? "Read with a problem: " + i.problem : "";
  const parts = [];
  if (i.seconds) { const t = Math.round(i.seconds); parts.push(Math.floor(t / 60) + ":" + String(t % 60).padStart(2, "0")); }
  if (i.low) parts.push(i.low === i.high ? i.low : i.low + "–" + i.high);
  parts.push(plural(i.notes, "note"));
  if (i.tracks > 1) parts.push(plural(i.tracks, "track"));
  if (!i.notes) parts.push("no notes to play");
  if (i.drums_only) parts.push("drums only");
  return parts.join(" · ");
}
const BADGES = {add: ["New", "new"], replace: ["Changed", "changed"], same: ["On the MPC", "same"], problem: ["Can't copy", "bad"]};
function renderLibrary() {
  const L = S.library;
  $("#lib-path").textContent = L.root;
  $("#lib-path").title = L.root;
  const box = $("#collections");
  if (L.error) { box.innerHTML = `<div class="alert bad"><b>Can't read the library folder</b><p>${esc(L.error)}</p></div>`; return; }
  if (!L.collections.length) {
    box.innerHTML = `<div class="empty"><b>No pieces yet</b>Drop .mid files on this page, or click Add MIDI files. They go into collections: a collection is a folder in Sight Reader's LOAD list.</div>`;
  } else {
    box.innerHTML = L.collections.map(c => `
      <section class="coll" data-coll="${esc(c.name)}">
        <header><h3>${esc(c.name)}</h3><span class="count">${plural(c.pieces.length, "piece")}</span>
          <button class="btn ghost small" type="button" data-act="add-to">Add…</button>
          <button class="btn ghost small" type="button" data-act="rename-coll">Rename</button>
          <button class="btn ghost small danger-ghost" type="button" data-act="delete-coll">Delete</button></header>
        ${c.pieces.length ? `<ul class="pieces">${c.pieces.map(p => {
          const b = BADGES[p.status];
          const meta = [p.composer, p.level ? "Level " + p.level : ""].filter(Boolean).join(" · ");
          return `<li class="piece" data-coll="${esc(c.name)}" data-file="${esc(p.file)}">
            <button class="piece-main" type="button" data-act="edit" title="${esc(c.name + "/" + p.file)}">
              <span class="t">${esc(p.title)}</span>
              ${meta ? `<span class="m">${esc(meta)}</span>` : ""}
              <span class="i${p.problem ? " problem" : ""}">${esc(infoLine(p))}</span>
            </button>
            ${b ? `<span class="badge ${b[1]}">${b[0]}</span>` : ""}</li>`;
        }).join("")}</ul>` : `<p class="muted small empty">Empty. Drop .mid files here.</p>`}
      </section>`).join("");
  }
  if (L.stray && L.stray.length) {
    box.insertAdjacentHTML("beforeend", `<div class="alert warn"><b>${plural(L.stray.length, "file")} outside a collection</b>
      <p>Sight Reader lists pieces by collection, so ${L.stray.length === 1 ? "this file is" : "these files are"} not copied: ${esc(L.stray.slice(0, 4).join(", "))}${L.stray.length > 4 ? "…" : ""}. Move ${L.stray.length === 1 ? "it" : "them"} into a collection folder (Show folder).</p></div>`);
  }
}
function clock(when) {
  const d = new Date(when * 1000), mins = (Date.now() / 1000 - when) / 60;
  return mins < 1 ? "just now" : "at " + d.toLocaleTimeString([], {hour: "2-digit", minute: "2-digit"});
}
function renderMpc() {
  const m = S.mpc, set = S.settings, j = S.job, body = $("#mpc-body");
  const looking = j && j.state === "running" && j.kind === "look";
  $("#btn-look").disabled = !set.address || busy();
  $("#btn-restart").disabled = !set.address || busy();
  if (!set.address) { body.innerHTML = `<p class="muted">Enter the MPC's address in Settings to connect.</p>`; return; }
  if (looking && (!m || m.error)) { body.innerHTML = `<div class="row"><span class="spin"></span><span>Looking at the MPC at ${esc(set.address)}…</span></div>`; return; }
  if (!m) { body.innerHTML = `<p class="muted">Not checked yet.</p>`; return; }
  if (m.error) {
    body.innerHTML = `<div class="alert bad"><b>${esc(m.error.title)}</b>${m.error.lines.map(l => `<p>${esc(l)}</p>`).join("")}</div>
      <p class="muted small">Address: ${esc(set.address)} · checked ${clock(m.when)}. Change it in Settings, or press Check again.</p>`;
    return;
  }
  let html = `<dl class="facts">
      <dt>MPC</dt><dd>${esc(m.model || "MPC")} at ${esc(set.address)}</dd>
      <dt>Sight Reader</dt><dd>${m.plugin ? esc(m.plugin.version || "installed") + " on " + esc(m.drive) : "not found"}</dd>
      ${m.lib_path ? `<dt>Library</dt><dd>${plural(m.pieces, "piece")} <span class="muted mono">${esc(m.lib_path)}</span></dd>` : ""}
      <dt>Checked</dt><dd>${looking ? "now…" : clock(m.when)}</dd></dl>`;
  if (m.problem) html += `<div class="alert warn"><b>${esc(m.problem)}</b></div>`;
  if (m.plugin && !m.auto_rescan) html += `<div class="alert warn"><b>This Sight Reader is older than 0.6.0</b><p>After a sync, press RESCAN on its LOAD page. Version 0.6.0 and later pick changes up by themselves.</p></div>`;
  const only = m.mpc_only || [];
  for (const r of Array.from(picked)) if (!only.some(x => x.rel === r)) picked.delete(r);
  if (only.length) {
    html += `<div class="stack"><div><h3>Only on the MPC <span class="muted">(${only.length})</span></h3>
      <p class="muted small">Put there some other way: tools/library.py, another computer, or by hand. Sync leaves them alone.</p></div>
      <ul class="mpc-only">${only.map(x => `<li><label><input type="checkbox" data-rel="${esc(x.rel)}"${picked.has(x.rel) ? " checked" : ""}>
        <span class="t">${esc(x.title)}</span><span class="m">${esc(x.coll)}</span></label></li>`).join("")}</ul>
      <div class="mpc-actions">
        <button class="btn small" type="button" data-act="pick-all">${picked.size === only.length ? "Select none" : "Select all"}</button>
        <button class="btn small" type="button" data-act="pull" ${picked.size && !busy() ? "" : "disabled"}>Copy to this computer</button>
        <button class="btn small danger-ghost" type="button" data-act="remote-delete" ${picked.size && !busy() ? "" : "disabled"}>Delete from the MPC…</button>
      </div></div>`;
  }
  body.innerHTML = html;
}
function renderSync() {
  const p = S.plan, m = S.mpc, box = $("#sync-summary"), btn = $("#btn-sync");
  let html, can = false;
  if (!S.settings.address) html = `<span class="muted">Set up the MPC in Settings to sync.</span>`;
  else if (!m || (busy() && S.job.kind === "look")) html = `<span class="muted">Looking at the MPC…</span>`;
  else if (m.error) html = `<span class="muted">Not connected to the MPC.</span>`;
  else if (!p) html = `<span class="muted">${esc(m.problem || "Nothing to sync with yet.")}</span>`;
  else if (p.empty) html = `<span>✓ The MPC has all your pieces.</span>`;
  else {
    const bits = [];
    if (p.add) bits.push(`<span><span class="n">${p.add}</span> new</span>`);
    if (p.replace) bits.push(`<span><span class="n">${p.replace}</span> changed</span>`);
    if (p.remove.length) bits.push(`<span><span class="n">${p.remove.length}</span> to remove</span>`);
    if (p.titles && !p.add && !p.replace) bits.push(`<span>titles to update</span>`);
    if (p.upload) bits.push(`<span class="muted small">${esc(p.upload_text)} to copy</span>`);
    html = bits.join("");
    can = true;
  }
  box.innerHTML = html;
  btn.disabled = !can || busy();
}

// ---- dialogs -------------------------------------------------------------------------------------
let modalLocked = false, modalResolve = null;
function modal(html, buttons, opts) {
  opts = opts || {};
  const d = $("#dialog");
  d.innerHTML = html + (buttons && buttons.length ? `<div class="actions">${buttons.map((b, i) =>
    `<button class="btn ${b[1] || ""}" type="button" data-modal="${i}">${esc(b[0])}</button>`).join("")}</div>` : "");
  modalLocked = !!opts.locked;
  d._buttons = buttons || [];
  $("#modal").hidden = false;
  const focus = $("input:not([type=checkbox]), select", d) || $(".btn.primary, .btn.danger", d) || $(".btn", d);
  if (focus && !opts.noFocus) focus.focus();
}
function closeModal() { $("#modal").hidden = true; $("#dialog").innerHTML = ""; modalLocked = false; jobOpen = null; if (modalResolve) { const r = modalResolve; modalResolve = null; r(null); } }
$("#modal").addEventListener("click", async e => {
  const b = e.target.closest("[data-modal]");
  if (b) {
    const def = $("#dialog")._buttons[+b.dataset.modal];
    if (def && def[2]) {
      b.disabled = true;
      try { const keep = await def[2](); if (keep !== true) closeModal(); }
      catch (err) { b.disabled = false; toast(err.message); }
    } else closeModal();
    return;
  }
  if (e.target.id === "modal" && !modalLocked) closeModal();
});
document.addEventListener("keydown", e => {
  if (e.key === "Escape" && !$("#modal").hidden && !modalLocked) closeModal();
  if (e.key === "Enter" && !$("#modal").hidden && e.target.matches("#dialog input:not([type=checkbox])")) {
    e.preventDefault(); const p = $("#dialog .btn.primary, #dialog .btn.danger"); if (p) p.click();
  }
});
let toastTimer = null;
function toast(text, stay) {
  const t = $("#toast"); t.textContent = text; t.hidden = false;
  clearTimeout(toastTimer); if (!stay) toastTimer = setTimeout(() => { t.hidden = true; }, 5200);
}
function showStopped(title, text) {
  stopped = true;
  $("#stopped-title").textContent = title; $("#stopped-text").textContent = text; $("#stopped").hidden = false;
}

// ---- jobs: the MPC work, followed step by step -------------------------------------------------------
async function startJob(path, body) {
  const r = await api(path, body || {});
  jobOpen = r.job.id;
  S.job = r.job; drawJob(r.job);
  schedule();
  return true;
}
function drawJob(job) {
  const done = job.state !== "running";
  const steps = job.steps.map((s, i) => `<li class="${i < job.steps.length - 1 || done ? "done" : "now"}">${esc(s)}</li>`).join("");
  const result = done ? `<div class="alert ${job.ok ? "ok" : "bad"}"><b>${esc(job.message)}</b>${job.lines.map(l => `<p>${esc(l)}</p>`).join("")}</div>`
    : `<p class="muted small">${job.kind === "restart" ? "The MPC app takes about a minute to come back. " : ""}${Math.round(job.elapsed)} s</p>`;
  modal(`<h2>${esc(job.title)}</h2><ol class="steps">${steps || `<li class="now">Starting</li>`}</ol>${result}`,
    done ? [["OK", "primary"]] : [], {locked: !done, noFocus: !done});
  jobOpen = job.id;
}
function followJob() {
  const j = S && S.job;
  if (!j || !jobOpen || j.id !== jobOpen) return;
  drawJob(j);
  if (j.state !== "running") { jobOpen = null; modalLocked = false; }
}
async function look(quiet) {
  try { const r = await api("/api/look", {}); S.job = r.job; render(); schedule(); }
  catch (e) { if (!quiet) toast(e.message); }
}

// ---- adding files ------------------------------------------------------------------------------------
function chooseCollection(count) {
  const colls = S.library.collections.map(c => c.name);
  const last = colls.includes(S.settings.last_collection) ? S.settings.last_collection : (colls[0] || "");
  return new Promise(resolve => {
    modal(`<h2>Add ${plural(count, "file")} to which collection?</h2>
      <p class="muted small">A collection is a folder in Sight Reader's LOAD list.</p>
      ${colls.length ? `<label>Collection<select id="m-coll">${colls.map(c => `<option${c === last ? " selected" : ""}>${esc(c)}</option>`).join("")}
        <option value="">A new collection…</option></select></label>` : ""}
      <label id="m-new-wrap"${colls.length ? " hidden" : ""}>New collection<input id="m-new" maxlength="48" placeholder="for example My Pieces"></label>`,
      [["Cancel"], ["Add", "primary", () => {
        const sel = $("#m-coll"), name = sel && sel.value ? sel.value : $("#m-new").value.trim();
        if (!name) { toast("Give the collection a name."); return true; }
        const r = modalResolve; modalResolve = null; r(name);
      }]]);
    modalResolve = resolve;
    const sel = $("#m-coll");
    if (sel) sel.addEventListener("change", () => { $("#m-new-wrap").hidden = !!sel.value; if (!sel.value) $("#m-new").focus(); });
  });
}
async function addFiles(list, coll) {
  const files = Array.from(list || []).filter(f => /\.(mid|midi)$/i.test(f.name));
  const skipped = (list ? list.length : 0) - files.length;
  if (!files.length) { toast(skipped ? "Only .mid and .midi files can be added." : "No files."); return; }
  if (!coll) { coll = await chooseCollection(files.length); if (!coll) return; }
  let added = 0, already = 0; const errors = [];
  for (let i = 0; i < files.length; i++) {
    toast(`Adding ${i + 1} of ${files.length}: ${files[i].name}`, true);
    try {
      const r = await api(`/api/upload?collection=${encodeURIComponent(coll)}&name=${encodeURIComponent(files[i].name)}`, files[i], true);
      coll = r.collection; if (r.already) already++; else added++;
    } catch (e) { errors.push(e.message); }
  }
  const parts = [];
  if (added) parts.push(`Added ${plural(added, "piece")} to ${coll}.`);
  if (already) parts.push(`${plural(already, "piece")} ${already === 1 ? "was" : "were"} there already.`);
  if (skipped) parts.push(`${plural(skipped, "file")} skipped (not .mid).`);
  if (errors.length) parts.push(errors[0] + (errors.length > 1 ? ` (and ${errors.length - 1} more)` : ""));
  toast(parts.join(" ") || "Nothing added.");
  await refresh();
}
$("#file-input").addEventListener("change", e => { const f = e.target.files; const c = e.target._coll; e.target._coll = null; addFiles(f, c).finally(() => { e.target.value = ""; }); });
function pickFiles(coll) { const i = $("#file-input"); i._coll = coll || null; i.click(); }

let dragDepth = 0;
const hasFiles = e => e.dataTransfer && Array.from(e.dataTransfer.types || []).includes("Files");
window.addEventListener("dragenter", e => { if (!hasFiles(e)) return; dragDepth++; document.body.classList.add("dragging"); $("#dropmask").hidden = false; });
window.addEventListener("dragleave", e => {
  if (!hasFiles(e)) return;
  dragDepth = Math.max(0, dragDepth - 1);
  if (!dragDepth) { document.body.classList.remove("dragging"); $("#dropmask").hidden = true; $$(".coll.over").forEach(c => c.classList.remove("over")); }
});
window.addEventListener("dragover", e => {
  if (!hasFiles(e)) return;
  e.preventDefault(); e.dataTransfer.dropEffect = "copy";
  const c = e.target.closest && e.target.closest(".coll");
  $$(".coll.over").forEach(x => { if (x !== c) x.classList.remove("over"); });
  if (c) c.classList.add("over");
  $("#dropmask span").textContent = c ? "Drop to add to " + c.dataset.coll : "Drop to add to your library";
});
window.addEventListener("drop", e => {
  if (!hasFiles(e)) return;
  e.preventDefault(); dragDepth = 0;
  document.body.classList.remove("dragging"); $("#dropmask").hidden = true;
  const c = e.target.closest && e.target.closest(".coll");
  $$(".coll.over").forEach(x => x.classList.remove("over"));
  if (!$("#modal").hidden && modalLocked) { toast("Wait until the MPC is done."); return; }
  addFiles(e.dataTransfer.files, c ? c.dataset.coll : null);
});

// ---- the buttons ---------------------------------------------------------------------------------------
function findPiece(coll, file) {
  const c = S.library.collections.find(x => x.name === coll);
  return c ? c.pieces.find(x => x.file === file) : null;
}
function editPiece(coll, file) {
  const p = findPiece(coll, file); if (!p) return;
  const levels = ["", "1", "2", "3", "4", "5"];
  if (p.level && !levels.includes(String(p.level))) levels.push(String(p.level));
  modal(`<h2>${esc(p.title)}</h2>
    <form class="stack" id="piece-form" autocomplete="off">
      <label>Title<input name="title" maxlength="80" value="${esc(p.title)}"></label>
      <label>Composer<input name="composer" maxlength="80" value="${esc(p.composer)}"></label>
      <label><span class="lbl">Level <span class="opt">1 easiest, 5 hardest: Sight Reader can sort by it</span></span><select name="level">${levels.map(v =>
        `<option value="${v}"${String(p.level || "") === v ? " selected" : ""}>${v || "None"}</option>`).join("")}</select></label>
      <p class="muted small">${esc(coll + "/" + p.file)} · ${Math.max(1, Math.round(p.size / 1024))} KB${infoLine(p) ? " · " + esc(infoLine(p)) : ""}</p>
      <p class="muted small">The MPC's screen shows plain letters, so accents are left out when you save (Für Elise becomes Fur Elise).</p>
    </form>`,
    [["Delete…", "danger-ghost left", () => { deletePiece(coll, p); return true; }], ["Cancel"],
     ["Save", "primary", async () => {
       const f = $("#piece-form");
       await api("/api/piece", {collection: coll, file: p.file, title: f.title.value, composer: f.composer.value, level: f.level.value});
       await refresh();
     }]]);
  $("#piece-form").addEventListener("submit", e => e.preventDefault());
}
function deletePiece(coll, p) {
  modal(`<h2>Delete ${esc(p.title)}?</h2><p>The file goes to the Trash on this computer. ${S.plan && p.status === "same"
      ? "The next sync removes it from the MPC too." : ""}</p>`,
    [["Cancel"], ["Delete", "danger", async () => { await api("/api/piece/delete", {collection: coll, file: p.file}); toast("Moved to the Trash."); await refresh(); }]]);
}
function newCollection() {
  modal(`<h2>New collection</h2><p class="muted small">A collection is a folder in Sight Reader's LOAD list.</p>
    <label>Name<input id="m-name" maxlength="48" placeholder="for example Gershwin"></label>`,
    [["Cancel"], ["Create", "primary", async () => {
      const r = await api("/api/collection", {name: $("#m-name").value}); toast("Made " + r.name + ". Drop .mid files onto it."); await refresh();
    }]]);
}
function renameCollection(name) {
  modal(`<h2>Rename ${esc(name)}</h2>
    <label>New name<input id="m-name" maxlength="48" value="${esc(name)}"></label>
    <p class="muted small">Sight Reader keeps your progress by where a file is, so after the next sync these pieces start again at no plays on the MPC.</p>`,
    [["Cancel"], ["Rename", "primary", async () => { await api("/api/collection/rename", {name, new: $("#m-name").value}); await refresh(); }]]);
}
function deleteCollection(name) {
  const c = S.library.collections.find(x => x.name === name); const n = c ? c.pieces.length : 0;
  modal(`<h2>Delete the collection ${esc(name)}?</h2><p>The folder and its ${plural(n, "piece")} go to the Trash on this computer. The next sync removes the pieces it put on the MPC.</p>`,
    [["Cancel"], ["Delete", "danger", async () => { await api("/api/collection/delete", {name}); toast("Moved to the Trash."); await refresh(); }]]);
}
function confirmSync() {
  const p = S.plan; if (!p) return;
  const rows = [];
  if (p.add) rows.push(`${plural(p.add, "new piece")} to copy`);
  if (p.replace) rows.push(`${plural(p.replace, "changed piece")} to copy again`);
  if (p.titles) rows.push(`titles to update in ${plural(p.titles, "collection")}`);
  const rm = p.remove;
  const note = S.mpc && S.mpc.auto_rescan ? "Sight Reader shows the changes by itself a few seconds later." : "Afterwards, press RESCAN on Sight Reader's LOAD page.";
  modal(`<h2>Sync with the MPC</h2>
    ${rows.length ? `<ul class="plain">${rows.map(r => `<li>${esc(r)}</li>`).join("")}</ul>` : ""}
    ${rm.length ? `<label class="check"><input type="checkbox" id="m-remove" checked>
        <span>Remove from the MPC the ${plural(rm.length, "piece")} you removed here</span></label>
      <ul class="scroll">${rm.map(x => `<li>${esc(x.title)} <span class="muted">· ${esc(x.coll)}</span></li>`).join("")}</ul>` : ""}
    <p class="muted small">${p.upload ? "About " + esc(p.upload_text) + " to copy. " : ""}${note} Only Sight Reader's library folder on the MPC is touched.</p>`,
    [["Cancel"], ["Sync now", "primary", () => startJob("/api/sync", {remove: rm.length ? $("#m-remove").checked : true})]]);
}
function confirmRestart() {
  modal(`<h2>Restart the MPC app?</h2>
    <div class="alert warn"><b>Anything unsaved in the open project is lost.</b><p>Save your project on the MPC first.</p></div>
    <p>The MPC application closes and starts again. That takes about a minute, and there is no sound meanwhile. The MPC itself stays on.</p>`,
    [["Cancel"], ["Restart the MPC app", "danger", () => startJob("/api/restart", {})]]);
}
function confirmRemoteDelete() {
  const only = (S.mpc && S.mpc.mpc_only) || [];
  const sel = only.filter(x => picked.has(x.rel));
  if (!sel.length) return;
  modal(`<h2>Delete ${plural(sel.length, "piece")} from the MPC?</h2>
    <p>${sel.length === 1 ? "It is" : "They are"} only on the MPC, so this cannot be undone from here. To keep a copy, use Copy to this computer first.</p>
    <ul class="scroll">${sel.map(x => `<li>${esc(x.title)} <span class="muted">· ${esc(x.coll)}</span></li>`).join("")}</ul>`,
    [["Cancel"], ["Delete from the MPC", "danger", () => { const rels = sel.map(x => x.rel); picked.clear(); return startJob("/api/remote-delete", {rels}); }]]);
}
function confirmQuit() {
  modal(`<h2>Quit the loader?</h2><p>The page stops working until you start the loader again. Your library and settings stay as they are.</p>`,
    [["Cancel"], ["Quit", "primary", async () => {
      try { await api("/api/quit", {}); } catch (e) { /* it may go before it answers */ }
      showStopped("The loader has stopped", "You can close this tab. To start it again, double-click mpc-sightreading-midi-loader.command in the loader folder (or run python3 loader/mpc-sightreading-midi-loader.py), then reload this page.");
    }]]);
}

document.addEventListener("click", async e => {
  const b = e.target.closest("[data-act]");
  if (!b || b.disabled) return;
  const act = b.dataset.act;
  const piece = b.closest(".piece"), coll = b.closest(".coll");
  try {
    if (act === "edit" && piece) editPiece(piece.dataset.coll, piece.dataset.file);
    else if (act === "add") pickFiles(null);
    else if (act === "add-to" && coll) pickFiles(coll.dataset.coll);
    else if (act === "new-coll") newCollection();
    else if (act === "rename-coll" && coll) renameCollection(coll.dataset.coll);
    else if (act === "delete-coll" && coll) deleteCollection(coll.dataset.coll);
    else if (act === "reveal") await api("/api/reveal", {});
    else if (act === "choose") {
      toast("Choose the folder in the window that opened (it may be behind this one).", true);
      const r = await api("/api/choose-folder", {});
      $("#toast").hidden = true;
      if (r.path) { formDirty = false; toast("The library is " + r.path); await refresh(); }
    }
    else if (act === "look") await look(false);
    else if (act === "sync") confirmSync();
    else if (act === "restart") confirmRestart();
    else if (act === "pull") { const rels = Array.from(picked); picked.clear(); await startJob("/api/pull", {rels}); }
    else if (act === "remote-delete") confirmRemoteDelete();
    else if (act === "pick-all") {
      const only = (S.mpc && S.mpc.mpc_only) || [];
      if (picked.size === only.length) picked.clear(); else only.forEach(x => picked.add(x.rel));
      renderMpc();
    }
    else if (act === "close-settings") { $("#settings").hidden = true; formDirty = false; render(); }
    else if (act === "quit") confirmQuit();
  } catch (err) { toast(err.message); }
});
document.addEventListener("change", e => {
  const c = e.target.closest("input[data-rel]");
  if (!c) return;
  if (c.checked) picked.add(c.dataset.rel); else picked.delete(c.dataset.rel);
  renderMpc();
});
$("#btn-settings").addEventListener("click", () => {
  const s = $("#settings"); s.hidden = !s.hidden;
  if (!s.hidden) { formDirty = false; renderSettings(); $("#f-address").focus(); }
});
$("#settings-form").addEventListener("input", () => { formDirty = true; });
$("#settings-form").addEventListener("submit", async e => {
  e.preventDefault();
  const f = e.target;
  const body = {address: f.address.value.trim(), user: f.user.value.trim() || "root", ssh_port: f.ssh_port.value.trim() || "22",
                drive: f.drive.value.trim(), library: f.library.value.trim()};
  if (f.password.value) body.password = f.password.value;
  if (!body.address) { toast("Enter the MPC's IP address."); f.address.focus(); return; }
  try {
    await api("/api/settings", body);
    formDirty = false;
    $("#settings").hidden = true;
    await refresh();
    await look(false);
  } catch (err) { toast(err.message); }
});
$("#btn-reload").addEventListener("click", () => location.reload());

refresh().then(schedule);
</script>
</body>
</html>
"""

if __name__ == "__main__":
    sys.exit(main())
