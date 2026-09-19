# Assisi Engine

<p align="center">
  <img src="atom-frames/atom-spinner-v4-60.webp" alt="Assisi atom spinner" width="120">
</p>

Assisi is a modern C++ game engine for Windows and Linux, focused on performant, modular systems for
real-time games. It's free to use in your own projects under the [license](LICENSE.txt).

**New here? Read [The Assisi Book](https://dafrancc.github.io/Assisi/)**, a step-by-step guide to making
a game with the engine.

# Quick start

**You need:** Windows or Linux, a Vulkan-capable GPU, a C++ compiler (MSVC 2022+, GCC or Clang), CMake
3.28+, Ninja, Make, ccache and Python 3. On Linux, podman or docker as well if you want release builds
that run on other distributions (optional; see *Building for every Linux distribution* below).

<details>
<summary><b>Installing those on Windows</b></summary>

Install [Visual Studio 2022+](https://visualstudio.microsoft.com/) with the **Desktop development with
C++** workload (which brings MSVC, CMake, and Ninja), plus [Python 3](https://www.python.org/) if you
do not already have it on `PATH`, [Make](https://www.gnu.org/software/make/), and
[ccache](https://ccache.dev/) on `PATH`. Build from a
*Developer Command Prompt* so the MSVC environment is set up.

</details>

<details>
<summary><b>Installing those on Linux</b></summary>

<details>
<summary><b>Arch</b></summary>

```bash
sudo pacman -S --needed base-devel git cmake ninja ccache python \
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

</details>

<details>
<summary><b>Fedora and other RHEL-based distributions (RHEL, Rocky, Alma)</b></summary>

```bash
sudo dnf install gcc-c++ make git cmake ninja-build ccache python3 pkgconf-pkg-config \
                 libstdc++-static \
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

</details>

<details>
<summary><b>Debian and Ubuntu — untested</b></summary>

Nobody has built the engine on a Debian-based distribution yet. What follows is a translation of the
two lists above, not a tested recipe; corrections are welcome.

It needs **Debian 13 (trixie) or newer**, or **Ubuntu 24.04 or newer**. Debian 12 (bookworm) is too
old at both ends: the engine is C++23 and asks for CMake 3.28+, while bookworm ships GCC 12 and CMake
3.25 — and `std::expected`, which this codebase returns its errors through, arrived in GCC 13.

```bash
sudo apt install build-essential git cmake ninja-build ccache python3 pkg-config \
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

</details>

<details>
<summary><b>Other distributions, and what each package is for</b></summary>

Install the equivalents of the package groups below — they are the whole list.

| System packages | Why they are needed |
|---|---|
| Static C++ runtime (`libstdc++.a`) | The game links the C++ runtime into itself, so a player needs no `libstdc++` of their own. Fedora packages it separately as `libstdc++-static`; Arch's `gcc` and Debian's `build-essential` already include it. Without it a Release configure stops and names the package, and other builds link the runtime shared. |
| Wayland + libxkbcommon | GLFW builds its Wayland backend by default and requires `wayland-client`, `wayland-cursor`, `wayland-egl`, and `xkbcommon` at configure time. GLFW vendors the protocol XML files, so `wayland-protocols` is *not* required — only `wayland-scanner`, which ships with the Wayland dev package. |
| Xcursor, Xi, Xinerama, Xrandr | GLFW also builds its X11 backend by default; these pull in `libX11` and the Xorg protocol headers. Both backends are selected at runtime, so build both even if you only ever run one. |
| Vulkan loader + GPU driver | **Runtime only.** The engine loads Vulkan dynamically, so no Vulkan SDK is needed to build — but nothing will render without a loader and an ICD. |
| podman or docker | **Optional**, only for the Steam Runtime build (`make gs-steamrt…`), which builds release games that run on any distro with glibc 2.31+. Nothing else uses it. |

Optionally, installing `simdjson` (Arch) or `simdjson-devel` (Fedora) makes fastgltf link the system
copy instead of compiling its own bundled amalgamation. Both work. The system copy trims a little off
the first build but leaves the resulting binary with a runtime dependency on that shared library; pass
`-DCMAKE_DISABLE_FIND_PACKAGE_simdjson=TRUE` to force the self-contained build regardless of what is
installed.

</details>

</details>

Then clone, configure once, build, and run the editor:

```bash
git clone https://github.com/DaFrancc/Assisi.git
cd Assisi

make configure-gcc    # Linux (or configure-clang); Windows: make configure-msvc
make gv               # build the dev configuration;  Windows: make mv

./out/build/gcc-dev/apps/game/Assisi-GameEditor -l levels/Test.alvl
# Windows: .\out\build\msvc-dev\apps\game\Assisi-GameEditor.exe -l levels/Test.alvl
```

The first configure downloads and builds every dependency, which takes several minutes. It happens once.

**Your game's code goes in `apps/game/src/`**, and its content (levels, models, settings) in `assets/`.
Any file you add under `src/` is built automatically, with nothing to register.
[The book](https://dafrancc.github.io/Assisi/) walks through writing your first system.

<details>
<summary><b>Build variants and every make target</b></summary>

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

On Linux, `scripts/run-sanitized.sh` launches the editor under a sanitizer build and captures the
report to a log file, so a diagnostic survives even if the window dies.

The Makefile targets (`make help` prints the full list):

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

# Steps after a build: the player Game executable, a cook of assets/, and a pak.
make gdg     # gcc-debug-game
make gdgkp   # gcc-debug-game-cook-pack

# Optional, Linux: gcc-ship built inside Valve's Steam Runtime SDK container, for
# a game that runs on any distro with glibc 2.31+ (see "Building for every Linux
# distribution" below).
make gs-steamrt-game-cook-pack
make gs-steamrt-test   # the game tests in the SDK, then a boot on a bare Debian 11

# Or use cmake directly
cmake --preset msvc-debug
cmake --build --preset msvc-debug
```

The download of dependency sources happens once for the whole tree, not once per build directory: they
are cloned into `out/_deps-src` and every preset is pointed at them. `make clean` keeps that cache;
`make clean-deps` deletes it, which is also what makes a bumped dependency pin take effect.

`Assisi-GameEditor` is the game with the editor linked in. `Assisi-Game` is the same project with no
editor in the link — what a player would run. It is out of the default build; build it with the `-game`
targets above.

</details>

<details>
<summary><b>Running the tests</b></summary>

The unit tests (doctest) and the `reflectgen` golden-file tests run through CTest presets that
mirror the build presets:

```bash
ctest --preset gcc-dev        # build first, then run all suites
ctest --preset gcc-dev -R ECS # a single suite
```

</details>

<details>
<summary><b>Building for every Linux distribution (the Steam Runtime build)</b></summary>

A Linux program runs on the glibc it was built against or newer, never older. A game built bare on a
current distro (`make gsgkp`) therefore refuses to start on Ubuntu LTS, Debian stable, or anything else
older than the build machine, and running it through Steam does not change that: Steam uses the host's
glibc whenever it is newer than its runtime's.

For a release, build inside Valve's Steam Linux Runtime 3 ("sniper") SDK instead. It is Debian 11 with
glibc 2.31 and GCC 14, the environment Steam runs native Linux games in, and the game it builds starts
on any x86-64 distro with glibc 2.31 or newer (Ubuntu 20.04+, Debian 11+, Fedora 32+, SteamOS 3, …).
The container is only the build machine: the result is an ordinary executable, and players need
nothing extra.

| | Bare | Steam Runtime |
|---|---|---|
| Command | `make gsgkp` | `make gs-steamrt-game-cook-pack` |
| Runs on | the build machine's glibc or newer | glibc 2.31 or newer |
| Needs | the packages above | the packages above, plus podman or docker |
| Disk | — | about 5 GB (4.3 GB of images, ~1 GB build tree and ccache) |
| Output | `out/build/gcc-ship/apps/game/` | `out/build/gcc-ship-steamrt/apps/game/` |

**It is optional.** No other target, preset or test starts a container or needs podman or docker.

**Dependencies.** podman (preferred, rootless) or docker, usable as your own user without `sudo`:

```bash
sudo pacman -S podman              # Arch
sudo dnf install podman            # Fedora and RHEL-based (Fedora Workstation has it already)
sudo apt install podman uidmap     # Debian and Ubuntu
```

With docker, your user must be in the `docker` group and the daemon running. Everything else — the
compiler, a CMake new enough for the engine, Ninja, Python, ccache, and GLFW's headers — comes from the
SDK image, and the build shares `out/_deps-src` with every other preset.

**Targets:**

```bash
make gs-steamrt-game-cook-pack  # build, cook and pack; any step combination works, e.g. gs-steamrt-cook-pack
make gs-steamrt                 # build only
make gs-steamrt-test            # the game tests in the SDK, then a headless boot on a bare Debian 11
make steamrt-fetch              # download and prepare the images without building
make steamrt-remove             # delete the images and the container's ccache
make clean-gcc-ship-steamrt     # delete the build tree
```

The first run downloads the SDK (about 3.9 GB, pinned by digest) and builds a small image on top of it
from `scripts/steamrt/Containerfile` that adds a newer CMake. Later runs reuse both, and the build is
incremental like any other: the build tree and the ccache (`out/steamrt-home/`) stay on the host. The
container runs as your own user with the repository mounted at its own path, so nothing it writes is
owned by root.

Inside the container the build is the `gcc-ship-steamrt` preset, which can only be configured there.
It sets a glibc ceiling of 2.31 that `GameNeedsOnlySystemLibraries` enforces on the built game.
`scripts/steamrt.py` does the container work; [the book](https://dafrancc.github.io/Assisi/) has a
chapter on it, including troubleshooting.

</details>

<details>
<summary><b>Dependencies and the CPU baseline</b></summary>

**Everything in this table is fetched and built automatically by CMake on first configure** —
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
| [libsodium](https://github.com/jedisct1/libsodium) | GameNetworkingSockets' encryption on Linux (Windows uses the OS's own) |
| [LZ4](https://github.com/lz4/lz4) | Fast pak compression |
| [Zstandard](https://github.com/facebook/zstd) | Smaller pak compression |
| [Assimp](https://github.com/assimp/assimp) | Multi-format mesh import — **off by default** |

[Assimp](https://github.com/assimp/assimp) is the deferred catch-all import backend (FBX/OBJ/DAE/…) and
stays off (`ASSISI_ENABLE_ASSIMP`) so its heavy build doesn't tax every configure; fastgltf covers the
runtime glTF path today. Networking is on by default but can be turned off with
`ASSISI_ENABLE_NETWORKING=OFF`, which drops GameNetworkingSockets, protobuf and libsodium — a noticeable chunk of
first-configure time if you are not building a multiplayer game. Rendering is **Vulkan**: a
Vulkan-capable GPU and driver are required at runtime, but no Vulkan SDK is needed to build (the engine
loads Vulkan dynamically).

**Minimum CPU:** x86-64 with **AVX2** (Intel Haswell 2013+ / AMD Zen 2017+, and the FMA/F16C/LZCNT/BMI
extensions that ship alongside it). This is a deliberate baseline — the engine is compiled with these
instruction sets enabled globally for SIMD performance, so binaries will crash with an
illegal-instruction fault on older CPUs.

**macOS** is currently unsupported, but you are free to fiddle around with it and submit a pull request.

</details>

<details>
<summary><b>Module overview</b></summary>

Assisi is organized into several modules, each responsible for a specific aspect of the engine. All
modules compile as static libraries under the `Assisi::` CMake namespace.

### Core
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

### Math
Math is the vectors, matrices and quaternions everything else is written in terms of. Today it is mostly
a thin wrapper around [GLM](https://github.com/g-truc/glm); the wrapper exists so engine-specific
utilities and optimizations can be added later without every call site changing.

### Window
Window creates the operating-system window and reads input from it. It manages GLFW's lifetime
(`GlfwLibrary` is shared, so GLFW is initialised once and shut down when the last user of it goes away),
and exposes `WindowContext` for the window itself and `InputContext` for polling the keyboard and mouse.
`ActionMap` sits on top of that: it maps a name like `"Jump"` to a key or mouse button, loaded from
`assets/config/input.json` and then overlaid with whatever the player rebound, so game code asks about
actions rather than hardcoding keys.

### Geometry
Geometry owns mesh and material *data* with no GPU involved, which is what lets importers and
command-line tools use it without a Vulkan device. It provides `MeshData` / `Vertex` / `SubMesh` /
`LodRange` for the geometry itself, `Bounds` (the bounding spheres and boxes that culling uses to decide
whether something is on screen), `DefaultMeshes` (the built-in cube, sphere and plane), and
`MaterialData` / `MaterialFile` — `.amat`, the on-disk material format. `MeshImporter` and `AssetImport`
load glTF files through fastgltf, splitting a file's materials into separate assets as they import.

### Render
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
`GpuMarker.hpp` labels command buffers so RenderDoc and Nsight captures are readable.

### ECS
ECS (Entity-Component-System) is how game objects are represented: an entity is just an ID, its data
lives in components attached to that ID, and systems run over every entity that has a particular set of
components. This module is that machinery — `Scene` (the container it all lives in), `Registry` and
`SparseSet` (the component storage), and `Query`, which walks every entity holding a chosen set of
components and can skip the ones that also hold something else (`Without<>`). Entity handles carry a
generation counter, so a handle to an entity that has since been destroyed is caught as stale instead of
quietly pointing at whatever reused its slot.

### Runtime
Runtime is the ready-made components and systems most games need, built on ECS. The components are
`Transform` (position, rotation and scale, with parenting through `Parent` and `PropagateTransforms`),
`MeshRenderer`, `Camera`, and point/spot/directional lights. The systems are `SceneRenderer`, the default
render path, which ties the camera, the clustered `LightingSystem` and mesh drawing together;
`SceneSerializer`, which saves and loads `.alvl` level files using reflection, so a new component is
serialized without anyone writing serialization code for it; `AssetResolve`, which turns the asset
references stored on components into loaded assets; and `Lifecycle`, whose `DestroyTag` / `DestroyMarked`
pair defers entity destruction to the end of the frame so nothing is deleted out from under a system
that is still iterating.

### Physics
Physics wraps [Jolt Physics](https://github.com/jrouwe/JoltPhysics) for rigid-body simulation.
`PhysicsWorld` owns the simulation — creating bodies, stepping it forward, gravity. A simulated entity
carries two components: `RigidBodyDescriptor`, the saved description of the body (its shape, and whether
it is static or dynamic), and `RigidBody`, the live handle to Jolt's copy of that body, which is created
on load and never written to disk. Call `PhysicsWorld::Clear()` before loading a new level, or the
previous level's bodies stay in the simulation.

### Net
Net moves bytes between two machines and has no idea what they mean — it knows nothing about entities or
components. It wraps [GameNetworkingSockets](https://github.com/ValveSoftware/GameNetworkingSockets)
behind `NetTransport`, which opens and closes connections and sends each message either **reliable**
(resent until it arrives, delivered in order) or **unreliable** (sent once; if it drops, it is gone).
Messages travel on separate *lanes*, so one big slow transfer cannot hold up small urgent ones queued
behind it.

`CreateLoopbackPair()` hands back both ends of a connection living inside a single process. That is how
the replication tests run: the real code path, without a network and without a test-only fake.

### NetSync
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
2. **The game can veto a type outright.** `neverReplicate` in `assets/config/network.json` blocks a
   component for that game, even one an engine module marked replicable.
3. **The entity has to opt in.** Only entities carrying the `Replicated` component are sent at all.
4. **That entity can drop individual components.** `Replicated::excluded` is a per-entity list of
   opt-outs, so one crate can send its transform but keep, say, its audio state to itself.
5. **Individual fields can be held back.** `AFIELD(norep)` marks a field as saved to disk but never sent
   over the network.

Steps 1 and 2 are deliberately separate decisions made by separate people. An engine module can say a
component *is able to* replicate, but only the game says whether it actually *does* — so marking
something in an engine header never quietly adds it to every game's bandwidth bill. Steps 3 and 4 are
what you edit in the editor: each component header gets a clickable glyph, and the `Replicated` component
shows the same choices as a checklist.

### Debug
Debug is the developer UI: [Dear ImGui](https://github.com/ocornut/imgui) and
[ImPlot](https://github.com/epezent/implot) wired up to a GLFW + **Vulkan** backend.
`DebugUI::Initialize(window, vulkanContext)` has to be called after the Vulkan context exists. Override
`OnImGui()` in your application class to draw your own panels and overlays.

### Mondrian
Mondrian is the game UI, the one a player sees: menus, HUD and, later, in-world screens. It is retained
mode and lives outside the ECS. Every windowed `Application` owns one and drives it in two steps a
frame: input right after the input poll, and layout plus the draw list right before rendering. Systems
reach it as `ctx.ui`, which is null on a headless server. The core (`Assisi::Mondrian`) links only Core,
so layout, focus and parsing are tested without a window or GPU; the engine layer
(`Assisi::MondrianEngine`) is what draws its output. Text is drawn from signed-distance glyph atlases
the cook rasterises from a `.afont` description with FreeType (`Assisi::FontImport`); the game reads
the cooked atlas and links no FreeType. Dear ImGui stays the editor's and debug UI's.

### App
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

Configuration is split by lifetime, into files under `assets/config/`. `AppConfig`, read from
`config/game.json`, is what the game ships with: window title and size, clear colour, physics rate,
log retention. `NetworkConfig` (`config/network.json`) is owned by NetSync and holds the quantization
both peers must agree on, the correction smoothing, and the replication policy. `InputBindings`
(`config/input.json`) is owned by Window and holds the default action bindings. Each file is a
reflected asset type, parsed exactly once by the module that declares its schema.

`OptionsConfig`, saved to `options.json` under the writable user root, is what the player changes:
anti-aliasing mode, MSAA sample count, VSync and FPS limit — all editable in-app through the **F11**
options window — plus the window size and any rebound actions, which override the shipped defaults
above. It stores only what differs from the default, so a setting nobody touched follows the defaults
as they change.

### Editor
The Editor is the level editor, built **as a library** instead of as an executable, so a game and its
editor come out as two thin targets over the same code. `EditorApp` derives from `App::Application` and
adds the fly camera, entity list, inspector (generated from reflection, so your own components show up in
it without any work), transform gizmo, asset browser, collider wireframes, play/pause/stop, and `.alvl`
level loading and saving. `EditHistory` is undo/redo (Ctrl-Z): every edit site records into it, and the
history survives entering and leaving play mode. Play can start as a host or as a client, and
**Host + N play-in-editor clients** launch as child processes from a single button, so testing a
networked session does not mean starting binaries by hand. `apps/game` is a few hundred lines on top
of all this, built twice: `Assisi-GameEditor` links the editor, and `Assisi-Game` does not. The editor's
overlays are opt-in at the renderer level (`enableEditorVisuals`), so a shipped game never creates those
pipelines or loads editor assets.

### Chiara
Chiara is the performance and memory analyzer — a frame profiler that is always on whenever it is
compiled in. `ASSISI_PROFILE_SCOPE` and `ASSISI_PROFILE_COUNTER` record named scopes and counters into a
lock-free ring buffer per thread, which is exported as Chrome Trace JSON and opens in
[Perfetto](https://ui.perfetto.dev). A scope costs about 18 ns; in a build without
`ASSISI_ENABLE_CHIARA` the macros compile to nothing at all, so instrumentation can stay in the code
permanently rather than rotting. Press **F9** in the editor for the capture panel — snapshot the recent
past out of the ring, or stream a longer session straight to disk.

Chiara has no dependencies of its own, which is what lets `Core` (and therefore everything else) sit
above it. [`scripts/chiara-frame.py`](scripts/chiara-frame.py) reads a capture from the terminal when you
want numbers rather than a flame chart, showing one frame with every scope against its own median over
the whole recording — so a duration reads as steady state or spike.

</details>

<details>
<summary><b>Documentation and contributing</b></summary>

[The Assisi Book](https://dafrancc.github.io/Assisi/) is the guide for people making games with the
engine. Its source is in `book/`: markdown chapters built with [mdBook](https://rust-lang.github.io/mdBook/)
and published automatically when they change on `main`. To preview it locally, run `mdbook serve book`.

Per-module overviews are in the section above, and the public API is documented with Doxygen-style
comments in the headers. Those headers are the reference: a header states what its type guarantees and
why, next to the code that has to keep the promise.

There are no design documents in this repository, deliberately. Plans, design rationale and the running
list of known gaps live in the [issue tracker](https://github.com/DaFrancc/Assisi/issues) instead, where
they are edited by the same act that advances the work. A markdown file in the tree is a copy that stops
being true without anything noticing, which is what happened to the tree this replaced.

Suggestions, PRs, forks, and bug reports are welcome.

</details>

<details>
<summary><b>Credit, third-party assets and AI notice</b></summary>

This project is primarily for my own education and for my own needs first. Use of this codebase or
engine in your own projects or forks is 100% welcome and free of charge if in compliance with the
license. I kindly ask that you credit the engine and myself (Francisco Vivas Puerto aka "DaFrancc") in
both the repo and any games you make with this engine. Credit is not required, but it would be greatly
appreciated.

### Third-party assets

Assets in `assets/` that were not made for this engine, with their source and
the credit their terms ask for. Add a line here for every asset brought in from
outside; a file with no entry is a file nobody has checked.

- `assets/textures/moon.jpg` — Moon albedo — NASA's Scientific Visualization
  Studio (Ernie Wright), Moon Phase and Libration, 2026:
  https://svs.gsfc.nasa.gov/5587/
  LROC WAC colour mosaic: NASA/GSFC/Arizona State University.
  Not subject to copyright in the US; credit given as requested. Nothing here
  implies NASA endorsement, and the NASA insignia and worm are not used.

### AI notice

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

</details>
