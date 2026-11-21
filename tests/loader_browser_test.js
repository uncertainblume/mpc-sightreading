// The MIDI loader's page in a real browser (Chromium through Playwright), against a pretend MPC.
//
//   node tests/loader_browser_test.js [screenshot folder]
//
// Starts loader/mpc-sightreading-midi-loader.py with tests/fake_ssh.py as its ssh, then does what
// a person would: first-run settings, adding a file, a drop onto a collection, Sync, Restart,
// a reload (the settings are remembered), Quit. Needs node and the playwright package.
"use strict";
const { chromium } = require("playwright");
const { spawn } = require("child_process");
const http = require("http");
const fs = require("fs");
const os = require("os");
const path = require("path");

const ROOT = path.resolve(__dirname, "..");
const shots = process.argv[2] || "";
let failures = 0;
function check(cond, what) { if (!cond) { failures++; console.log("FAIL", what); } else console.log("ok  ", what); }

function smf(notes) {
  const ev = [0x00, 0xff, 0x51, 0x03, 0x07, 0xa1, 0x20];
  for (const n of notes) ev.push(0x00, 0x90, n, 80, 0x83, 0x60, 0x80, n, 0);
  ev.push(0x00, 0xff, 0x2f, 0x00);
  const len = [(ev.length >> 24) & 255, (ev.length >> 16) & 255, (ev.length >> 8) & 255, ev.length & 255];
  return Buffer.from([0x4d, 0x54, 0x68, 0x64, 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xe0, 0x4d, 0x54, 0x72, 0x6b, ...len, ...ev]);
}

