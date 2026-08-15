# GPU profiling on Linux + Wayland + NVIDIA

**What this is for:** finding out where the GPU spends its time in a frame — which
pass costs what. Chiara (the engine's own profiler) measures the CPU and reports
one whole-frame GPU number. This is how you break that number apart.

**Who this is for:** someone who has never used a GPU profiler. Every step is
spelled out. If you already know RenderDoc, skip to
[The short version](#the-short-version).

**Why this document exists:** the Wayland + NVIDIA combination has three failure
modes that each look like an engine bug and are each ~30 minutes to diagnose.
They are all written down in [Gotchas](#gotchas-and-why-they-happen). Read that
section before you conclude something is broken.

---

## The short version

For someone who has done this before:

```bash
sudo pacman -S renderdoc                      # once
make gs-c                                     # optimized build, markers + Chiara on
scripts/rdc-capture.sh                        # launches under RenderDoc; press F12
RDC_CAPTURE=captures-rdc/assisi_frame402.rdc \
    qrenderdoc --python scripts/rdc-analyze.py
cat captures-rdc/assisi_frame402-gpu.txt
```

---

## Background: the two halves of a frame

A frame has a CPU side and a GPU side, and they are measured by different tools.

| | Tool | What it tells you |
|---|---|---|
| CPU | **Chiara** (built into the engine) | Which engine code took how long. Always on in `-chiara` builds; press F9 to dump. |
| GPU | **RenderDoc** (separate program) | Which draw/dispatch took how long on the graphics card. |

They meet through **GPU markers**: labels the engine writes into the command
stream that carry the same names as Chiara's scopes (`scene`, `light-cull`,
`imgui`). Without them RenderDoc shows a flat list of ~350 anonymous Vulkan
commands and you have to work out which is which by counting. With them you get
your own pass names. They are enabled automatically in any `-chiara` build.

RenderDoc is a **side program**. It requires no changes to your code — it
attaches to the running app. The markers are the only thing the engine does for
it, and they are optional.

---

## Step 1 — Install RenderDoc

```bash
sudo pacman -S renderdoc
```

Check it worked:

```bash
renderdoccmd version
```

You should see a version and `APIs supported at compile-time: Vulkan, GL, GLES`.
Vulkan is the one that matters.

This installs two programs:
- `renderdoccmd` — the command-line half, used to launch the app.
- `qrenderdoc` — the graphical half, used to look at captures.

## Step 2 — Build the engine

```bash
make gs-c
```

`gs-c` = **g**cc **s**hip + **c**hiara: optimized, with profiling compiled in.

**Use an optimized build.** A debug build is 10–40× slower on the CPU and will
mislead you badly — see [Debug builds lie](#debug-builds-lie) below.

## Step 3 — Capture a frame

```bash
scripts/rdc-capture.sh
```

The app launches. Move the camera to whatever you want to measure, then
**press F12**. That captures one frame. Press it again for more. Close the app
when done.

Captures land in `captures-rdc/` as `assisi_frameNNN.rdc`.

The script exists because the launch needs three environment variables set in a
non-obvious way; see [Gotchas](#gotchas-and-why-they-happen). If you want to
capture a different level:

```bash
scripts/rdc-capture.sh -- -l levels/Materials.alvl
```

## Step 4 — Read the numbers

```bash
RDC_CAPTURE=captures-rdc/assisi_frame402.rdc \
    qrenderdoc --python scripts/rdc-analyze.py
```

This runs without opening a window and writes a report next to the capture
(`assisi_frame402-gpu.txt`). Output looks like:

```
368 actions, 329 timed, 8 marker groups
total per-event GPU: 0.5853 ms

    GPU ms   share  events   pass
------------------------------------------------------------------------------
    0.2499   42.7%       3   scene / draw-scene / draw-submit
    0.2364   40.4%       5   scene / lighting / light-cull
    0.0553    9.4%       2   post-process
    0.0297    5.1%       2   scene / overlay-lines
    0.0079    1.4%       2   clear-targets
    0.0031    0.5%     300   scene / editor-icons
    0.0031    0.5%      14   imgui / imgui-render
```

That is the answer: the mesh draw and the light cull are 83% of GPU time.

### Or use the GUI

To poke around by hand instead — see the actual textures, the shaders, the
pipeline state at each draw:

```bash
qrenderdoc captures-rdc/assisi_frame402.rdc
```

The Event Browser on the left is the marker tree. This is the better tool when
you want to know *what a pass is doing*, as opposed to what it costs.

---

## Reading the results correctly

### The times do not add up to the frame, and that is expected

The report says `total per-event GPU: 0.59 ms`. Chiara will say
`frame/gpu-ms: ~2.3`. **Neither is wrong.**

RenderDoc measures each draw *in isolation* by replaying it. The time between
draws — pipeline barriers, image layout transitions, the GPU draining one pass
before starting the next — belongs to no single draw and is not counted.

So:
- Use **RenderDoc** to rank passes against each other.
- Use **Chiara's `frame/gpu-ms`** for the real total.

Never conclude "the GPU is 75% idle" from the gap. It isn't.

### Debug builds lie

Measured on the same scene, same machine:

| | Debug (`gd-c`) | Ship (`gs-c`) |
|---|---|---|
| `propagate-transforms` | 0.641 ms | 0.017 ms |
| whole `scene` | 1.003 ms | 0.060 ms |

37× on one scope. Debug builds inline nothing, and the code that suffers most is
exactly the math-heavy code you are most likely to be profiling — so a debug
profile does not just scale everything up, it **reorders** what looks expensive.
Always profile `gs-c`.

### Check whether you are even GPU-bound first

Before optimizing a pass, dump a Chiara capture (F9) and look at these:

```
frame/cpu-ms        how long the CPU worked
frame/gpu-ms        how long the GPU worked
frame/sleep-ms      how long the frame limiter idled
frame/gpu-wait-ms   how long the CPU blocked on the GPU / vsync
```

If `sleep-ms` or `gpu-wait-ms` dominates, you are capped by the frame limiter or
vsync and making a pass faster will change nothing. Turning vsync off is not
enough on its own — the engine's own frame limiter takes over. Both have to be
off before you can see the true ceiling.

---

## Gotchas, and why they happen

### RenderDoc cannot capture Wayland

**Symptom:**

```
GLFW error 0x00010006: Vulkan: Window surface creation extensions not found
VulkanContext: glfwCreateWindowSurface failed.
Failed to initialize render system.
```

**Cause:** RenderDoc's Vulkan layer filters the extension list to what it
supports, and it does not support `VK_KHR_wayland_surface`. Run
`VK_INSTANCE_LAYERS=VK_LAYER_RENDERDOC_Capture vulkaninfo` and you will see it
say so outright:

```
ERROR: [RDOC] RenderDoc does not support requested instance extension:
       VK_KHR_wayland_surface
```

**Fix:** run the app on X11 instead. Your desktop is Wayland, but XWayland is
already running, so this costs nothing — it is the same screen.

### Unsetting `WAYLAND_DISPLAY` does not work

The obvious fix is `env -u WAYLAND_DISPLAY`. It fails, with the identical error.

**Cause:** libwayland falls back to the default socket name `wayland-0` when the
variable is missing. Removing it does not stop the connection from succeeding.

**Fix:** point it at a socket that does not exist, so the connection genuinely
fails and GLFW falls back to X11:

```bash
WAYLAND_DISPLAY=nonexistent-sock XDG_SESSION_TYPE=x11 DISPLAY=:1
```

`scripts/rdc-capture.sh` does this for you.

### Markers compile in but do nothing

**Symptom:** the report says `1 marker groups` and everything is `(unmarked)`,
even though the build has `ASSISI_ENABLE_GPU_MARKERS=ON`.

This one has two independent causes and both had to be fixed (2026-07-31). They
are recorded here because each is silent — no warning, no error, the markers
simply have no effect.

1. **`VK_EXT_debug_utils` was only enabled in debug builds.** It was bundled with
   the Khronos validation layer under `#ifndef NDEBUG` in
   `VulkanContext::CreateInstance`. But the extension carries two unrelated
   things: the validation messenger (expensive, debug-only) and command-buffer
   labels (free, and what RenderDoc reads). An optimized build — the only build
   worth profiling — therefore had no labels. They are separated now.

2. **nvrhi was never told which instance extensions were enabled.**
   `nvrhi::vulkan::DeviceDesc::instanceExtensions` was left unset. nvrhi does not
   query Vulkan for this; it believes the list you hand it, and
   `CommandList::beginMarker` checks `extensions.EXT_debug_utils` and returns
   silently when unset.

If markers ever stop appearing, check those two places first.

### qrenderdoc opens a window and hangs

**Cause:** `--python` runs your script *before* the main UI, and qrenderdoc then
opens the UI regardless. In a terminal or a script that looks like a hang.

**Fix:** the script must kill its own process when done. `scripts/rdc-analyze.py`
ends with `os._exit(0)` for exactly this reason. Do not remove it.

### `import renderdoc` fails in normal Python

Arch's `renderdoc` package ships no importable Python module — only the `.so` the
GUI embeds. So `python3 scripts/rdc-analyze.py` cannot work. The
`qrenderdoc --python` route is the only way in on Arch.

### Arguments after `--` are ignored

qrenderdoc hands its command line to Qt first, and Qt silently drops what it does
not recognise. `qrenderdoc --python script.py -- capture.rdc` loses the capture
path with no error. This is why `rdc-analyze.py` takes its input through the
`RDC_CAPTURE` environment variable instead.

---

## Adding markers to new code

If you add a render pass, mark it:

```cpp
#include <Assisi/Render/GpuMarker.hpp>

{
    ASSISI_PROFILE_GPU_SCOPE(frame.commandList, "my-pass");
    // ... record commands ...
}
```

One macro, one name, both timelines — a Chiara CPU scope and a RenderDoc label
that are guaranteed to agree. Two rules:

- **Only wrap code that records GPU commands.** A marker around CPU-only work
  (transform propagation, building cull tables) produces an empty range that
  reads as a pass costing nothing. Use plain `ASSISI_PROFILE_SCOPE` there.
- **Never span the command list's open or close.** `begin-frame` and `end-frame`
  keep plain CPU scopes because the list is not open for the whole of either.

See `modules/Render/include/Assisi/Render/GpuMarker.hpp`.

---

## Automating captures (no keyboard)

F12 needs a human. For CI, or for an agent that cannot press keys, trigger the
capture from inside the app with RenderDoc's in-application API — header-only,
`dlopen`'d, no link dependency:

```cpp
#include <dlfcn.h>
#include <renderdoc_app.h>

if (void *mod = dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD))
{
    auto getApi = reinterpret_cast<pRENDERDOC_GetAPI>(dlsym(mod, "RENDERDOC_GetAPI"));
    RENDERDOC_API_1_6_0 *api = nullptr;
    if (getApi && getApi(eRENDERDOC_API_Version_1_6_0, reinterpret_cast<void **>(&api)) == 1)
        api->TriggerCapture();   // captures the next frame
}
```

`RTLD_NOLOAD` means this is a no-op unless the app is already running under
RenderDoc, so it is safe to leave in. This is not currently wired into the
engine — add a CLI flag if it becomes routine.

---

## Related

- `scripts/rdc-capture.sh` — launches the sandbox under RenderDoc
- `scripts/rdc-analyze.py` — per-pass GPU report from a `.rdc`
- `scripts/chiara-analyze.py` — per-frame CPU report from a Chiara `.json`
- `docs/chiara-design-notes.md` — the CPU profiler's design, and §11 on
  complementary tools (`samply`, `heaptrack`, Nsight)

## Going deeper than RenderDoc

RenderDoc tells you *what* is slow. When you need to know *why* — occupancy,
memory bandwidth, warp stalls — the next tool is **NVIDIA Nsight Graphics**. It
is much heavier, and its Linux feature coverage has historically trailed Windows,
so only reach for it once RenderDoc has named the pass you care about.
