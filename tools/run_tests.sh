#!/bin/sh
# Desktop tests: core unit tests (with sanitizers), then the built plugin driven through its
# VST2 interface with a small library, then Akai's host simulator, then the MIDI loader. Needs a
# C++17 compiler; the plugin-level tests need tools/build.sh to have run (the native build).
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SDK="$ROOT/third_party/mpc-plugin-builder-sdk"
OUT="$ROOT/tests/build"; mkdir -p "$OUT"
CXX=${CXX:-c++}
"$CXX" -std=c++17 -Wall -Wextra -O1 -g -fsanitize=address,undefined -I"$ROOT/plugin/src" "$ROOT/tests/sr_test.cpp" -o "$OUT/sr_test"
"$OUT/sr_test" selftest
NATIVE="$ROOT/plugin/build/native/sight-reader.so"
[ -f "$NATIVE" ] || NATIVE="$ROOT/plugin/build/native/sight-reader.dylib"
if [ -f "$NATIVE" ] && [ -d "$SDK/framework" ]; then
    HOME_DIR=$(mktemp -d)
    mkdir -p "$HOME_DIR/Library/Classics"
    cp "$ROOT/tests/data/minuet_g.mid" "$HOME_DIR/Library/Classics/anna-magdalena-04.mid"
    printf 'anna-magdalena-04.mid\tMinuet in G\tJ. S. Bach (attr.), BWV Anh. 114\t1\tMutopia Project, public domain\n' > "$HOME_DIR/Library/Classics/index.tsv"
    "$CXX" -std=c++17 -O1 -g -I"$SDK/framework" -I"$ROOT/plugin/src" "$ROOT/tests/plugin_test.cpp" -o "$OUT/plugin_test" -ldl -lpthread
    "$OUT/plugin_test" "$NATIVE" "$HOME_DIR" "$OUT/live.json"
    SIGHTREADER_HOME="$HOME_DIR" "$SDK/.sdk-tools/host_sim" "$NATIVE" test | tail -3
    rm -rf "$HOME_DIR"
else
    echo "(run tools/build.sh first for the plugin-level tests)"
fi
# The MIDI loader (Python): against a pretend MPC, under sh and (when installed) busybox
if python3 "$ROOT/tests/loader_test.py" >"$OUT/loader_test.log" 2>&1; then tail -3 "$OUT/loader_test.log"; else cat "$OUT/loader_test.log"; exit 1; fi
# ... and its page in a browser, where node and Playwright are installed
if command -v node >/dev/null 2>&1 && node -e "require('playwright')" >/dev/null 2>&1; then
    if node "$ROOT/tests/loader_browser_test.js" >"$OUT/loader_browser.log" 2>&1; then tail -1 "$OUT/loader_browser.log"; else cat "$OUT/loader_browser.log"; exit 1; fi
else
    echo "(node with playwright not found: the loader's browser test is skipped)"
fi
