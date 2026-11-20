#!/usr/bin/env python3
"""Fills Sight Reader's library: download music, add your own files, copy it all to the MPC.

    python3 tools/library.py fetch                      download library/catalog.tsv into library/cache/
    python3 tools/library.py mutopia "Clementi" [--max 30] [--collection NAME]
                                                        search the Mutopia Project's piano music and fetch
    python3 tools/library.py add FILES... --collection "My Songs" [--title T] [--level N]
                                                        copy your own .mid files into the cache
    python3 tools/library.py list                       what is in the cache
    python3 tools/library.py push <mpc-ip> [--drive /nvme] [--dry-run]
                                                        copy the cache to <drive>/SightReader/Library over SSH
    python3 tools/library.py sync <mpc-ip>              fetch, then push
    python3 tools/library.py device-script > get.sh     a shell script that downloads the catalogue on the
                                                        MPC itself:  ssh root@<mpc> sh -s < get.sh

Needs Python 3.8+ and the ssh command. The MPC must be in developer mode (Preferences ->
Developer: SSH server on; you log in as root with no password). Nothing is installed or changed
on the MPC except the SightReader/Library folder on the drive. Sight Reader 0.6.0 and later see
the change and rescan by themselves (older versions: press RESCAN on the plugin's LOAD page).

For adding your own files there is also a page you can use instead of these commands:
loader/mpc-sightreading-midi-loader.py (see the README).

Each collection folder gets an index.tsv (file, title, composer, level, source) that the
plugin shows in its list. Music from Mutopia is public domain or Creative Commons; the licence
of every piece is on its Mutopia page, linked from index.tsv.
"""
import argparse, html, io, json, os, re, shlex, shutil, signal, subprocess, sys, tarfile, time, urllib.parse, urllib.request, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CATALOG = os.path.join(ROOT, "library", "catalog.tsv")
CACHE = os.path.join(ROOT, "library", "cache")
MUTOPIA = "https://www.mutopiaproject.org/ftp/"
UA = {"User-Agent": "sight-reader-library/0.1 (+https://www.mutopiaproject.org)"}

def safe(name):
    name = re.sub(r"[^A-Za-z0-9 ._()&,'-]+", "_", name).strip()
    return name or "Untitled"

def http_get(url, tries=3):
    for k in range(tries):
        try:
            req = urllib.request.Request(url, headers=UA)
            with urllib.request.urlopen(req, timeout=60) as r:
                return r.read()
        except Exception as e:                       # noqa: BLE001 - network errors of every kind
            if k == tries - 1:
                raise
            time.sleep(2 * (k + 1))

def read_index(folder):
    rows = {}
    p = os.path.join(folder, "index.tsv")
    if os.path.exists(p):
        for line in open(p, encoding="utf-8"):
            if line.startswith("#") or not line.strip():
                continue
            f = line.rstrip("\n").split("\t")
            rows[f[0]] = f + [""] * (5 - len(f))
    return rows

def write_index(folder, rows):
    with open(os.path.join(folder, "index.tsv"), "w", encoding="utf-8") as f:
        f.write("# file\ttitle\tcomposer\tlevel\tsource\n")
        for name in sorted(rows):
            r = rows[name]
            f.write("\t".join(str(x) for x in r[:5]) + "\n")

def is_midi(data):
    return data[:4] == b"MThd" or (data[:4] == b"RIFF" and b"MThd" in data[:64])

def store(collection, url, title, composer, level, source):
    """Downloads one .mid (or a Mutopia -mids.zip) into the cache. Returns files written."""
    folder = os.path.join(CACHE, safe(collection))
    os.makedirs(folder, exist_ok=True)
    rows = read_index(folder)
    base = os.path.basename(urllib.parse.urlparse(url).path)
    written = []
    if base.endswith("-mids.zip"):
        stem = base[:-len("-mids.zip")]
        if any(k.startswith(stem + "-") for k in rows):
            return []
        data = http_get(url)
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            mids = sorted(n for n in z.namelist() if n.lower().endswith(".mid"))
            for i, n in enumerate(mids):
                d = z.read(n)
                if not is_midi(d):
                    continue
                fname = "%s-%d.mid" % (stem, i + 1)
                with open(os.path.join(folder, fname), "wb") as f:
                    f.write(d)
                rows[fname] = [fname, "%s (%d)" % (title, i + 1), composer, str(level), source]
                written.append(fname)
    else:
        if base in rows and os.path.exists(os.path.join(folder, base)):
            return []
        data = http_get(url)
        if not is_midi(data):
            raise ValueError("not a MIDI file")
        with open(os.path.join(folder, base), "wb") as f:
            f.write(data)
        rows[base] = [base, title, composer, str(level), source]
        written.append(base)
    write_index(folder, rows)
    return written

