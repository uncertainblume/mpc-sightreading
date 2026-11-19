#!/bin/sh
# Fetches Akai's MPC Plugin SDK (MIT) at the commit this project was built against, into
# third_party/mpc-plugin-builder-sdk (not committed). Safe to re-run.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SDK_URL=https://github.com/inmusic-software/mpc-plugin-builder-sdk
SDK_COMMIT=a5949cf6fe2cbfb95bcbe0ddf1f7ef555e71f2be
DEST="$ROOT/third_party/mpc-plugin-builder-sdk"
if [ ! -d "$DEST/.git" ]; then
    mkdir -p "$ROOT/third_party"
    git clone --quiet "$SDK_URL" "$DEST"
fi
git -C "$DEST" cat-file -e "$SDK_COMMIT^{commit}" 2>/dev/null || git -C "$DEST" fetch --quiet origin
git -C "$DEST" checkout --quiet "$SDK_COMMIT"
echo "MPC Plugin SDK ready at $DEST ($SDK_COMMIT)"
