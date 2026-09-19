# Installation

This chapter gets you from nothing to the editor running on your screen. It
takes four steps:

1. Get the code.
2. Install the tools and a few system packages.
3. Configure: CMake downloads and prepares everything else.
4. Build and run the editor.

> **Tip:** The first configure downloads and builds every library the engine
> uses. It takes several minutes, but only happens once.

## What your computer needs

- **Windows 10/11 or Linux.** macOS isn't supported.
- **A GPU with Vulkan support** and an up-to-date driver. The engine draws with
  Vulkan. You don't need the Vulkan SDK, just a driver.
- **An x86-64 CPU with AVX2**: Intel Haswell (2013) or newer, or AMD Zen
  (2017) or newer. The engine is compiled to use these instructions everywhere,
  so it crashes on older CPUs.

## 1. Get the code

```bash
git clone https://github.com/DaFrancc/Assisi.git
cd Assisi
```

## 2. Install the tools

You need a C++ compiler, **CMake 3.28+**, **Ninja**, **Make**, **ccache** and
**Python 3**. ccache makes rebuilds faster, and the build expects it to be
installed. Python runs a code generator during the build; it only uses the
standard library, so there's nothing to `pip install`.

You don't install any C++ libraries yourself. CMake downloads and builds all of
them during the configure step.

### Windows