def cmd_fetch(args):
    n = 0
    for line in open(CATALOG, encoding="utf-8"):
        if line.startswith("#") or not line.strip():
            continue
        coll, path, title, composer, level = (line.rstrip("\n").split("\t") + [""] * 5)[:5]
        url = MUTOPIA + path
        try:
            got = store(coll, url, title, composer, level or "3", "Mutopia Project: " + url)
            if got:
                print("  + %-28s %s" % (coll[:28], title))
                n += len(got)
                time.sleep(0.3)                       # be gentle with a volunteer-run site
        except Exception as e:                        # noqa: BLE001
            print("  ! %s: %s (%s)" % (title, e, url))
    print("fetched %d new file(s) into %s" % (n, CACHE))

def cmd_mutopia(args):
    """Searches Mutopia's catalogue (solo piano) and fetches the matches."""
    q = urllib.parse.quote(args.query)
    found = []
    start = 0
    while len(found) < args.max:
        url = ("https://www.mutopiaproject.org/cgibin/make-table.cgi?startat=%d&searchingfor=%s&Composer=&Instrument=Piano"
               "&Style=&collection=&id=&solo=&recent=&timelength=&timeunit=&lilyversion=&preview=" % (start, q))
        page = http_get(url).decode("utf-8", "replace")
        tables = re.split(r'<table class="table-bordered result-table"', page)[1:]
        if not tables:
            break
        for t in tables:
            cells = [html.unescape(re.sub(r"\s+", " ", re.sub(r"<[^>]+>", " ", c))).strip() for c in re.findall(r"<td[^>]*>(.*?)</td>", t, re.S)]
            mids = re.findall(r'href="([^"]+\.mid)"', t) or re.findall(r'href="([^"]+-mids\.zip)"', t)
            if not mids or len(cells) < 5:
                continue
            inst = cells[4]
            if "piano" not in inst.lower() or "," in inst:
                continue                               # solo piano only
            composer = re.sub(r"^by\s+", "", cells[1]).strip()
            found.append((mids[0], cells[0], composer))
            if len(found) >= args.max:
                break
        start += 10
        time.sleep(0.3)
    coll = args.collection or args.query.title()
    n = 0
    for url, title, composer in found:
        try:
            got = store(coll, url, title, composer, args.level, "Mutopia Project: " + url)
            if got:
                print("  + %s - %s" % (title, composer)); n += len(got); time.sleep(0.3)
        except Exception as e:                          # noqa: BLE001
            print("  ! %s: %s" % (title, e))
    print("%d match(es), %d new file(s) in collection '%s'" % (len(found), n, coll))

def cmd_add(args):
    folder = os.path.join(CACHE, safe(args.collection))
    os.makedirs(folder, exist_ok=True)
    rows = read_index(folder)
    for src in args.files:
        data = open(src, "rb").read()
        if not is_midi(data):
            print("  ! %s is not a MIDI file" % src); continue
        fname = safe(os.path.basename(src))
        if not fname.lower().endswith((".mid", ".midi")):
            fname += ".mid"
        shutil.copyfile(src, os.path.join(folder, fname))
        title = args.title or os.path.splitext(os.path.basename(src))[0].replace("_", " ")
        rows[fname] = [fname, title, args.composer or "", str(args.level), "added by you"]
        print("  + %s" % fname)
    write_index(folder, rows)

def collections():
    if not os.path.isdir(CACHE):
        return []
    return sorted(d for d in os.listdir(CACHE) if os.path.isdir(os.path.join(CACHE, d)))

def cmd_list(args):
    for c in collections():
        rows = read_index(os.path.join(CACHE, c))
        print("%s (%d)" % (c, len(rows)))
        for r in sorted(rows.values(), key=lambda r: (r[3], r[1])):
            print("   L%s  %s%s" % (r[3] or "?", r[1], (" - " + r[2]) if r[2] else ""))

def ssh(host, command, data=None, dry=False):
    argv = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8", "-o", "StrictHostKeyChecking=accept-new", "root@" + host, command]
    if dry:
        print("would run:", " ".join(shlex.quote(a) for a in argv), "(+ %d bytes)" % len(data or b""))
        return b""
    r = subprocess.run(argv, input=data, capture_output=True)
    if r.returncode != 0:
        sys.exit("ssh failed (%d): %s" % (r.returncode, r.stderr.decode(errors="replace").strip()))
    return r.stdout

