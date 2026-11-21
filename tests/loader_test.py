#!/usr/bin/env python3
"""Tests for loader/mpc-sightreading-midi-loader.py against a pretend MPC.

    python3 tests/loader_test.py            (tools/run_tests.sh runs it)

The MPC is a folder in a temporary directory, reached through tests/fake_ssh.py, which runs the
loader's remote commands here. Every test runs twice: under /bin/sh, and (when busybox is
installed) under busybox with nothing but busybox applets on the PATH, the way a small device
shell would be. A third pass takes md5sum away, as on an MPC without it.
"""
import http.client
import importlib.util
import io
import json
import os
import shutil
import stat
import sys
import tempfile
import threading
import time
import unittest
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FAKE_SSH = os.path.join(ROOT, "tests", "fake_ssh.py")
spec = importlib.util.spec_from_file_location("loader", os.path.join(ROOT, "loader", "mpc-sightreading-midi-loader.py"))
loader = importlib.util.module_from_spec(spec)
spec.loader.exec_module(loader)

BUSYBOX = shutil.which("busybox")
APPLETS = ["sh", "tar", "md5sum", "wc", "sed", "cat", "mkdir", "rm", "rmdir", "mv", "printf", "tr", "head", "ls", "echo", "test", "["]


def rb(path):
    with open(path, "rb") as f:
        return f.read()