Install [Visual Studio 2022 or newer](https://visualstudio.microsoft.com/) with
the **Desktop development with C++** workload. That gives you the MSVC compiler,
CMake and Ninja. Also install [Python 3](https://www.python.org/) and make sure
it's on your `PATH`. Also install [Make](https://www.gnu.org/software/make/) and
[ccache](https://ccache.dev/), and make sure both are on your `PATH`.

Run every command in this book from a **Developer Command Prompt for VS**, which
you'll find in the Start menu. A normal terminal can't find the compiler.

### Arch Linux

```bash
sudo pacman -S --needed base-devel git cmake ninja ccache python \
                        wayland libxkbcommon \
                        libxcursor libxi libxinerama libxrandr \
                        vulkan-icd-loader
```

Then the Vulkan driver for your GPU. Run the one that matches your card:

**AMD**

```bash
sudo pacman -S vulkan-radeon
```

**Intel**

```bash
sudo pacman -S vulkan-intel
```

**NVIDIA**, on the standard `linux` kernel:

```bash
sudo pacman -S nvidia nvidia-utils
```

**NVIDIA**, on any other kernel (`linux-lts`, `linux-zen`, …):

```bash
sudo pacman -S nvidia-dkms nvidia-utils
```

Reboot after installing an NVIDIA driver.

### Fedora, RHEL, Rocky, Alma

```bash
sudo dnf install gcc-c++ make git cmake ninja-build ccache python3 pkgconf-pkg-config \
                 libstdc++-static \
                 wayland-devel libxkbcommon-devel \
                 libXcursor-devel libXi-devel libXinerama-devel libXrandr-devel \
                 vulkan-loader
```

Then the Vulkan driver for your GPU. Run the one that matches your card:

**AMD or Intel**

```bash
sudo dnf install mesa-vulkan-drivers
```

**NVIDIA**: the driver comes from the RPM Fusion repositories. First enable
them:

```bash
sudo dnf install https://mirrors.rpmfusion.org/free/fedora/rpmfusion-free-release-$(rpm -E %fedora).noarch.rpm \
                 https://mirrors.rpmfusion.org/nonfree/fedora/rpmfusion-nonfree-release-$(rpm -E %fedora).noarch.rpm
```

Then install the driver:

```bash
sudo dnf install akmod-nvidia xorg-x11-drv-nvidia
```

**Don't reboot yet.** The driver takes a few minutes to build in the background,
and rebooting too early leaves you without a working driver. Run this until it
prints a version number, then reboot:

```bash
modinfo -F version nvidia
```

Those two repository links are for Fedora. On RHEL, Rocky or Alma, get the
matching ones from [rpmfusion.org/Configuration](https://rpmfusion.org/Configuration).

### Debian and Ubuntu (untested)

Nobody has built the engine on Debian or Ubuntu yet, so treat this as a best
guess. You need **Debian 13+** or **Ubuntu 24.04+**; older releases ship a
compiler and CMake that are too old.

```bash
sudo apt install build-essential git cmake ninja-build ccache python3 pkg-config \
                 libwayland-dev libwayland-bin libxkbcommon-dev \
                 libxcursor-dev libxi-dev libxinerama-dev libxrandr-dev \
                 libvulkan1
```

Then the Vulkan driver for your GPU. Run the one that matches your card:

**AMD or Intel**

```bash
sudo apt install mesa-vulkan-drivers
```

**NVIDIA on Ubuntu**: this picks the right driver version for your card:

```bash
sudo ubuntu-drivers install
```

**NVIDIA on Debian**: first enable the `contrib` and `non-free` components
(they're off by default), then:

```bash
sudo apt install nvidia-driver
```

Reboot after installing an NVIDIA driver.

### Optional: podman, for release builds on Linux

To release a Linux game that runs on other people's distributions, you build it
inside Valve's Steam Runtime, which needs **podman** (or docker). You don't need
it to follow this book or to make a game, so you can skip it until you're ready
to release. [Building for every Linux distribution](steam-runtime.md) covers it,
including how to install podman.

<details>
<summary>What are all those Linux packages for?</summary>

| Package group | Why |
|---|---|
| Static C++ runtime (`libstdc++-static` on Fedora) | The game carries its own copy of the C++ runtime, so players don't need a matching one. Arch and Debian include it with the compiler. |
| Wayland, libxkbcommon | The window library (GLFW) supports Wayland. |
| Xcursor, Xi, Xinerama, Xrandr | GLFW also supports X11. Both are built, and the right one is picked when the game starts. |
| Vulkan loader and driver | Only needed to *run*, not to build. |

</details>

## 3. Configure

Configuring tells CMake which compiler to use and downloads the engine's
libraries. **Run only the one command for your system**, once.

**Linux** (GCC, the usual choice):

```bash
make configure-gcc
```

**Linux with Clang** instead, if you prefer it:

```bash
make configure-clang
```

**Windows**, from the Developer Command Prompt:

```bash
make configure-msvc
```

This is the slow step. Later configures are fast because the downloads are
cached in `out/_deps-src`.

## 4. Build and run the editor

Build the **dev** configuration. It's optimized, so the editor runs smoothly,
but still debuggable. Use the same compiler you configured in step 3.

**Linux (GCC)**

```bash
make gcc-dev
```

**Linux (Clang)**

```bash
make clang-dev
```

**Windows**

```bash
make msvc-dev
```

**dev** is the right build for everyday work, but it isn't the only one:

- **To step through your code in a debugger**, you need a **debug** build.
- **To run the game fully optimized**, for a release or to measure performance,
  you need a **ship** build.

**If you want either, open "Every build target" below** for the exact commands.

<details>
<summary>Every build target</summary>

| | Linux (GCC) | Linux (Clang) | Windows |
|---|---|---|---|
| **debug** | `make gcc-debug` | `make clang-debug` | `make msvc-debug` |
| **dev** | `make gcc-dev` | `make clang-dev` | `make msvc-dev` |
| **ship** | `make gcc-ship` | `make clang-ship` | `make msvc-ship` |

Each build goes into its own folder, like `out/build/gcc-ship/`, so building one
doesn't replace another. To run a different build, swap `dev` in the editor path
below for its name, e.g. `out/build/gcc-debug/...`.

There are also sanitizer builds for hunting memory bugs and data races, and
profiler builds. [Build types](build-types.md) explains all of them and when to
use each. `make help` lists every target.

</details>

Then start the editor with the example level open.

**Linux (GCC)**

```bash
./out/build/gcc-dev/apps/game/Assisi-GameEditor -l levels/Test.alvl
```

**Linux (Clang)**

```bash
./out/build/clang-dev/apps/game/Assisi-GameEditor -l levels/Test.alvl
```

**Windows**

```bash
.\out\build\msvc-dev\apps\game\Assisi-GameEditor.exe -l levels/Test.alvl
```

A window should open showing the level. Hold the **right mouse button** and use
**W A S D** to fly around. **You're set up.**

`-l` picks the level to open. Without it the editor starts with an empty world,
and you can load a level from its **Levels** panel.

From now on, rebuilding after a code change is the same build command as above.
It only rebuilds what changed. Each one also has a short alias: `make gv` (GCC),
`make cv` (Clang), `make mv` (Windows).

## If something went wrong

- **The configure step fails naming a package**: install that package and run
  the configure again.
- **The editor starts and immediately exits, or complains about Vulkan**: check
  that your GPU driver is installed and supports Vulkan.
- **"Illegal instruction" on startup**: your CPU is older than the AVX2
  baseline above.

[Troubleshooting and FAQ](troubleshooting.md) covers more.

<details>
<summary>Optional: running the tests</summary>

The engine has unit tests. Build first, then:

```bash
ctest --preset gcc-dev          # every test suite
ctest --preset gcc-dev -R ECS   # just the suites whose name matches "ECS"
```

</details>
