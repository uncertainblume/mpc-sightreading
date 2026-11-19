#!/bin/sh
# Builds Sight Reader for the MPC (arm32 + arm64) and this computer, with Akai's SDK.
#   tools/build.sh                 regenerate skins and drills if Pillow is installed, then build
#   tools/build.sh --target native extra arguments go to the SDK's build.py
# Output: plugin/build/"Practice Tools - VST - Sight Reader"/ - copy that folder into
# "VST2 Plugins" on the MPC's internal drive (or use the SDK's mpc_remote.py, see README).
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SDK="$ROOT/third_party/mpc-plugin-builder-sdk"
[ -f "$SDK/tools/build.py" ] || "$ROOT/tools/setup_sdk.sh"
if python3 -c "import PIL" 2>/dev/null; then
    python3 "$ROOT/tools/gen_skin.py"
    python3 "$ROOT/tools/make_drills.py" >/dev/null
else
    echo "Pillow not installed: using the committed skins (pip install pillow to regenerate them)"
fi
python3 "$SDK/tools/build.py" "$ROOT/plugin" "$@"
