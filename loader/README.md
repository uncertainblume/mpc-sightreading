# mpc-sightreading-midi-loader

A page in your browser for putting your own MIDI files on the MPC for Sight Reader. You drop
`.mid` files into collections on your computer, press **Sync**, and the MPC's Sight Reader
library matches your computer's. Sight Reader 0.6.0 and later show the change by themselves a
couple of seconds later.

All the work happens on your computer. The page is served by a small Python program on your
computer, at an address only your computer can open (`http://127.0.0.1:47823/`). It talks to the
MPC over SSH, as the SDK's `mpc_remote.py` does. Nothing is installed or left running on the MPC:
a sync is one SSH login, one archive unpacked, and the pieces you removed deleted.

## Start it

On a Mac, double-click **`mpc-sightreading-midi-loader.command`** in this folder. A Terminal window
opens, and the page opens in your browser. Keep the window open while you use the page; **Quit
loader** on the page, or closing the window, stops it.

Or from a terminal, in the repository:

```bash
python3 loader/mpc-sightreading-midi-loader.py
```

It needs Python 3.8 or later and the `ssh` command. Macs have both once Apple's Command Line
Tools are installed (`xcode-select --install`; Homebrew installs them too); Linux has them, and
so does Windows 10/11 (OpenSSH Client). It uses only Python's standard library: nothing to `pip
install`.

The MPC must be in developer mode, which starts its SSH server. On the MPC: Preferences > Info, tap
the version value 3 times, then the serial number value 2 times, then the firmware version once.

## The first time

The page asks for the MPC's IP address. The MPC shows it in Preferences: the Wi-Fi tab (hold
Shift to reach it) or the Ethernet tab. Your router's device list has it too. There is no
default: you type it once, and the loader remembers it in your user settings folder, never in the
repository:

| | |
|---|---|
| macOS | `~/Library/Application Support/mpc-sightreading-midi-loader/` |
| Linux | `~/.config/mpc-sightreading-midi-loader/` |
| Windows | `%APPDATA%\mpc-sightreading-midi-loader\` |

`settings.json` there has the address, the SSH user and port, the drive and the library folder.
`synced.json` lists which files this computer put on which MPC. An SSH password, for an MPC that
asks for one, is kept only while the loader runs.

## The library

The library is a folder on your computer with one folder per **collection**. A collection is a
folder in Sight Reader's LOAD list.

```
Library folder/
  Gershwin/
    Rhapsody in Blue (easy).mid
    index.tsv          titles, composers and levels (the loader writes it)
  Goldberg/
    Aria.mid
```

Run from the repository, the loader uses `library/cache/`, the folder `tools/library.py` fills
from the Mutopia Project, so both tools share one library. Otherwise it uses `~/Music/Sight
Reader Library`. **Settings > Library folder** picks another, remembered too.

You can also manage the folder in Finder: the page sees what you add, rename or delete there. On
the page:
- **Add MIDI files**, or drop files anywhere on the page. Drop them onto a collection to put them
  in it.
- Click a piece to set its **title**, **composer** and **level** (1 easiest to 5). Sight Reader's
  LOAD page shows them, and sorts by level. The MPC's screen shows plain letters, so accents are
  left out (*Für Elise* becomes *Fur Elise*).
- **Delete** moves a piece or a collection to the Trash.

Each piece shows what is in it (length, range, notes, tracks) and its state against the MPC:
**New**, **Changed** or **On the MPC**.

### Add from a link (0.6.5)

**Add from a link…** takes a web link, and so does a link dragged onto the page from another
browser tab:
- **a `.mid` file**: added;
- **a `.zip` of MIDI files** (the Mutopia Project's `-mids.zip`, for example): every MIDI file in
  it is added;
- **a web page**: the loader lists the page's `.mid` and `.zip` links, under the page's title, and
  you tick the ones to add.

Share links from GitHub (a file's page), Google Drive (*Anyone with the link*) and Dropbox are
turned into links to the file itself. The link is kept as the piece's source in `index.tsv`.

Your computer downloads it, as your browser would (at most 64 MB, a MIDI file at most 8 MB). The
MPC never touches the web. Some sites only let a browser download and turn scripts away. The page
says so: then open the link in your browser, save the file, and drop it on the page. A Mac whose
Python came from python.org may need *Install Certificates.command* (in its Python folder) once,
for secure links.

## Sync

**Sync to the MPC** shows what it will do, then does it:
- It copies new pieces and pieces that changed here.
- It updates the titles.
- It removes the pieces you removed here. A tick box lets you keep them on the MPC this time.

It only touches `SightReader/Library` on the drive Sight Reader is installed on. Your progress,
playlist and the exercises saved from GEN are never touched.

It only removes pieces this computer put there. Pieces that reached the MPC some other way
(another computer, `tools/library.py`, by hand) are listed under **Only on the MPC**. Sync leaves
them alone. There you can **Copy to this computer** (then they are part of your library, and Sync
keeps them) or **Delete from the MPC**.

Files copied earlier with `tools/library.py push` are recognised when they are the same as here,
so nothing is copied twice.

Sight Reader keeps your progress per file location. Renaming a collection, or moving a file to
another collection, starts that piece's record again on the MPC.

## Restart the MPC app

**Restart the MPC app…** closes and reopens the MPC application (`systemctl restart acvs`, as the
SDK's `mpc_remote.py restart` does) and waits until it is back, about a minute. Anything unsaved
in the open project is lost, so save first. The MPC itself stays on.

## Options

```
--port N        the page's port on this computer (default 47823; remembered)
--library DIR   the library folder (remembered)
--no-browser    do not open the browser
```

If the loader is running already, starting it again just opens the page.

## When something goes wrong

The page says what it found and what to try:
- **Nothing answers**: the MPC is off, on another network, or the address is wrong.
- **SSH server is off**: developer mode is off.
- **Login turned down**: the MPC wants a password (Settings).
- **Sight Reader is not installed**: install the plugin first (see the main README).

Sight Reader older than 0.6.0 does not notice a sync by itself: press RESCAN on its LOAD page.

Tests: `tests/loader_test.py` runs every operation against a pretend MPC, under `sh` and
`busybox`, and `tests/loader_browser_test.js` drives the page in Chromium (`tools/run_tests.sh`
runs both).