def cmd_push(args):
    colls = collections()
    if not colls:
        sys.exit("the cache is empty: run 'fetch' or 'add' first")
    def as_root(ti):                                  # owned by the MPC's root, readable by all
        ti.uid = ti.gid = 0
        ti.uname = ti.gname = "root"
        ti.mode = 0o755 if ti.isdir() else 0o644
        return ti
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as tar:
        for c in colls:
            tar.add(os.path.join(CACHE, c), arcname=c, filter=as_root)
    lib = args.drive.rstrip("/") + "/SightReader/Library"
    probe = ssh(args.host, "test -d %s && echo ok" % shlex.quote(args.drive), dry=args.dry_run)
    if not args.dry_run and b"ok" not in probe:
        sys.exit("%s is not a folder on the MPC (use --drive; the internal drive is /nvme on a Live III)" % args.drive)
    # .rescan last: Sight Reader 0.6.0 and later see it change and rescan by themselves
    mark = "printf '%%s\\n' %s > .rescan.tmp && mv -f .rescan.tmp .rescan" % shlex.quote("%d library.py" % time.time_ns())
    ssh(args.host, "mkdir -p %s && cd %s && tar -xf - && %s && ls" % (shlex.quote(lib), shlex.quote(lib), mark), buf.getvalue(), args.dry_run)
    total = sum(len(read_index(os.path.join(CACHE, c))) for c in colls)
    print("copied %d collection(s), %d file(s) to %s:%s" % (len(colls), total, args.host, lib))
    print("Sight Reader 0.6.0 or later shows them within a few seconds (older versions: press RESCAN on the LOAD page).")

def cmd_sync(args):
    cmd_fetch(args)
    cmd_push(args)

def cmd_device_script(args):
    """A POSIX sh script for the MPC (BusyBox): downloads the catalogue with curl or wget."""
    print("#!/bin/sh")
    print("# Generated by tools/library.py device-script. Run on the MPC:  ssh root@<mpc> sh -s < get.sh")
    print("set -u")
    print('LIB="${SIGHTREADER_LIB:-%s/SightReader/Library}"' % args.drive.rstrip("/"))
    print('if command -v curl >/dev/null 2>&1; then GET="curl -fsSL --retry 2 -o"; '
          'elif command -v wget >/dev/null 2>&1; then GET="wget -q -O"; else echo "no curl or wget on this MPC: use library.py push"; exit 1; fi')
    print('ok=0; bad=0')
    print('get() { mkdir -p "$LIB/$1"; f="$LIB/$1/$(basename "$2")"; [ -s "$f" ] && return 0; '
          'if $GET "$f.part" "$2" && mv "$f.part" "$f"; then ok=$((ok+1)); printf "%s\\t%s\\t%s\\t%s\\t%s\\n" "$(basename "$2")" "$3" "$4" "$5" "Mutopia Project: $2" >> "$LIB/$1/index.tsv"; '
          'else rm -f "$f.part"; bad=$((bad+1)); echo "failed: $2"; fi; }')
    for line in open(CATALOG, encoding="utf-8"):
        if line.startswith("#") or not line.strip():
            continue
        coll, path, title, composer, level = (line.rstrip("\n").split("\t") + [""] * 5)[:5]
        if path.endswith(".zip"):
            continue                                   # zips need unzip; push them from a computer instead
        print("get %s %s %s %s %s" % tuple(shlex.quote(x) for x in (coll, MUTOPIA + path, title, composer, level or "3")))
    print('echo "downloaded $ok, failed $bad. Press RESCAN on the LOAD page."')

def main():
    if hasattr(signal, "SIGPIPE"):
        signal.signal(signal.SIGPIPE, signal.SIG_DFL)   # quiet when piped into head
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("fetch")
    m = sub.add_parser("mutopia"); m.add_argument("query"); m.add_argument("--max", type=int, default=30)
    m.add_argument("--collection"); m.add_argument("--level", default="3")
    a = sub.add_parser("add"); a.add_argument("files", nargs="+"); a.add_argument("--collection", default="My Music")
    a.add_argument("--title"); a.add_argument("--composer"); a.add_argument("--level", type=int, default=3)
    sub.add_parser("list")
    for name in ("push", "sync"):
        p = sub.add_parser(name); p.add_argument("host"); p.add_argument("--drive", default="/nvme"); p.add_argument("--dry-run", action="store_true")
    d = sub.add_parser("device-script"); d.add_argument("--drive", default="/nvme")
    args = ap.parse_args()
    {"fetch": cmd_fetch, "mutopia": cmd_mutopia, "add": cmd_add, "list": cmd_list, "push": cmd_push,
     "sync": cmd_sync, "device-script": cmd_device_script}[args.cmd](args)

if __name__ == "__main__":
    main()
