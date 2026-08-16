#!/usr/bin/env bash
# Launch the sandbox under RenderDoc so a frame can be captured with F12.
#
# Exists because two things about this are not guessable, and both fail in ways
# that look like an engine bug rather than a tooling one:
#
#   1. RenderDoc cannot capture Wayland. It does not expose
#      VK_KHR_wayland_surface, so window creation fails outright with
#      "Window surface creation extensions not found".
#   2. Unsetting WAYLAND_DISPLAY is not enough to escape Wayland: libwayland
#      falls back to the default "wayland-0" socket. The variable has to point
#      at a socket that does not exist, so the connection fails and GLFW falls
#      back to X11 (XWayland).
#
# Usage:
#   scripts/rdc-capture.sh [-b BUILD_DIR] [-o OUT_DIR] [-- app args...]
#
# Then press F12 in the app window to capture a frame. Captures land in OUT_DIR.
# Analyse one with:
#   RDC_CAPTURE=<file>.rdc qrenderdoc --python scripts/rdc-analyze.py

set -euo pipefail

BUILD_DIR="out/build/gcc-ship-chiara/apps/sandbox"
OUT_DIR="$(pwd)/captures-rdc"
APP_ARGS=(-l levels/Lights.alvl)

while [[ $# -gt 0 ]]; do
    case "$1" in
        -b|--build-dir) BUILD_DIR="$2"; shift 2 ;;
        -o|--out-dir)   OUT_DIR="$2";   shift 2 ;;
        --)             shift; APP_ARGS=("$@"); break ;;
        -h|--help)      sed -n '2,22p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

command -v renderdoccmd >/dev/null || {
    echo "renderdoccmd not found. Install RenderDoc (Arch: sudo pacman -S renderdoc)." >&2
    exit 1
}

[[ -x "$BUILD_DIR/Assisi-Sandbox" ]] || {
    echo "no Assisi-Sandbox in $BUILD_DIR — build it first with: make gs-c" >&2
    exit 1
}

mkdir -p "$OUT_DIR"

# GPU markers only exist in a build configured with ASSISI_ENABLE_GPU_MARKERS
# (the -chiara presets turn it on). Without them the capture still works, it is
# just a flat list of anonymous draws — worth warning about, not worth blocking.
CACHE="$(dirname "$(dirname "$BUILD_DIR")")/CMakeCache.txt"
if [[ -f "$CACHE" ]] && ! grep -q "ASSISI_ENABLE_GPU_MARKERS:BOOL=ON" "$CACHE"; then
    echo "warning: this build has no GPU markers; passes will be unnamed." >&2
    echo "         rebuild with 'make gs-c' for a labelled capture." >&2
fi

echo "Launching under RenderDoc (forced onto XWayland)."
echo "  Press F12 in the app window to capture a frame."
echo "  Captures -> $OUT_DIR"

cd "$BUILD_DIR"
exec env \
    WAYLAND_DISPLAY=nonexistent-sock \
    XDG_SESSION_TYPE=x11 \
    DISPLAY="${DISPLAY:-:0}" \
    renderdoccmd capture -w -c "$OUT_DIR/assisi" ./Assisi-Sandbox "${APP_ARGS[@]}"
