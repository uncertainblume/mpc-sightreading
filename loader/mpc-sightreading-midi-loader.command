#!/bin/sh
# Double-click in Finder to start the Sight Reader MIDI Loader: it opens its page in your browser.
# Keep this window open while you use the page; close it (or press Quit on the page) to stop.
cd "$(dirname "$0")" || exit 1
if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 is not installed. On a Mac, run:  xcode-select --install"
    echo "(it installs Apple's Command Line Tools, which include python3 and ssh), then try again."
    read -r _
    exit 1
fi
exec python3 ./mpc-sightreading-midi-loader.py "$@"
