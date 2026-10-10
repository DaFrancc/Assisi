# Installation

This chapter gets you from nothing to the editor running on your screen. It
takes three steps:

1. Get the code.
2. Install the tools and a few system packages.
3. Build and run the editor with one command.

> **Tip:** The first build downloads and builds every library the engine uses.
> It takes several minutes, but only happens once.

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

You need a C++ compiler, **CMake 3.28+**, **Ninja**, **ccache** and **Python
3.9+**. ccache makes rebuilds faster, and the build expects it to be installed.
Python runs the engine's build tool, `assisi`, and a code generator during the
build. Neither needs anything from `pip`.

You don't install any C++ libraries yourself. CMake downloads and builds all of
them the first time you build.

### Windows

Install [Visual Studio 2022 or newer](https://visualstudio.microsoft.com/) with
the **Desktop development with C++** workload. That gives you the MSVC compiler,
CMake and Ninja. Also install [Python 3](https://www.python.org/) and
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
sudo dnf install gcc-c++ git cmake ninja-build ccache python3 pkgconf-pkg-config \
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

## 3. Build and run the editor

Everything from here on goes through `assisi`, the engine's build tool, which
sits in the top folder of the repository. First check that step 2 left nothing
out:

**Linux**

```bash
./assisi doctor
```

**Windows**, from the Developer Command Prompt:

```bash
assisi doctor
```

It lists each tool with `ok`, or `MISSING` and what to install. When it says
**All required tools are present**, build the editor and start it with the
example level open:

**Linux**

```bash
./assisi run editor -- -l levels/Test.alvl
```

**Windows**

```bash
assisi run editor -- -l levels/Test.alvl
```

The first time, this downloads and builds the engine's libraries before it
builds the editor, which is the slow part. The downloads are kept in
`out/_deps-src`, so it never happens again.

A window should open showing the level. Hold the **right mouse button** and use
**W A S D** to fly around. **You're set up.**

Anything after `--` is handed to the editor. `-l` picks the level to open;
without it the editor starts with an empty world, and you can load a level from
its **Levels** panel.

From now on, run the same command after changing code. It rebuilds only what
changed, then starts the editor. To build without starting it, use
`./assisi build`.

### Which build that was

`./assisi` built the **dev** level with your system's usual compiler: GCC on
Linux, MSVC on Windows. It lives in `out/build/gcc-dev/` (or `msvc-dev`). dev is
optimized, so the editor runs smoothly, but still debuggable, which makes it
the right level for everyday work. Two others are worth knowing:

- **To step through your code in a debugger**, you need the **debug** level:
  `./assisi run editor debug`.
- **To run the game fully optimized**, for a release or to measure performance,
  you need the **ship** level: `./assisi build ship`.

Each level goes into its own folder, so building one doesn't replace another.
The compiler is a separate choice: add `--compiler clang` to use Clang instead
of GCC, or run `./assisi default --compiler clang` to make it your usual one.
[Build types](build-types.md) explains every level and option.

### The menu

Run `./assisi` with nothing after it and it opens a menu in your terminal
instead: every build with what has been built of it, your most used commands,
and keys to build, package or start the editor. The first time, it downloads
the few Python packages the menu uses into `out/tool-env/`, which needs
Python 3.10 or newer. Every command works without the menu.

## If something went wrong

- **The first build fails naming a package**: install that package and run the
  same command again.
- **The editor starts and immediately exits, or complains about Vulkan**: check
  that your GPU driver is installed and supports Vulkan.
- **"Illegal instruction" on startup**: your CPU is older than the AVX2
  baseline above.

[Troubleshooting and FAQ](troubleshooting.md) covers more.

<details>
<summary>Optional: running the tests</summary>

The engine has unit tests. This builds, then runs them:

```bash
./assisi test                   # every test suite
./assisi test -- -R ECS         # just the suites whose name matches "ECS"
```

</details>