def rt(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def smf(notes, tempo=500000, name="Test", step=480):
    """A one-track MIDI file: each note a quarter at `tempo` (0.5 s at 120 BPM)."""
    ev = bytearray(b"\x00\xff\x03" + bytes([len(name)]) + name.encode("latin-1"))
    ev += b"\x00\xff\x51\x03" + tempo.to_bytes(3, "big")
    vlq = bytes([0x80 | (step >> 7), step & 0x7F])
    for n in notes:
        ev += b"\x00\x90" + bytes([n, 80]) + vlq + b"\x80" + bytes([n, 0])
    ev += b"\x00\xff\x2f\x00"
    return (b"MThd" + (6).to_bytes(4, "big") + (0).to_bytes(2, "big") + (1).to_bytes(2, "big") + step.to_bytes(2, "big")
            + b"MTrk" + len(ev).to_bytes(4, "big") + bytes(ev))


class Base(unittest.TestCase):
    mode = "sh"                              # sh | busybox | nomd5

    def setUp(self):
        if self.mode != "sh" and not BUSYBOX:
            self.skipTest("busybox is not installed")
        self.tmp = tempfile.mkdtemp(prefix="srml-test-")
        self.drive = os.path.join(self.tmp, "mpc", "nvme")
        self.plugin = os.path.join(self.drive, "VST2 Plugins", "Practice Tools - VST - Sight Reader")
        os.makedirs(self.plugin)
        with open(os.path.join(self.plugin, "plugin.json"), "w") as f:
            f.write('{\n  "name": "Sight Reader",\n  "version": "0.6.0",\n  "format": "vst2"\n}\n')
        os.makedirs(os.path.join(self.tmp, "mpc", "media", "Some SD card"))     # a drive without Sight Reader
        self.lib_remote = os.path.join(self.drive, "SightReader", "Library")
        self.cfg = os.path.join(self.tmp, "settings")
        self.libdir = os.path.join(self.tmp, "computer", "Sight Reader Library")
        self.log = os.path.join(self.tmp, "ssh.log")
        bindir = os.path.join(self.tmp, "bin")
        os.makedirs(bindir)
        with open(os.path.join(bindir, "systemctl"), "w") as f:
            f.write('#!/bin/sh\necho "$*" >> "%s/systemctl.log"\n'
                    'case "$1" in cat|restart) exit 0;; is-active) echo active; exit 0;; esac\nexit 1\n' % self.tmp)
        os.chmod(os.path.join(bindir, "systemctl"), 0o755)
        env = {"MPC_SSH_BIN": FAKE_SSH, "FAKE_MPC_LOG": self.log, "FAKE_MPC_HOME": self.tmp}
        if self.mode == "sh":
            env["FAKE_MPC_PATH"] = bindir + os.pathsep + os.environ.get("PATH", "/usr/bin:/bin")
        else:
            for a in APPLETS:
                if a == "md5sum" and self.mode == "nomd5":
                    continue
                os.symlink(BUSYBOX, os.path.join(bindir, a))
            env["FAKE_MPC_PATH"] = bindir
            env["FAKE_MPC_SHELL"] = BUSYBOX + " sh"
        self._old_env = {k: os.environ.get(k) for k in env}
        os.environ.update(env)
        self._old_drives = loader.DRIVES
        loader.DRIVES = "%s/mpc/nvme %s/mpc/media/*" % (self.tmp, self.tmp)
        self.app = loader.App(self.cfg)
        self.app.save_settings({"address": "mpc.test", "library": self.libdir})

    def tearDown(self):
        if hasattr(self, "app"):
            self.app.close()
        loader.DRIVES = getattr(self, "_old_drives", loader.DRIVES)
        for k, v in getattr(self, "_old_env", {}).items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
        shutil.rmtree(getattr(self, "tmp", ""), ignore_errors=True)

    # -- helpers ------------------------------------------------------------------------------------
    def run_job(self, fn, *args):
        job = loader.Job("test", "test")
        fn(job, *args)
        return job

    def sync(self, remove=True):
        job = self.run_job(self.app.job_sync, remove)
        self.assertTrue(job.ok, (job.message, job.lines))
        return job

    def remote(self, rel):
        return os.path.join(self.lib_remote, *rel.split("/"))

    def remote_index(self, coll):
        with open(os.path.join(self.lib_remote, coll, "index.tsv"), encoding="utf-8") as f:
            return loader.read_index_text(f.read())

    def plan(self):
        self.run_job(self.app.job_look)
        return self.app.state()["plan"]


class LoaderTests(Base):
    def test_no_address_until_entered_then_remembered(self):
        fresh = loader.App(os.path.join(self.tmp, "fresh"))
        self.assertEqual(fresh.settings["address"], "")
        self.assertEqual(fresh.state()["settings"]["address"], "")
        fresh.save_settings({"address": "10.1.2.3", "password": "secret"})
        again = loader.App(os.path.join(self.tmp, "fresh"))
        self.assertEqual(again.settings["address"], "10.1.2.3")
        self.assertIsNone(again.password)
        with open(os.path.join(self.tmp, "fresh", "settings.json")) as f:
            self.assertNotIn("secret", f.read())
        self.assertEqual(stat.S_IMODE(os.stat(os.path.join(self.tmp, "fresh", "settings.json")).st_mode), 0o600)
        with self.assertRaises(loader.LibraryError):
            fresh.save_settings({"address": "10.0.0.1; rm -rf /"})

    def test_look_finds_sight_reader(self):
        self.run_job(self.app.job_look)
        m = self.app.mpc
        self.assertEqual(m["plugin"]["version"], "0.6.0")
        self.assertEqual(m["drive"], self.drive)
        self.assertEqual(m["lib_path"], self.lib_remote)
        self.assertTrue(m["auto_rescan"])
        self.assertEqual(m["md5"], self.mode != "nomd5")
        self.assertEqual(len(m["drives"]), 1)          # the SD card has neither VST2 Plugins nor SightReader

    def test_add_sync_update_remove(self):
        lib = self.app.library
        r = lib.add("Gershwin", "Rhapsody in Blue (easy).mid", smf([60, 62, 64]), title="Rhapsody in Blue (easy)",
                    composer="George Gershwin")
        self.assertEqual(r, {"collection": "Gershwin", "file": "Rhapsody in Blue (easy).mid", "already": False})
        lib.add("Bach's Goldberg", "Aria – BWV 988.mid", smf([67, 69, 71, 72]), composer="J. S. Bäch")
        self.assertTrue(lib.add("Gershwin", "again.mid", smf([60, 62, 64]))["file"] == "again.mid")
        self.assertTrue(lib.add("Gershwin", "Rhapsody in Blue (easy).mid", smf([60, 62, 64]))["already"])
        p = self.plan()
        self.assertEqual((p["add"], p["replace"], p["remove"]), (3, 0, []))
        job = self.sync()
        self.assertIn("3 added", job.message)
        self.assertEqual(rb(self.remote("Gershwin/Rhapsody in Blue (easy).mid")), smf([60, 62, 64]))
        self.assertTrue(os.path.isfile(self.remote("Bach's Goldberg/Aria - BWV 988.mid")))
        rows = self.remote_index("Bach's Goldberg")
        self.assertEqual(rows["Aria - BWV 988.mid"][1:3], ["Aria - BWV 988", "J. S. Bach"])
        self.assertEqual(self.remote_index("Gershwin")["Rhapsody in Blue (easy).mid"][2], "George Gershwin")
        marker = rt(os.path.join(self.lib_remote, ".rescan"))
        self.assertIn(loader.APP, marker)
        self.assertTrue(self.plan()["empty"])
        # a changed file is copied again, a removed one is removed there, an empty collection goes
        time.sleep(0.01)
        with open(os.path.join(self.libdir, "Gershwin", "again.mid"), "wb") as f:
            f.write(smf([72, 74]))
        lib.delete("Bach's Goldberg", "Aria - BWV 988.mid")
        p = self.plan()
        self.assertEqual(p["replace"], 1)
        self.assertEqual([x["rel"] for x in p["remove"]], ["Bach's Goldberg/Aria - BWV 988.mid"])
        self.assertEqual(p["remove"][0]["title"], "Aria - BWV 988")
        self.sync()
        self.assertEqual(rb(self.remote("Gershwin/again.mid")), smf([72, 74]))
        self.assertFalse(os.path.exists(os.path.join(self.lib_remote, "Bach's Goldberg")))
        self.assertNotEqual(rt(os.path.join(self.lib_remote, ".rescan")), marker)
        self.assertTrue(self.plan()["empty"])
        # a title edited here reaches the MPC's index.tsv
        lib.update("Gershwin", "again.mid", "Again!", "", "2")
        p = self.plan()
        self.assertEqual((p["add"], p["replace"], p["titles"]), (0, 0, 1))
        self.assertFalse(p["empty"])
        self.sync()
        self.assertEqual(self.remote_index("Gershwin")["again.mid"][1:4], ["Again!", "", "2"])

    def test_removals_can_wait(self):
        self.app.library.add("A", "one.mid", smf([60]))
        self.app.library.add("A", "two.mid", smf([62]))
        self.sync()
        self.app.library.delete("A", "two.mid")
        self.sync(remove=False)
        self.assertTrue(os.path.isfile(self.remote("A/two.mid")))
        self.assertIn("two.mid", self.remote_index("A"))        # its title stays with it
        self.assertEqual([x["rel"] for x in self.plan()["remove"]], ["A/two.mid"])   # still offered
        self.sync()
        self.assertFalse(os.path.exists(self.remote("A/two.mid")))
        self.assertTrue(os.path.isfile(self.remote("A/one.mid")))

    def test_pieces_only_on_the_mpc_are_left_alone_then_copied_back(self):
        os.makedirs(os.path.join(self.lib_remote, "Classics"))
        with open(self.remote("Classics/minuet.mid"), "wb") as f:
            f.write(smf([67, 60, 62]))
        with open(self.remote("Classics/index.tsv"), "w") as f:
            f.write("# file\ttitle\tcomposer\tlevel\tsource\nminuet.mid\tMinuet in G\tPetzold\t1\tMutopia\n")
        self.app.library.add("Classics", "mine.mid", smf([64]))
        p = self.plan()
        self.assertEqual(p["add"], 1)
        only = self.app.state()["mpc"]["mpc_only"]
        self.assertEqual([(x["rel"], x["title"]) for x in only], [("Classics/minuet.mid", "Minuet in G")])
        self.sync()
        self.assertTrue(os.path.isfile(self.remote("Classics/minuet.mid")))          # untouched
        rows = self.remote_index("Classics")
        self.assertEqual(sorted(rows), ["mine.mid", "minuet.mid"])                       # both titled
        self.assertEqual(rows["minuet.mid"][1], "Minuet in G")
        job = self.run_job(self.app.job_pull, ["Classics/minuet.mid", "Classics/nothere.mid"])
        self.assertTrue(job.ok, job.message)
        self.assertEqual(rb(os.path.join(self.libdir, "Classics", "minuet.mid")), smf([67, 60, 62]))
        local_rows = loader.read_index_text(rt(os.path.join(self.libdir, "Classics", "index.tsv")))
        self.assertEqual(local_rows["minuet.mid"][1:4], ["Minuet in G", "Petzold", "1"])
        p = self.plan()
        self.assertTrue(p["empty"], p)
        self.assertEqual(self.app.state()["mpc"]["mpc_only"], [])
        # now it is ours: removing it here removes it there
        self.app.library.delete("Classics", "minuet.mid")
        self.assertEqual([x["rel"] for x in self.plan()["remove"]], ["Classics/minuet.mid"])

    def test_delete_on_the_mpc(self):
        os.makedirs(os.path.join(self.lib_remote, "Old"))
        for n in ("a.mid", "b.mid"):
            with open(self.remote("Old/" + n), "wb") as f:
                f.write(smf([60]))
        with open(self.remote("Old/index.tsv"), "w") as f:
            f.write("a.mid\tPiece A\t\t\t\nb.mid\tPiece B\t\t\t\n")
        self.plan()
        job = self.run_job(self.app.job_remote_delete, ["Old/a.mid"])
        self.assertTrue(job.ok, job.message)
        self.assertFalse(os.path.exists(self.remote("Old/a.mid")))
        self.assertEqual(list(self.remote_index("Old")), ["b.mid"])
        self.run_job(self.app.job_remote_delete, ["Old/b.mid"])
        self.assertFalse(os.path.exists(os.path.join(self.lib_remote, "Old")))

    def test_pieces_copied_before_are_recognised(self):
        """tools/library.py push put the same files there earlier: nothing to copy, and they become ours."""
        self.app.library.add("Mutopia", "x.mid", smf([60, 64]))
        os.makedirs(os.path.join(self.lib_remote, "Mutopia"))
        shutil.copy(os.path.join(self.libdir, "Mutopia", "x.mid"), self.remote("Mutopia/x.mid"))
        p = self.plan()
        if self.mode == "nomd5":
            self.assertEqual(p["replace"], 1)            # cannot tell without md5sum: copied again, harmless
        else:
            self.assertEqual((p["add"], p["replace"], p["same"]), (0, 0, 1))
        self.sync()
        self.app.library.delete("Mutopia", "x.mid")
        self.assertEqual([x["rel"] for x in self.plan()["remove"]], ["Mutopia/x.mid"])

    def test_restart(self):
        job = self.run_job(self.app.job_restart)
        self.assertTrue(job.ok, job.message)
        log = rt(os.path.join(self.tmp, "systemctl.log")).split("\n")
        self.assertIn("restart acvs", log)
        self.assertIn("is-active acvs", log)

    def test_problems_are_explained(self):
        for host, words in (("unreachable.invalid", "Nothing answers"), ("refused.invalid", "SSH server is off"),
                            ("denied.invalid", "turned down the login")):
            self.app.save_settings({"address": host})
            self.app.start("look", "Looking", self.app.job_look)
            job = self.app.wait(30)
            self.assertFalse(job.ok)
            self.assertIn(words, job.message)
            self.assertIn(words, self.app.state()["mpc"]["error"]["title"])
        self.app.save_settings({"address": "mpc.test"})
        shutil.rmtree(self.plugin)
        self.run_job(self.app.job_look)
        self.assertIn("not installed", self.app.mpc["problem"])
        job = loader.Job("sync", "sync")
        with self.assertRaises(loader.LibraryError):
            self.app.job_sync(job)

    def test_older_plugin_needs_rescan(self):
        with open(os.path.join(self.plugin, "plugin.json"), "w") as f:
            f.write('{"version": "0.5.6"}')
        self.app.library.add("A", "one.mid", smf([60]))
        self.plan()
        self.assertFalse(self.app.mpc["auto_rescan"])
        job = self.sync()
        self.assertIn("RESCAN", " ".join(job.lines))


class WebSite:
    """A small web server on 127.0.0.1 with the kinds of links people paste."""
    def __init__(self):
        import http.server
        z = io.BytesIO()
        with zipfile.ZipFile(z, "w") as zf:
            zf.writestr("pack/a.mid", smf([60, 62]))
            zf.writestr("pack/b.midi", smf([64, 65]))
            zf.writestr("pack/readme.txt", "not music")
            zf.writestr("pack/deeper/c.mid", smf([67]))
        page = ('<html><head><title>Bach at Mutopia</title></head><body>'
                '<a href="piece.mid">Piece</a> <a href="/pack.zip">All of them</a> <a href="piece.mid#top">again</a>'
                '<a href="score.pdf">PDF</a> <a href="mailto:x@example.org">mail</a>'
                '<a href="http://127.0.0.1:%d/other/song.MID">elsewhere</a></body></html>')
        site = self
        class H(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass
            def do_GET(self):
                path = self.path.split("?")[0]
                body, ctype, extra, code = None, "application/octet-stream", {}, 200
                if path in ("/piece.mid", "/other/song.MID"):
                    body, ctype = smf([60, 64, 67]), "audio/midi"
                elif path == "/pack.zip":
                    body, ctype = z.getvalue(), "application/zip"
                elif path == "/page.html":
                    body, ctype = (page % site.port).encode(), "text/html; charset=utf-8"
                elif path == "/empty.html":
                    body, ctype = b"<html><body>no links</body></html>", "text/html"
                elif path == "/dl":
                    body, extra = smf([72]), {"Content-Disposition": 'attachment; filename="Goldberg Aria.mid"'}
                elif path == "/notmidi.bin":
                    body = b"\x00\x01 just bytes"
                elif path == "/big.mid":
                    body = b"MThd" + b"\x00" * (9 << 20)
                elif path == "/forbidden.mid":
                    code = 403
                else:
                    code = 404
                self.send_response(code)
                for k, v in extra.items():
                    self.send_header(k, v)
                if body is not None:
                    self.send_header("Content-Type", ctype)
                    self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                if body is not None:
                    self.wfile.write(body)
        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
        self.port = self.httpd.server_address[1]
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def url(self, path):
        return "http://127.0.0.1:%d%s" % (self.port, path)

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()


class LinkTests(Base):
    """Add from a link (0.6.5): downloaded by the computer, never by the MPC."""
    def setUp(self):
        Base.setUp(self)
        self.web = WebSite()

    def tearDown(self):
        self.web.close()
        Base.tearDown(self)

    def test_a_midi_link(self):
        r = self.app.add_from_links([self.web.url("/piece.mid")], "From the Web")
        self.assertEqual((r["added"], r["collection"]), (["piece.mid"], "From the Web"))
        rows = loader.read_index_text(rt(os.path.join(self.libdir, "From the Web", "index.tsv")))
        self.assertEqual(rows["piece.mid"][4], self.web.url("/piece.mid"))      # where it came from
        again = self.app.add_from_links([self.web.url("/piece.mid")], "From the Web")
        self.assertEqual((again["added"], again["already"]), ([], 1))
        named = self.app.add_from_links([self.web.url("/dl?id=7")], "From the Web")
        self.assertEqual(named["added"], ["Goldberg Aria.mid"])                   # the name the site gave

    def test_a_zip_of_midi_files(self):
        r = self.app.add_from_links([self.web.url("/pack.zip")], "Pack")
        self.assertEqual(sorted(r["added"]), ["a.mid", "b.midi", "c.mid"])
        self.assertEqual(sorted(os.listdir(os.path.join(self.libdir, "Pack"))), ["a.mid", "b.midi", "c.mid", "index.tsv"])

    def test_a_page_of_links(self):
        r = self.app.add_from_links([self.web.url("/page.html")], "Bach")
        self.assertEqual(r["title"], "Bach at Mutopia")
        self.assertEqual([l["name"] for l in r["links"]], ["piece.mid", "pack.zip", "song.MID"])
        self.assertFalse(os.path.exists(os.path.join(self.libdir, "Bach")))       # nothing added yet
        picked = self.app.add_from_links([r["links"][0]["url"], r["links"][2]["url"]], "Bach")
        self.assertEqual(sorted(picked["added"]), ["piece.mid", "song.mid"])
        with self.assertRaises(loader.LibraryError) as e:
            self.app.add_from_links([self.web.url("/empty.html")], "Bach")
        self.assertIn("no links to MIDI files", str(e.exception))

    def test_what_goes_wrong_is_said(self):
        cases = [("/forbidden.mid", "turned the loader away"), ("/missing.mid", "nothing at that link"),
                 ("/notmidi.bin", "not a MIDI file"), ("/big.mid", "over 8 MB")]
        for path, words in cases:
            with self.assertRaises(loader.LibraryError) as e:
                self.app.add_from_links([self.web.url(path)], "X")
            self.assertIn(words, str(e.exception), path)
        with self.assertRaises(loader.LibraryError) as e:
            self.app.add_from_links(["file:///etc/passwd"], "X")
        self.assertIn("http:// or https://", str(e.exception))
        # several links: what worked is added, what did not is listed
        r = self.app.add_from_links([self.web.url("/piece.mid"), self.web.url("/missing.mid")], "Mixed")
        self.assertEqual(r["added"], ["piece.mid"])
        self.assertEqual(len(r["problems"]), 1)
        self.assertIn("missing.mid", r["problems"][0])

    def test_share_links(self):
        self.assertEqual(loader.direct_link("https://github.com/someone/songs/blob/main/bach/aria.mid"),
                         "https://raw.githubusercontent.com/someone/songs/main/bach/aria.mid")
        self.assertEqual(loader.direct_link("https://drive.google.com/file/d/1AbC_d-9/view?usp=sharing"),
                         "https://drive.google.com/uc?export=download&id=1AbC_d-9")
        self.assertEqual(loader.direct_link("https://www.dropbox.com/s/xyz/aria.mid?dl=0"),
                         "https://www.dropbox.com/s/xyz/aria.mid?dl=1")
        self.assertEqual(loader.direct_link("https://example.org/a.mid"), "https://example.org/a.mid")

    def test_through_the_page_api(self):
        httpd = loader.serve(self.app, 0)
        port = httpd.server_address[1]
        threading.Thread(target=httpd.serve_forever, daemon=True).start()
        try:
            c = http.client.HTTPConnection("127.0.0.1", port, timeout=30)
            body = json.dumps({"urls": [self.web.url("/pack.zip")], "collection": "Via the page"}).encode()
            c.request("POST", "/api/url", body=body, headers={"Host": "127.0.0.1:%d" % port, "X-Loader-Token": self.app.token,
                                                                  "Content-Type": "application/json"})
            r = c.getresponse()
            j = json.loads(r.read().decode())
            c.close()
            self.assertEqual(r.status, 200)
            self.assertEqual(len(j["added"]), 3)
            self.assertEqual(self.app.state()["settings"]["last_collection"], "Via the page")
        finally:
            httpd.shutdown()
            httpd.server_close()


class ShellTests(LoaderTests):
    pass


class BusyboxTests(LoaderTests):
    mode = "busybox"


class NoMd5Tests(LoaderTests):
    mode = "nomd5"


del LoaderTests


class PureTests(unittest.TestCase):
    def test_names(self):
        self.assertEqual(loader.fold("Für Elise – Bagatelle"), "Fur Elise - Bagatelle")
        self.assertEqual(loader.safe_name("../../etc/passwd"), "etc_passwd")
        self.assertEqual(loader.safe_name(".hidden"), "hidden")
        self.assertEqual(loader.midi_file_name("Prelude in C# minor.MID"), "Prelude in C# minor.mid")
        self.assertEqual(loader.midi_file_name("weird:name?.midi"), "weird_name.midi")
        self.assertEqual(loader.title_from("goldberg_aria.mid"), "goldberg aria")

    def test_midi_info(self):
        i = loader.midi_info(smf([48, 60, 72, 84], tempo=1000000))
        self.assertEqual((i["tracks"], i["notes"], i["low"], i["high"]), (1, 4, "C3", "C6"))
        self.assertAlmostEqual(i["seconds"], 4.0)
        self.assertEqual(i["names"], ["Test"])
        with self.assertRaises(ValueError):
            loader.midi_info(b"RIFF....not midi")
        cut = loader.midi_info(smf([60, 62])[:-6])
        self.assertTrue(cut["problem"])
        real = rb(os.path.join(ROOT, "tests", "data", "minuet_g.mid"))
        r = loader.midi_info(real)
        self.assertGreater(r["notes"], 50)
        self.assertGreater(r["seconds"], 10)

    def test_plan_without_mpc_listing(self):
        scan = {"collections": [{"name": "A", "pieces": [{"rel": "A/x.mid", "file": "x.mid", "md5": "1", "size": 3,
                                                          "problem": "", "row": None}]}]}
        p = loader.make_plan(scan, None, {})
        self.assertEqual(p["add"], ["A/x.mid"])

    def test_survey_parsing(self):
        out = (b"@@ok\n@@id abc\n@@model MPC Live III\n@@hasmd5\n@@drive /nvme\n@@plugin /nvme/VST2 Plugins/X - VST - Sight Reader\n"
               b"@@version 0.6.0\n@@lib /nvme/SightReader/Library\n@@libok\n@@coll ./My Pieces\n"
               b"@@size    120 ./My Pieces/a b.mid\n@@size 33 ./My Pieces/index.tsv\n@@size 9 ./My Pieces/notes.txt\n@@size 999 total\n"
               b"@@md5 0123456789abcdef0123456789abcdef  ./My Pieces/a b.mid\n"
               b"@@index ./My Pieces/index.tsv\n|a b.mid\tA B\t\t\t\n|\n\n@@end\n")
        s = loader.parse_survey(out)
        lib = s["drives"][0]["lib"]
        self.assertEqual(s["model"], "MPC Live III")
        self.assertEqual(sorted(lib["files"]), ["My Pieces/a b.mid", "My Pieces/index.tsv"])
        self.assertEqual(lib["files"]["My Pieces/a b.mid"], {"size": 120, "md5": "0123456789abcdef0123456789abcdef"})
        self.assertEqual(loader.read_index_text(lib["index"]["My Pieces"])["a b.mid"][1], "A B")
        d, problem = loader.choose_drive(s)
        self.assertEqual((d["path"], problem), ("/nvme", ""))


class HttpTests(Base):
    def setUp(self):
        Base.setUp(self)
        self.httpd = loader.serve(self.app, 0)
        self.port = self.httpd.server_address[1]
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def tearDown(self):
        self.httpd.shutdown()
        self.httpd.server_close()
        Base.tearDown(self)

    def call(self, method, path, body=None, headers=None, token=True):
        c = http.client.HTTPConnection("127.0.0.1", self.port, timeout=30)
        h = {"Host": "127.0.0.1:%d" % self.port}
        if token:
            h["X-Loader-Token"] = self.app.token
        h.update(headers or {})
        if isinstance(body, dict):
            body = json.dumps(body).encode()
        c.request(method, path, body=body, headers=h)
        r = c.getresponse()
        data = r.read()
        c.close()
        try:
            return r.status, json.loads(data.decode()), r
        except ValueError:
            return r.status, data, r

    def test_page_and_guards(self):
        st, page, r = self.call("GET", "/", token=False)
        self.assertEqual(st, 200)
        self.assertIn(self.app.token.encode(), page)
        self.assertIn(b"Sight Reader MIDI Loader", page)
        self.assertIn("nonce-" + self.app.nonce, r.getheader("Content-Security-Policy"))
        self.assertNotIn(b"__TOKEN__", page)
        self.assertEqual(self.call("GET", "/api/state", token=False)[0], 403)                     # no token
        self.assertEqual(self.call("GET", "/", headers={"Host": "evil.example:%d" % self.port}, token=False)[0], 403)
        self.assertEqual(self.call("GET", "/api/state", headers={"Host": "evil.example"})[0], 403)  # DNS rebinding
        self.assertEqual(self.call("POST", "/api/restart", {}, headers={"Origin": "http://evil.example"})[0], 403)
        self.assertEqual(self.call("GET", "/api/ping", token=False)[1]["app"], loader.APP)

    def test_upload_edit_and_sync_through_the_api(self):
        st, j, _ = self.call("POST", "/api/upload?collection=My%20Pieces&name=Aria.mid", b"not midi at all")
        self.assertEqual(st, 400)
        self.assertIn("not a MIDI file", j["error"])
        st, j, _ = self.call("POST", "/api/upload?collection=My%20Pieces&name=Aria.mid", smf([60, 64, 67]))
        self.assertEqual((st, j["collection"], j["file"]), (200, "My Pieces", "Aria.mid"))
        st, j, _ = self.call("POST", "/api/piece", {"collection": "My Pieces", "file": "Aria.mid", "title": "Aria",
                                                    "composer": "J. S. Bach", "level": "3"})
        self.assertEqual(st, 200)
        st, s, _ = self.call("GET", "/api/state")
        piece = s["library"]["collections"][0]["pieces"][0]
        self.assertEqual((piece["title"], piece["composer"], piece["level"], piece["status"]), ("Aria", "J. S. Bach", "3", "unknown"))
        self.assertEqual(piece["info"]["notes"], 3)
        self.assertEqual(s["settings"]["last_collection"], "My Pieces")
        st, j, _ = self.call("POST", "/api/look", {})
        self.assertEqual(st, 200)
        self.app.wait(30)
        st, j, _ = self.call("POST", "/api/sync", {"remove": True})
        self.assertEqual(st, 200)
        job = self.app.wait(60)
        self.assertTrue(job.ok, job.message)
        self.assertTrue(os.path.isfile(self.remote("My Pieces/Aria.mid")))
        st, s, _ = self.call("GET", "/api/state")
        self.assertEqual(s["library"]["collections"][0]["pieces"][0]["status"], "same")
        self.assertTrue(s["plan"]["empty"])
        st, j, _ = self.call("POST", "/api/collection", {"name": "Generated"})
        self.assertEqual(st, 400)                                                                    # reserved
        st, j, _ = self.call("POST", "/api/piece/delete", {"collection": "My Pieces", "file": "../../x"})
        self.assertEqual(st, 400)


if __name__ == "__main__":
    unittest.main(verbosity=1)