(async () => {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "srml-browser-"));
  const drive = path.join(tmp, "mpc", "nvme");
  const plugin = path.join(drive, "VST2 Plugins", "Practice Tools - VST - Sight Reader");
  fs.mkdirSync(plugin, { recursive: true });
  fs.writeFileSync(path.join(plugin, "plugin.json"), '{"name": "Sight Reader", "version": "0.6.0"}\n');
  const bin = path.join(tmp, "bin");
  fs.mkdirSync(bin);
  fs.writeFileSync(path.join(bin, "systemctl"), `#!/bin/sh\necho "$*" >> "${tmp}/systemctl.log"\ncase "$1" in cat|restart) exit 0;; is-active) echo active; exit 0;; esac\nexit 1\n`);
  fs.chmodSync(path.join(bin, "systemctl"), 0o755);
  const lib = path.join(tmp, "computer", "Library");
  // a small web site for Add from a link
  const site = http.createServer((req, res) => {
    if (req.url === "/aria.mid") { res.writeHead(200, { "Content-Type": "audio/midi" }); res.end(smf([67, 69, 71])); }
    else if (req.url === "/list.html") {
      res.writeHead(200, { "Content-Type": "text/html" });
      res.end('<html><head><title>Some pieces</title></head><body><a href="one.mid">one</a> <a href="two.mid">two</a></body></html>');
    }
    else if (req.url === "/one.mid" || req.url === "/two.mid") { res.writeHead(200, { "Content-Type": "audio/midi" }); res.end(smf([60, 62])); }
    else { res.writeHead(404); res.end(); }
  });
  await new Promise(r => site.listen(0, "127.0.0.1", r));
  const siteUrl = p => "http://127.0.0.1:" + site.address().port + p;
  const env = Object.assign({}, process.env, {
    MPC_SSH_BIN: path.join(ROOT, "tests", "fake_ssh.py"), MPC_SR_LOADER_HOME: path.join(tmp, "settings"),
    MPC_SR_DRIVES: drive, FAKE_MPC_PATH: bin + ":" + process.env.PATH, FAKE_MPC_HOME: tmp,
  });
  const proc = spawn("python3", [path.join(ROOT, "loader", "mpc-sightreading-midi-loader.py"), "--no-browser", "--port", "0", "--library", lib], { env });
  let out = "", exitCode = null;
  proc.on("exit", c => { exitCode = c; });                 // from the start: Quit may be quicker than us
  proc.stdout.on("data", d => { out += d; });
  proc.stderr.on("data", d => { process.stderr.write(d); });
  const url = await new Promise((resolve, reject) => {
    const t = setInterval(() => { const m = out.match(/Page:\s+(http:\/\/127\.0\.0\.1:\d+\/)/); if (m) { clearInterval(t); resolve(m[1]); } }, 50);
    setTimeout(() => reject(new Error("the loader did not start: " + out)), 15000);
  });
  check(!/192\.168|10\.\d+\.\d+\.\d+/.test(out), "no MPC address printed before one is entered");
  const browser = await chromium.launch();
  const context = await browser.newContext({ viewport: { width: 1280, height: 860 }, deviceScaleFactor: shots ? 2 : 1 });
  const page = await context.newPage();
  const errors = [];
  page.on("pageerror", e => errors.push(String(e)));
  page.on("console", m => { if (m.type() === "error") errors.push(m.text()); });
  try {
    await page.goto(url);
    await page.waitForSelector("#settings:not([hidden])");
    check(await page.inputValue("#f-address") === "", "first run: the address is empty (no default)");
    check((await page.textContent("#pill-text")).includes("not set up"), "pill says the MPC is not set up");
    if (shots) await page.screenshot({ path: path.join(shots, "loader-first-run.png") });
    await page.fill("#f-address", "mpc.test");
    await page.click("#settings-form button[type=submit]");
    await page.waitForFunction(() => document.querySelector("#pill-text").textContent.includes("Sight Reader 0.6.0"), null, { timeout: 15000 });
    check(await page.isHidden("#settings"), "settings close after Save and connect");
    // add a file through the file picker, into a new collection
    await page.setInputFiles("#file-input", { name: "Goldberg Aria.mid", mimeType: "audio/midi", buffer: smf([67, 69, 71, 72, 74]) });
    await page.waitForSelector("#m-new");
    await page.fill("#m-new", "Goldberg");
    await page.click("#dialog .btn.primary");
    await page.waitForSelector('.piece[data-file="Goldberg Aria.mid"] .badge.new', { timeout: 10000 });
    check(true, "the new piece shows as New");
    check((await page.textContent("#sync-summary")).includes("1 new"), "the sync bar counts it");
    // drop two files onto the collection (one is not MIDI)
    await page.evaluate(({ bytes }) => {
      const dt = new DataTransfer();
      dt.items.add(new File([new Uint8Array(bytes)], "Rhapsody in Blue.mid", { type: "audio/midi" }));
      dt.items.add(new File(["hello"], "notes.txt", { type: "text/plain" }));
      const target = document.querySelector('.coll[data-coll="Goldberg"] .pieces');
      target.dispatchEvent(new DragEvent("dragenter", { dataTransfer: dt, bubbles: true }));
      target.dispatchEvent(new DragEvent("dragover", { dataTransfer: dt, bubbles: true, cancelable: true }));
      target.dispatchEvent(new DragEvent("drop", { dataTransfer: dt, bubbles: true, cancelable: true }));
    }, { bytes: Array.from(smf([60, 64, 67])) });
    await page.waitForSelector('.piece[data-file="Rhapsody in Blue.mid"]', { timeout: 10000 });
    check((await page.textContent("#toast")).includes("1 file skipped"), "a dropped .txt is skipped and said so");
    check(fs.existsSync(path.join(lib, "Goldberg", "Rhapsody in Blue.mid")), "the drop landed in the collection it was dropped on");
    // edit a title
    await page.click('.piece[data-file="Goldberg Aria.mid"] .piece-main');
    await page.fill('#piece-form input[name=title]', "Aria (Goldberg Variations)");
    await page.fill('#piece-form input[name=composer]', "J. S. Bach");
    await page.selectOption('#piece-form select[name=level]', "3");
    await page.click("#dialog .btn.primary");
    await page.waitForFunction(() => document.body.textContent.includes("Aria (Goldberg Variations)"));
    check(true, "the title edit shows");
    if (shots) await page.screenshot({ path: path.join(shots, "loader-ready-to-sync.png") });
    // sync
    await page.click("#btn-sync");
    await page.waitForSelector("#dialog >> text=Sync now");
    await page.click("#dialog >> text=Sync now");
    await page.waitForSelector("#dialog .alert.ok", { timeout: 20000 });
    check((await page.textContent("#dialog")).includes("Synced: 2 added"), "sync reports 2 added");
    if (shots) await page.screenshot({ path: path.join(shots, "loader-synced.png") });
    await page.click("#dialog .btn.primary");
    const remote = path.join(drive, "SightReader", "Library", "Goldberg");
    check(fs.existsSync(path.join(remote, "Goldberg Aria.mid")) && fs.existsSync(path.join(remote, "Rhapsody in Blue.mid")), "both files are on the MPC");
    check(fs.readFileSync(path.join(remote, "index.tsv"), "utf8").includes("Aria (Goldberg Variations)\tJ. S. Bach\t3"), "the MPC's index.tsv has the title");
    check(fs.existsSync(path.join(drive, "SightReader", "Library", ".rescan")), "the rescan marker is there");
    await page.waitForSelector('.piece[data-file="Goldberg Aria.mid"] .badge.same');
    check((await page.textContent("#sync-summary")).includes("has all your pieces"), "nothing left to sync");
    // 0.6.5: add from a link, then from a page of links
    await page.click("text=Add from a link…");
    await page.fill("#m-url", siteUrl("/aria.mid"));
    if (shots) await page.screenshot({ path: path.join(shots, "loader-link.png") });
    await page.click("#dialog .btn.primary");
    await page.waitForSelector('.piece[data-file="aria.mid"] .badge.new', { timeout: 15000 });
    check(true, "a .mid link is added (and is New for the MPC)");
    await page.click("text=Add from a link…");
    await page.fill("#m-url", siteUrl("/list.html"));
    await page.click("#dialog .btn.primary");
    await page.waitForSelector("#dialog input[data-link]", { timeout: 15000 });
    check((await page.textContent("#dialog")).includes("Some pieces"), "a page's MIDI links are offered, under its title");
    await page.click("#m-all");
    await page.click("#dialog >> text=Add selected");
    await page.waitForSelector('.piece[data-file="two.mid"]', { timeout: 15000 });
    check(await page.isVisible('.piece[data-file="one.mid"]'), "the picked links are added");
    // restart
    await page.click("#btn-restart");
    await page.click("#dialog .btn.danger");
    await page.waitForSelector("#dialog .alert.ok", { timeout: 20000 });
    check((await page.textContent("#dialog")).includes("running again"), "restart reports the app is back");
    check(fs.readFileSync(path.join(tmp, "systemctl.log"), "utf8").includes("restart acvs"), "systemctl restart acvs was run");
    await page.click("#dialog .btn.primary");
    // reload: the address is remembered and the page connects by itself
    await page.reload();
    await page.waitForFunction(() => document.querySelector("#pill-text").textContent.includes("Sight Reader 0.6.0"), null, { timeout: 15000 });
    check(await page.isHidden("#settings"), "after a reload the settings stay closed (remembered)");
    await page.click("#btn-settings");
    check(await page.inputValue("#f-address") === "mpc.test", "the remembered address is in Settings");
    await page.click("#btn-settings");
    if (shots) {
      const dark = await browser.newContext({ viewport: { width: 1280, height: 860 }, deviceScaleFactor: 2, colorScheme: "dark" });
      const dp = await dark.newPage();
      await dp.goto(url);
      await dp.waitForFunction(() => document.querySelector("#pill-text").textContent.includes("Sight Reader"), null, { timeout: 15000 });
      await dp.screenshot({ path: path.join(shots, "loader-dark.png") });
      await dark.close();
    }
    // quit
    await page.click("text=Quit loader");
    await page.click("#dialog .btn.primary");
    await page.waitForSelector("#stopped:not([hidden])");
    for (let i = 0; i < 80 && exitCode === null; i++) await new Promise(r => setTimeout(r, 100));
    const code = exitCode === null ? "still running" : exitCode;
    check(code === 0, "Quit stops the loader (exit " + code + ")");
    check(errors.length === 0, "no errors in the page" + (errors.length ? ": " + errors.join(" | ") : ""));
  } catch (e) {
    failures++;
    console.log("FAIL", e.message);
    if (shots) await page.screenshot({ path: path.join(shots, "loader-failure.png") }).catch(() => {});
  } finally {
    await browser.close();
    site.close();
    try { proc.kill(); } catch (e) { /* gone */ }
    fs.rmSync(tmp, { recursive: true, force: true });
  }
  console.log("loader_browser_test: " + (failures ? "FAILED (" + failures + ")" : "ok"));
  process.exit(failures ? 1 : 0);
})();
