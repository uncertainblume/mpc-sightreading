#!/bin/sh
# Desktop tests: core unit tests (with sanitizers), then the built plugin driven through its
# VST2 interface with a small library, then Akai's host simulator. Needs a C++17 compiler;
# the last two need tools/build.sh to have run (the native build).
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
