# Assisi Engine

<p align="center">
  <img src="atom-frames/atom-spinner-v4-60.webp" alt="Assisi atom spinner" width="120">
</p>

Assisi is a modern C++ game engine for Windows and Linux focused on building performant, modular systems for
real-time games. This project is primarily for my own education and for my own
needs first. Suggestions, PRs, forks, and bug reports are welcome. Use of this
codebase or engine in your own projects or forks is 100% welcome and free of charge if in compliance
with the license. I kindly ask that you credit the engine and myself
(Francisco Vivas Puerto aka "DaFrancc") in both the repo and any games you make with this engine. Credit is not required, but it would be greatly appreciated.

# Getting Started
## 1. Clone the repo.
```bash
git clone https://github.com/DaFrancc/Assisi.git
```

## 2. Install Tools
- [Make](https://www.gnu.org/software/make/)
- [CMake](https://cmake.org/) 3.28+
- [Ninja](https://ninja-build.org/)
- [Python 3](https://www.python.org/) — runs `reflectgen` during the build (standard library only)

The following compilers have been tested:
- Windows — [MSVC](https://visualstudio.microsoft.com/) (Visual Studio 2022+)
- Linux — [GCC](https://gcc.gnu.org/), [Clang](https://clang.llvm.org/)

Every third-party C++ library the engine uses is fetched and built by CMake on first configure — none
of them is a package you install. What your package manager *does* have to provide is the toolchain
above plus a short list of system development packages: OpenSSL, and the Wayland/X11 headers GLFW
builds its two backends against.

### Windows
Install [Visual Studio 2022+](https://visualstudio.microsoft.com/) with the **Desktop development with
C++** workload (which brings MSVC, CMake, and Ninja), plus [Python 3](https://www.python.org/) if you
do not already have it on `PATH`. Build from a *Developer Command Prompt* so the MSVC environment is
set up.

### Linux

#### Arch
```bash
sudo pacman -S --needed base-devel git cmake ninja python \
                        openssl \
                        wayland libxkbcommon \
                        libxcursor libxi libxinerama libxrandr \
                        vulkan-icd-loader
```

Plus a Vulkan driver for your GPU.

AMD:
```bash
sudo pacman -S vulkan-radeon
```

Intel:
```bash
sudo pacman -S vulkan-intel
```

NVIDIA (proprietary; brings its own Vulkan driver, so the Mesa packages above are not what you want):
```bash
sudo pacman -S nvidia nvidia-utils
```

`nvidia` builds against the stock `linux` kernel — on `linux-lts` or any other kernel, install
`nvidia-dkms` in its place.

Optional, only if you want to build with Clang as well as GCC:
```bash
sudo pacman -S clang
```

---

#### Fedora and other RHEL-based distributions (RHEL, Rocky, Alma)
```bash
sudo dnf install gcc-c++ make git cmake ninja-build python3 pkgconf-pkg-config \
                 openssl-devel \
                 wayland-devel libxkbcommon-devel \
                 libXcursor-devel libXi-devel libXinerama-devel libXrandr-devel \
                 vulkan-loader
```

Plus a Vulkan driver for your GPU.

AMD and Intel (Mesa covers both):
```bash
sudo dnf install mesa-vulkan-drivers
```

NVIDIA: the proprietary driver brings its own Vulkan driver, and it comes from RPM Fusion rather than
Fedora's own repositories. Enable those first:
```bash
sudo dnf install https://mirrors.rpmfusion.org/free/fedora/rpmfusion-free-release-$(rpm -E %fedora).noarch.rpm \
                 https://mirrors.rpmfusion.org/nonfree/fedora/rpmfusion-nonfree-release-$(rpm -E %fedora).noarch.rpm
```

Then install the driver:
```bash
sudo dnf install akmod-nvidia xorg-x11-drv-nvidia
```

**Do not reboot yet.** The kernel module is built in the background and takes a few minutes; rebooting
before it finishes leaves you with a broken driver. This prints the driver version once the module is
ready, and nothing until then:
```bash
modinfo -F version nvidia
```

Reboot once it answers.

`mesa-vulkan-drivers` is not what you want on this card — it carries Mesa's open-source drivers, and
`xorg-x11-drv-nvidia` is what supplies the Vulkan driver your GPU actually runs.

Those two URLs are Fedora's. On RHEL and its rebuilds the equivalent repositories are listed at
[rpmfusion.org/Configuration](https://rpmfusion.org/Configuration).

Optional, only if you want to build with Clang as well as GCC:
```bash
sudo dnf install clang
```

---

#### Debian and Ubuntu — untested

Nobody has built the engine on a Debian-based distribution yet. What follows is a translation of the
two lists above, not a tested recipe; corrections are welcome.

It needs **Debian 13 (trixie) or newer**, or **Ubuntu 24.04 or newer**. Debian 12 (bookworm) is too
old at both ends: the engine is C++23 and asks for CMake 3.28+, while bookworm ships GCC 12 and CMake
3.25 — and `std::expected`, which this codebase returns its errors through, arrived in GCC 13.

```bash
sudo apt install build-essential git cmake ninja-build python3 pkg-config \
                 libssl-dev \
                 libwayland-dev libwayland-bin libxkbcommon-dev \
                 libxcursor-dev libxi-dev libxinerama-dev libxrandr-dev \
                 libvulkan1
```

Plus a Vulkan driver for your GPU.

AMD and Intel (Mesa covers both):
```bash
sudo apt install mesa-vulkan-drivers
```

NVIDIA (proprietary; brings its own Vulkan driver).

On Ubuntu, this picks the version that matches your card — use it rather than the Debian command
below:
```bash
sudo ubuntu-drivers install
```

On Debian, the driver lives in the `contrib` and `non-free` components, which are not enabled by
default:
```bash
sudo apt install nvidia-driver
```

Optional, only if you want to build with Clang as well as GCC:
```bash
sudo apt install clang
```

---

#### Other distributions
Install the equivalents of those four package groups — they are the whole list, and the dropdown below
says what each one is for.

<details>
<summary><b>What those packages are for, what CMake fetches for you, and the CPU baseline</b></summary>

| System packages | Why they are needed |
|---|---|
| OpenSSL headers | GameNetworkingSockets' crypto backend on Linux. This is a deliberate, documented exception to the tree's self-contained rule. Not needed at all with `ASSISI_ENABLE_NETWORKING=OFF`. |
| Wayland + libxkbcommon | GLFW builds its Wayland backend by default and requires `wayland-client`, `wayland-cursor`, `wayland-egl`, and `xkbcommon` at configure time. GLFW vendors the protocol XML files, so `wayland-protocols` is *not* required — only `wayland-scanner`, which ships with the Wayland dev package. |
| Xcursor, Xi, Xinerama, Xrandr | GLFW also builds its X11 backend by default; these pull in `libX11` and the Xorg protocol headers. Both backends are selected at runtime, so build both even if you only ever run one. |
| Vulkan loader + GPU driver | **Runtime only.** The engine loads Vulkan dynamically, so no Vulkan SDK is needed to build — but nothing will render without a loader and an ICD. |

Optionally, installing `simdjson` (Arch) or `simdjson-devel` (Fedora) makes fastgltf link the system
copy instead of compiling its own bundled amalgamation. Both work. The system copy trims a little off
the first build but leaves the resulting binary with a runtime dependency on that shared library; pass
`-DCMAKE_DISABLE_FIND_PACKAGE_simdjson=TRUE` to force the self-contained build regardless of what is
installed.

**Everything in the next table is fetched and built automatically by CMake on first configure** —
nothing in it is a package to install, pin, or vendor, on any platform:

| Dependency | What it does |
|---|---|
| [GLFW](https://github.com/glfw/glfw) | Window creation and input |
| [GLM](https://github.com/g-truc/glm) | Vector/matrix/quaternion math |
| [NVRHI](https://github.com/NVIDIA-RTX/NVRHI) | Render hardware interface over Vulkan |
| [glslang](https://github.com/KhronosGroup/glslang) | Compiles GLSL → SPIR-V at build time |
| [Dear ImGui](https://github.com/ocornut/imgui) | Immediate-mode UI for the editor and debug panels |
| [ImPlot](https://github.com/epezent/implot) | Plots and graphs for those panels |
| [ImGuizmo](https://github.com/CedricGuillemet/ImGuizmo) | The editor's move/rotate/scale gizmo |
| [Jolt Physics](https://github.com/jrouwe/JoltPhysics) | Rigid-body simulation |
| [fastgltf](https://github.com/spnda/fastgltf) | glTF mesh/material import |
| [stb](https://github.com/nothings/stb) | Image decoding (PNG, JPEG, …) |
| [libwebp](https://github.com/webmproject/libwebp) | WebP decoding, including animated |
| [FreeType](https://github.com/freetype/freetype) | Font rasterization |
| [nlohmann/json](https://github.com/nlohmann/json) | JSON for configs and level files |
| [doctest](https://github.com/doctest/doctest) | Unit-test framework |
| [GameNetworkingSockets](https://github.com/ValveSoftware/GameNetworkingSockets) | UDP transport for the networking modules — reliability, fragmentation, connection state |
| [protobuf](https://github.com/protocolbuffers/protobuf) | Pulled in by GameNetworkingSockets |
| [Assimp](https://github.com/assimp/assimp) | Multi-format mesh import — **off by default** |

[Assimp](https://github.com/assimp/assimp) is the deferred catch-all import backend (FBX/OBJ/DAE/…) and
stays off (`ASSISI_ENABLE_ASSIMP`) so its heavy build doesn't tax every configure; fastgltf covers the
runtime glTF path today. Networking is on by default but can be turned off with
`ASSISI_ENABLE_NETWORKING=OFF`, which drops GameNetworkingSockets and protobuf — a noticeable chunk of
first-configure time if you are not building a multiplayer game. Rendering is **Vulkan**: a
Vulkan-capable GPU and driver are required at runtime, but no Vulkan SDK is needed to build (the engine
loads Vulkan dynamically).

**Minimum CPU:** x86-64 with **AVX2** (Intel Haswell 2013+ / AMD Zen 2015+, and the FMA/F16C/LZCNT/BMI
extensions that ship alongside it). This is a deliberate baseline — the engine is compiled with these
instruction sets enabled globally for SIMD performance, so binaries will crash with an
illegal-instruction fault on older CPUs.

</details>

## 3. Configure
### Windows (from a Visual Studio Developer Command Prompt):
```bash
make configure-msvc
```
### Linux (identical on every distribution):
```bash
make configure-gcc    # GCC
make configure-clang  # Clang
```
- Note: The first configure will download and build all dependencies. This takes several minutes. Subsequent runs are fast.
### MacOS:
- Currently unsupported, but you are free to fiddle around with it and submit a pull request.

## 3b. (Optional) Create a New App
The setup scripts scaffold a new app under `apps/` and generate a `CMakeUserPresets.json` for it:
### Windows:
```bash
powershell -ExecutionPolicy Bypass -File .\setup.ps1
```
### Linux:
```bash
chmod +x ./setup.sh && ./setup.sh
```

## 4. Configure & Build

### Which build should I use?

There are five build *variants*. They differ in how much the compiler optimizes, how much debug
information is kept, and whether extra runtime checking is compiled in.

| Variant | Speed | Use it when |
|---|---|---|
| **debug** | slowest (10–40×) | Stepping through code in a debugger. Asserts on, nothing optimized. |
| **dev** | fast | **The everyday build.** Optimized, but keeps enough symbols for a usable stack trace. |
| **ship** | fastest | Releasing, and **any performance measurement**. |
| **asan** | ~2–3× slower | Chasing a crash, a memory leak, or corrupted-looking data. |
| **tsan** | ~5–15× slower | Chasing a race — random-looking bugs in threaded code. |

Rules of thumb: **build `dev` day to day**, drop to `debug` only when you need a debugger, and
**never judge performance from a `debug` build** — it does not merely make everything uniformly
slower, it changes *which* code looks slow. Reach for `asan`/`tsan` when something is wrong rather
than routinely; they are diagnostic tools, not a build you live in.

<details>
<summary><b>More detail on each</b></summary>

**debug** — `CMAKE_BUILD_TYPE=Debug`. No optimization at all: every small function is a real function
call, and templated math (GLM, the ECS) suffers worst. This is what makes it 10–40× slower than
`ship`, and why it is misleading to profile: a measured example from this engine had one function take
0.641 ms in `debug` and 0.017 ms in `ship` — a 37× difference concentrated in one place, which
reorders the whole profile. Its value is that the debugger shows you every variable and never
optimizes a line away.

**dev** — `CMAKE_BUILD_TYPE=RelWithDebInfo`. Optimized like a release build but keeps debug info, so
it runs at roughly shipping speed while a crash still gives a readable stack trace. This is the
sensible default for ordinary work: fast enough that the editor feels right, debuggable enough that
you can find out why something broke. Inlining means the debugger will sometimes skip or reorder
lines.

**ship** — `CMAKE_BUILD_TYPE=Release`. Full optimization, no debug info, and fast-math enabled
(`ASSISI_ENABLE_FAST_MATH`, on by default). This is what players run, and therefore the only build
whose performance numbers mean anything. A crash here gives you little to work with, so reproduce in
`dev` before investigating.

**asan** — `debug` plus **AddressSanitizer and UndefinedBehaviorSanitizer**. Instruments every memory
access and turns silent corruption into an immediate, precise report: use-after-free, buffer overrun,
leaks at exit, signed overflow, bad casts. If a bug shows up as "it crashes somewhere unrelated" or
"this value is nonsense", run it here first — it usually names the exact line. Costs ~2–3× runtime and
a lot of memory.

**tsan** — `debug` plus **ThreadSanitizer**, which finds *data races*: two threads touching the same
memory with no synchronization. It reports races even when the run looked fine, which matters because
races usually do look fine right up until they don't. Relevant to the job system, async asset loading,
and physics. Very slow, and **mutually exclusive with asan** — the two cannot be combined, so run them
separately.

**`-chiara` variants** — orthogonal to all of the above. They add the profiler (`ASSISI_ENABLE_CHIARA`)
and GPU debug markers rather than changing optimization, so `gcc-ship-chiara` is a full shipping build
that can also record a capture. That combination is the one to profile with.

On Linux, `scripts/run-sanitized.sh` launches the sandbox under a sanitizer build and captures the
report to a log file, so a diagnostic survives even if the window dies.

</details>

### Building

After initialization, you can build individual presets or use the all-in-one Makefile targets:

```bash
# Configure + build all presets for a toolchain
make msvc    # Windows
make gcc     # Linux (GCC)
make clang   # Linux (Clang)

# Or build a specific preset
make msvc-debug  # (alias: md)
make msvc-dev    # (alias: mv)
make msvc-ship   # (alias: ms)
make gcc-debug   # (alias: gd)
make gcc-dev     # (alias: gv)
make gcc-ship    # (alias: gs)
make clang-debug # (alias: cd)
make clang-dev   # (alias: cv)
make clang-ship  # (alias: cs)

# Sanitizer builds (no short aliases). ASan is available on MSVC too; TSan is
# Linux-only, and the two can never be combined in one build.
make gcc-asan    # AddressSanitizer + UBSan   (also: msvc-asan, clang-asan)
make gcc-tsan    # ThreadSanitizer            (also: clang-tsan)

# Every preset has a `-chiara` variant that compiles the profiler in (see the
# Chiara module below). Suffix the alias with `-c`:
make gcc-ship-chiara  # (alias: gs-c) — the build worth profiling
make gcc-debug-chiara # (alias: gd-c)

# Or use cmake directly
cmake --preset msvc-debug
cmake --build --preset msvc-debug
```

```bash
# Run the sandbox — the reference app. It is a thin consumer of the Editor
# library (a few hundred lines: argument parsing plus two demo systems); the
# editor itself lives in modules/Editor.
./out/build/msvc-debug/apps/sandbox/Assisi-Sandbox.exe
```

## 5. Run the Tests (optional)
The unit tests (doctest) and the `reflectgen` golden-file tests run through CTest presets that
mirror the build presets:

```bash
ctest --preset msvc-debug   # build first, then run all suites
ctest --preset msvc-debug -R ECS   # a single suite
```

# Understanding Assisi's Module Layout
Assisi is organized into several modules, each responsible for a specific aspect of the engine. All modules compile as static libraries under the `Assisi::` CMake namespace. Below is an overview of each module and its responsibilities:

## Core
Core holds the pieces every other module needs. `Logger` writes to the console and to a file.
`AssetSystem` is a small virtual filesystem: a read-only root for the assets the game ships with, a
writable one for per-user files like saves and settings, and `std::expected` returns instead of
exceptions. `EventQueue` is a typed message bus, drained once per frame, that lets systems talk to one
another without knowing each other exist. `JobSystem` is the engine's task pool — worker threads, plus a
main-thread queue for work that has to land at a specific safe point in the frame. `Prelude.hpp` is the
common-includes header.

Core is also where **reflection** lives. Tag a struct with `ACOMP()` and its fields with `AFIELD()`, and
the `reflectgen` build tool (`tools/reflectgen`) reads your header and writes the tedious code for you:
saving and loading the component, drawing it in the editor's inspector, and packing it for the network.
Extra tags adjust that — `ACOMP(replicable)` lets a component travel over the network, `AFIELD(norep)`
keeps one field off the wire (see NetSync). `ComponentRegistry` is the runtime table of everything that
was generated.

## Math
Math is the vectors, matrices and quaternions everything else is written in terms of. Today it is mostly
a thin wrapper around [GLM](https://github.com/g-truc/glm); the wrapper exists so engine-specific
utilities and optimizations can be added later without every call site changing.

## Window
Window creates the operating-system window and reads input from it. It manages GLFW's lifetime
(`GlfwLibrary` is shared, so GLFW is initialised once and shut down when the last user of it goes away),
and exposes `WindowContext` for the window itself and `InputContext` for polling the keyboard and mouse.
`ActionMap` sits on top of that: it maps a name like `"Jump"` to a key or mouse button, loaded from
`game.json`, so game code asks about actions rather than hardcoding keys.

## Geometry
Geometry owns mesh and material *data* with no GPU involved, which is what lets importers and
command-line tools use it without a Vulkan device. It provides `MeshData` / `Vertex` / `SubMesh` /
`LodRange` for the geometry itself, `Bounds` (the bounding spheres and boxes that culling uses to decide
whether something is on screen), `DefaultMeshes` (the built-in cube, sphere and plane), and
`MaterialData` / `MaterialFile` — `.amat`, the on-disk material format. `MeshImporter` and `AssetImport`
load glTF files through fastgltf, splitting a file's materials into separate assets as they import.

## Render
Render draws the scene with **Vulkan**, through NVIDIA's
[NVRHI](https://github.com/NVIDIA-RTX/NVRHI) hardware-abstraction layer. Windows and Linux; you do not
need the Vulkan SDK to build, only a Vulkan-capable driver to run. `RenderSystem` and `VulkanContext`
own the device, the swapchain and the frames-in-flight; `ShaderModule` loads SPIR-V that glslang compiles
from GLSL at build time; `MeshBuffer` / `Buffer` / `Texture` are the GPU resources.

The default renderer is **clustered forward**. The camera's view is diced into a 3D grid of small boxes,
and a compute shader records which lights reach each box (`ClusterGrid`). When `MeshPass` shades a pixel
it looks up the box that pixel falls in and considers only those few lights, instead of looping over
every light in the level — which is what lets a scene hold a lot of them. Shading uses a Cook–Torrance
PBR model, and `PostProcess` adds optional anti-aliasing (MSAA and/or FXAA).

Drawing is **GPU-driven**. Rather than the CPU working out what is visible and issuing one draw call per
object, `MeshCuller` does the visibility test on the GPU and writes the draw commands there too, so an
entire scene goes out as a single `drawIndexedIndirect`. `GeometryArena` is what makes that possible: it
keeps all mesh data in a few shared vertex and index buffers instead of one buffer per mesh. `AssetCache`
turns asset paths into GPU meshes and textures, reusing anything already loaded (including the built-in
`prim://` primitives and the fallback textures) and doing its uploads off the main thread. `OutlinePass`,
`IconPass` and `LinePass` are the editor's overlays, opt-in so a game build never compiles them.
`GpuMarker.hpp` labels command buffers so RenderDoc and Nsight captures are readable; see
[`docs/gpu-profiling-guide.md`](docs/gpu-profiling-guide.md).

## ECS
ECS (Entity-Component-System) is how game objects are represented: an entity is just an ID, its data
lives in components attached to that ID, and systems run over every entity that has a particular set of
components. This module is that machinery — `Scene` (the container it all lives in), `Registry` and
`SparseSet` (the component storage), and `Query`, which walks every entity holding a chosen set of
components and can skip the ones that also hold something else (`Without<>`). Entity handles carry a
generation counter, so a handle to an entity that has since been destroyed is caught as stale instead of
quietly pointing at whatever reused its slot.

## Runtime
Runtime is the ready-made components and systems most games need, built on ECS. The components are
`Transform` (position, rotation and scale, with parenting through `Parent` and `PropagateTransforms`),
`MeshRenderer`, `Camera`, and point/spot/directional lights. The systems are `SceneRenderer`, the default
render path, which ties the camera, the clustered `LightingSystem` and mesh drawing together;
`SceneSerializer`, which saves and loads `.alvl` level files using reflection, so a new component is
serialized without anyone writing serialization code for it; `AssetResolve`, which turns the asset
references stored on components into loaded assets; and `Lifecycle`, whose `DestroyTag` / `DestroyMarked`
pair defers entity destruction to the end of the frame so nothing is deleted out from under a system
that is still iterating.

## Physics
Physics wraps [Jolt Physics](https://github.com/jrouwe/JoltPhysics) for rigid-body simulation.
`PhysicsWorld` owns the simulation — creating bodies, stepping it forward, gravity. A simulated entity
carries two components: `RigidBodyDescriptor`, the saved description of the body (its shape, and whether
it is static or dynamic), and `RigidBody`, the live handle to Jolt's copy of that body, which is created
on load and never written to disk. Call `PhysicsWorld::Clear()` before loading a new level, or the
previous level's bodies stay in the simulation.

## Net
Net moves bytes between two machines and has no idea what they mean — it knows nothing about entities or
components. It wraps [GameNetworkingSockets](https://github.com/ValveSoftware/GameNetworkingSockets)
behind `NetTransport`, which opens and closes connections and sends each message either **reliable**
(resent until it arrives, delivered in order) or **unreliable** (sent once; if it drops, it is gone).
Messages travel on separate *lanes*, so one big slow transfer cannot hold up small urgent ones queued
behind it.

`CreateLoopbackPair()` hands back both ends of a connection living inside a single process. That is how
the replication tests run: the real code path, without a network and without a test-only fake.

## NetSync
NetSync is the layer that keeps two machines' scenes looking the same. Net moves the bytes; NetSync
decides which bytes to move.

**Keeping the two in sync.** The straightforward approach is for the server to send everyone's positions
every frame and for clients to draw exactly what they are told — but then every client is always
rendering a slightly stale world. Assisi instead has *every* machine run the same physics simulation at
60 Hz, so a client can work out most of what happens on its own. The server sends corrections when a
client has drifted away from the truth. The client is usually right by itself; the server's job is to
keep it honest.

**Sending only what changed.** The server remembers the last snapshot each client confirmed receiving,
and sends only the difference against that confirmed state. Nothing is ever retransmitted: if a packet is
lost, the next snapshot is still measured against the same confirmed baseline, so it already contains
whatever went missing. Packet loss costs a little extra bandwidth instead of putting the two machines
out of sync.

The main pieces are `ReplicationServer` and `ReplicationClient`, the two halves of the conversation;
`BodyState`, which carries physics corrections in compressed form (rotations as three numbers instead of
four, positions to roughly 2 mm) and eases the result in visually so a correction does not look like a
teleport; and `NetClock`, which keeps the client's idea of "which tick is it right now" close to the
server's. When two machines connect they first compare a **protocol hash** — a fingerprint of how every
replicated component is laid out — and refuse to connect if it differs, rather than decoding each other's
bytes into the wrong fields.

**Deciding what replicates.** Sending an entire scene would be wasteful, so five separate things all have
to agree before a given component is put on the wire:

1. **The component type must be allowed to travel.** Only types declared `ACOMP(replicable)` are ever
   eligible.
2. **The game can veto a type outright.** `networking.neverReplicate` in `game.json` blocks a component
   for that game, even one an engine module marked replicable.
3. **The entity has to opt in.** Only entities carrying the `Replicated` component are sent at all.
4. **That entity can drop individual components.** `Replicated::excluded` is a per-entity list of
   opt-outs, so one crate can send its transform but keep, say, its audio state to itself.
5. **Individual fields can be held back.** `AFIELD(norep)` marks a field as saved to disk but never sent
   over the network.

Steps 1 and 2 are deliberately separate decisions made by separate people. An engine module can say a
component *is able to* replicate, but only the game says whether it actually *does* — so marking
something in an engine header never quietly adds it to every game's bandwidth bill. Steps 3 and 4 are
what you edit in the editor: each component header gets a clickable glyph, and the `Replicated` component
shows the same choices as a checklist. Design notes and the reasoning behind all of this are in
[`docs/replication-optin-plan-v1.md`](docs/replication-optin-plan-v1.md) and
[`docs/replication-plan-v4.md`](docs/replication-plan-v4.md).

## Debug
Debug is the developer UI: [Dear ImGui](https://github.com/ocornut/imgui) and
[ImPlot](https://github.com/epezent/implot) wired up to a GLFW + **Vulkan** backend.
`DebugUI::Initialize(window, vulkanContext)` has to be called after the Vulkan context exists. Override
`OnImGui()` in your application class to draw your own panels and overlays.

## App
App is the framework that ties the lower modules together. `Application` is the base class you derive
from: it runs physics on a fixed timestep (60 Hz by default, `AppConfig::physicsHz`) and paces rendering
either to the display's refresh rate or to an optional FPS cap, switchable at runtime. You override
`OnStart`, `OnFixedUpdate(dt)`, `OnUpdate(dt)` and `OnRender(RenderFrame&)`, plus `OnImGui()`,
`OnShutdown()`, `OnResize()` and `OnRenderTargetsChanged()` if you need them.

`SystemRegistry` is where systems are registered. Each one goes into a phase (`PreUpdate`, `FixedUpdate`,
`Update`, `PostUpdate`) and can declare that it runs `After()` or `Before()` a named other system; the
registry sorts them accordingly, so ordering is stated deliberately instead of being an accident of
registration order. Render systems register separately, through `RegisterRender`.

`World` and `WorldManager` let **several scenes be loaded at the same time** — for streaming, or for
keeping a menu alive behind a level. Each world owns its own scene, its own physics world and its own
instances of the registered systems, and is in one of three states (`Loading`, `Active`, `Dormant`).
Which systems a world runs comes from a named **profile**, so a level chooses its systems rather than
every level being stuck with one global set. `Application` also owns the `Core::JobSystem`
(`Jobs().Run()` / `RunOnMain()`); its main-thread queue is drained at a fixed point each frame, which is
where background asset loads hand their results back.

Configuration comes in two halves. `AppConfig`, read from `assets/game.json`, is what the game ships
with: window setup, clear colour, physics rate. `OptionsConfig`, saved to `options.json`, is what the
player changes: anti-aliasing mode, MSAA sample count, VSync and FPS limit, all editable in-app through
the **F11** options window.

## Editor
The Editor is the level editor, built **as a library** instead of as an executable, so a game and its
editor come out as two thin targets over the same code. `EditorApp` derives from `App::Application` and
adds the fly camera, entity list, inspector (generated from reflection, so your own components show up in
it without any work), transform gizmo, asset browser, collider wireframes, play/pause/stop, and `.alvl`
level loading and saving. `EditHistory` is undo/redo (Ctrl-Z): every edit site records into it, and the
history survives entering and leaving play mode. Play can start as a host or as a client, and
**Host + N play-in-editor clients** launch as child processes from a single button, so testing a
networked session does not mean starting binaries by hand. `apps/sandbox` is a few hundred lines on top
of all this. The editor's overlays are opt-in at the renderer level (`enableEditorVisuals`), so a shipped
game never creates those pipelines or loads editor assets.

## Chiara
Chiara is the performance and memory analyzer — a frame profiler that is always on whenever it is
compiled in. `ASSISI_PROFILE_SCOPE` and `ASSISI_PROFILE_COUNTER` record named scopes and counters into a
lock-free ring buffer per thread, which is exported as Chrome Trace JSON and opens in
[Perfetto](https://ui.perfetto.dev). A scope costs about 18 ns; in a build without
`ASSISI_ENABLE_CHIARA` the macros compile to nothing at all, so instrumentation can stay in the code
permanently rather than rotting. Press **F9** in the sandbox for the capture panel — snapshot the recent
past out of the ring, or stream a longer session straight to disk.

Chiara has no dependencies of its own, which is what lets `Core` (and therefore everything else) sit
above it. Design notes are in [`docs/chiara-design-notes.md`](docs/chiara-design-notes.md), and
[`scripts/chiara-analyze.py`](scripts/chiara-analyze.py) reads a capture from the terminal when you want
numbers rather than a flame chart.

# Documentation
Per-module overviews are in the section above, and the public API is documented with Doxygen-style
comments in the headers. Design notes and the rolling code-review docket live in [`docs/`](docs/) —
including the architecture notes for [clustered lighting](docs/light-culling-design-notes.md),
[GPU-driven rendering](docs/gpu-driven-rendering-design-notes.md),
[multi-scene worlds](docs/multi-scene-design-notes.md), the [job system](docs/job-system-design-notes.md),
[asset streaming](docs/asset-streaming-design-notes.md), [replication](docs/replication-plan-v4.md) and
its [opt-in model](docs/replication-optin-plan-v1.md), and [Chiara](docs/chiara-design-notes.md).

The replication docs come with [a survey of how other ECS engines solve the same
problem](docs/replication-research-ecs-survey.md) — Unity NetCode, bevy_replicon, lightyear, Flecs,
EnTT, Unreal (classic/Iris/Mass), Photon Quantum, Overwatch, SpatialOS and Godot — with a source beside
every claim and the unverifiable ones marked as such. It is the evidence the design argues from,
including the two places it deliberately departs from the majority.

Two practical guides:
- [`docs/gpu-profiling-guide.md`](docs/gpu-profiling-guide.md) — capturing and reading a frame with
  RenderDoc, written for someone who has never used a GPU profiler. Includes the Wayland/NVIDIA
  pitfalls, which are not guessable.
- [`docs/remaining-work.md`](docs/remaining-work.md) — the running list of known gaps.

# Useful Links
- [Issue Tracker](https://github.com/DaFrancc/Assisi/issues)

# AI Notice
This project uses AI to help develop this project for the main purpose of education alongside some code generation, documentation,
bug spotting, bug fixing, and temporary art creation (i.e. placeholders for the sake of development, but never to end
up in full releases).

You are free to commit code that was written with the use of AI, and it will be reviewed to the same standard as code
that was not written with AI. You do not need to disclose exactly which ideas, lines, or commits were aided with the
use of AI, but you are free to do so. The only exception is that you must clearly disclose any art assets that
were created with AI such as: textures, 3D models, photos, videos, audio, and any other form of creative media that
goes into this engine.

This notice only applies to this repo. Any forks of this repo or software made using this engine do not need to follow
these guidelines regarding the use of AI.
