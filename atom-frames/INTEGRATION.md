# Wiring a spinner build into the engine

Where the knobs live for the editor's thumbnail loading spinner, and what has to
change together when you swap in a new atom-spinner version.

## The three knobs

| What | Where | Notes |
|---|---|---|
| **Backend** (TTF vs WebP) | `modules/Debug/include/Assisi/Debug/DebugUI.hpp` — `DebugUI::kUseWebpSpinner` | `true` → animated WebP, `false` → TTF glyph frames. Compile-time; flip and rebuild. |
| **TTF frame count** | `apps/sandbox/src/SandboxAssetBrowser.cpp` — `kTtfFrameCount` | Must equal the font's glyph count. TTF path only. |
| **Playback speed** | `apps/sandbox/src/SandboxAssetBrowser.cpp` — `kLoadingFps` | **Drives BOTH backends.** |

Also in `SandboxAssetBrowser.cpp`: `kTtfFirstFrame = 0xF000`, the codepoint of the
first frame glyph. All `build_font_v*.py` scripts map frames to `U+F000 + i`, so
this only changes if the bake's PUA base changes.

Both backends load from fixed virtual paths (`modules/Debug/src/DebugUI.cpp`):

    assets/editor/loading/Spinner.ttf
    assets/editor/loading/Spinner.webp

Fixed *names* — swapping versions means copying the chosen build over these, not
pointing a path at `Spinner4-30.ttf`. Either asset may be absent; the browser
falls back to a plain placeholder, so a half-shipped pair never crashes.

## The gotcha: kLoadingFps is shared

The WebP path advances its own time-driven counter at `kLoadingFps`
(`SandboxAssetBrowser.cpp`, `DrawWebpLoadingFrame`) and **ignores the per-frame
durations baked into the .webp**. So the file's own frame timing is decorative as
far as the engine is concerned — the rate is whatever `kLoadingFps` says.

Consequence: dropping in a 15 fps WebP while `kLoadingFps` is still 60 plays it
4x too fast (a 10 s loop finishes in 2.5 s). The frame *count* is auto-detected
from the decode, so only the rate needs syncing on the WebP side; the TTF side
needs both `kTtfFrameCount` and `kLoadingFps`.

## Settings per build

v3 (2 s loop, spin every loop) — the current shipped values:

    kTtfFrameCount = 120
    kLoadingFps    = 60.0f      // 120 / 60 = 2 s

v4 (10 s loop, protons at v3 tempo for 5 cycles, one spin in the last 1.5 s).
Pick a variant and use the whole row — `kLoadingFps` must match the variant's
fps for BOTH backends:

| Variant | TTF | WebP | `kTtfFrameCount` | `kLoadingFps` |
|---|---|---|---|---|
| v4-15 | `Spinner4-15.ttf` | `atom-spinner-v4-15.webp` | 150 | `15.0f` |
| v4-30 | `Spinner4-30.ttf` | `atom-spinner-v4-30.webp` | 300 | `30.0f` |
| v4-60 | `Spinner4-60.ttf` | `atom-spinner-v4-60.webp` | 600 | `60.0f` |

All three sample the same 10 s loop, so they differ only in smoothness and size
(TTF: 547 KB / 1.1 MB / 2.2 MB). Mixing rows — e.g. `Spinner4-60.ttf` with
`kTtfFrameCount = 300` — desyncs the loop and the spin lands mid-cycle.

## Rebuilding

Needs fontTools + shapely + Pillow. On this machine `python-fonttools` and
`python-pillow` are installed via pacman but **shapely is not**, so the v4 build
was run from a venv:

    python3 -m venv --system-site-packages .venv
    .venv/bin/pip install shapely
    .venv/bin/python build_font_v4.py          # all three TTFs (add --svg for SVG frames)
    .venv/bin/python render_webp_v4.py         # all three WebPs
    .venv/bin/python build_font_v4.py 30       # or one variant

Use `python -u` if you want progress to stream — stdout is block-buffered when
redirected, so a quiet log does not mean a stalled render.

### Render cost

The WebP render is the slow part: ~0.7-0.9 s/frame, ~13 min for all 1050 frames.
Known-but-unapplied speedups, if it ever needs to be faster:

- **Variant decimation.** 600/300/150 divide exactly and all sample the same
  loop, so v4-30 is v4-60's even frames and v4-15 is every 4th. Rendering 600
  once and striding would give all three for 57% of the work.
- **Still-frame short-circuit.** 85% of a v4 loop is motionless (no spin until
  8.5 s). When `sspeed == 0` the whole-atom stage still composites 3 identical
  copies and does a `rotate(0)` — ~0.47 s of a 0.70 s still frame. The lead copy
  is alpha-255 opaque, so the other two contribute nothing.
- **`SS = 3` supersamples to 1536x1536 for a 256 px output** — 6x linear, 36x the
  pixels kept. `SS = 2` is still 4x linear and cuts pixel work ~2.25x. This one
  is a real quality change, unlike the two above.
