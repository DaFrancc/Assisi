# Turning networking on and off

**What this is for:** building Assisi with or without its networking modules.

**Why you would want it off:** networking pulls in GameNetworkingSockets and,
through it, protobuf and abseil — by far the heaviest configure in the tree. If
you are only touching ECS, rendering, or the asset pipeline, you can skip
fetching and compiling all of it.

Networking is **ON by default**, and that is the right setting unless you have a
reason. It is a first-class engine subsystem, not an optional backend.

---

## The short version

```bash
# Off — a fresh build directory, so nothing is shared with your normal one.
cmake --preset gcc-debug -DASSISI_ENABLE_NETWORKING=OFF -B out/build/gcc-nonet
cmake --build out/build/gcc-nonet

# On — the ordinary build. Nothing to pass; it is the default.
make gd
```

The switch is the CMake option `ASSISI_ENABLE_NETWORKING`, declared in
`CMakeLists.txt`.

---

## The one thing that will catch you

**A build directory remembers.** CMake caches the option the first time a
directory is configured, and re-running the preset without `-D` will *not*
change it back:

```bash
cmake --preset gcc-debug -DASSISI_ENABLE_NETWORKING=OFF   # off
cmake --preset gcc-debug                                  # STILL off
```

To flip an existing directory you have to say so explicitly:

```bash
cmake --preset gcc-debug -DASSISI_ENABLE_NETWORKING=ON
```

Check what a directory is actually set to rather than guessing:

```bash
grep ASSISI_ENABLE_NETWORKING out/build/gcc-debug/CMakeCache.txt
```

**The recommendation:** use a separate build directory for the networking-off
build (`-B out/build/gcc-nonet` above) and leave your normal one alone. Flipping
one directory back and forth rebuilds most of the tree each time, because the
option changes a compile definition every target sees.

---

## What you lose with it off

The engine, the editor and the sandbox all still build and run. What disappears
is everything that needs a socket:

- **`Assisi::Net` and `Assisi::NetSync`** are not built at all.
- **The editor's Network panel**, hosting, and joining.
- **Play-in-editor *clients*** — the "Host + 2 clients" modes in the Play
  dropdown. Plain play-in-editor is not networking and still works.
- **The inspector's replication block** — the NetId readout, the Relevance
  dropdown, the per-component "sends" checklist, and the wire glyph on component
  headers. There is no `Replicated` component in this build for them to be about.
- **The sandbox's `--host` and `--connect`.** `--server` is *not* networking —
  it is headless simulation — and keeps working.

Asking for a networked role anyway is refused out loud rather than quietly
downgraded:

```
$ ./Assisi-Sandbox --host 27015
[ERROR] Server: this build was configured with ASSISI_ENABLE_NETWORKING=OFF, so
        --host and --connect do nothing. Reconfigure with networking on, or use
        --server for headless simulation.
```

A `--host` that silently hosts nothing is worse than one that says it cannot.

---

## Tests

`ctest` reports **14** suites with networking on and **12** with it off. That is
correct, not a failure: the `Net` and `NetSync` suites do not exist in a build
that does not compile those modules.

```bash
cd out/build/gcc-nonet && ctest      # expect 12/12
cd out/build/gcc-debug && ctest      # expect 14/14
```

---

## If you are writing code

The CMake option is mirrored into the preprocessor as **`ASSISI_NETWORKING`**,
defined on `Assisi-Options` so every target sees the same value. Guard with it
the same way the existing code does:

```cpp
#if defined(ASSISI_NETWORKING)
#    include <Assisi/NetSync/NetComponents.hpp>
#endif
```

**Prefer not needing the macro.** The better pattern — and what most of the tree
does — is to put networking code in its own translation unit and let the build
leave the file out entirely. `modules/App/CMakeLists.txt` does this with
`BlueprintReplication.cpp`, and `modules/Editor/CMakeLists.txt` with
`EditorNet.cpp`:

```cmake
if (ASSISI_ENABLE_NETWORKING)
  target_sources(Assisi-Editor PRIVATE "src/EditorNet.cpp")
  target_link_libraries(Assisi-Editor PUBLIC Assisi::NetSync)
endif()
```

`#if` is for the two cases a whole file cannot cover: a public header that names
a NetSync type in its own interface (`EditorApp.hpp`), and a file with a genuine
non-networking half (`ServerApp.cpp`, whose Offline role is just headless
simulation).

Two traps worth knowing:

- **Do not add a conditional source with a `$<BOOL:...>` generator expression.**
  `Assisi_apply_test_defaults` reads the `SOURCES` property to attach per-source
  warning opt-outs, and an unevaluated genex comes back as its own literal text —
  it matches no file, so the opt-outs silently miss that source. Use
  `if()` + `target_sources`.
- **A lifecycle override must not live in a networking-only file.** `OnShutdown`
  did, and dropping that file left a hole in the vtable that only showed up at
  link time.

---

## Nothing enforces this

No preset and no CI job builds with networking off, so this configuration can
break without anyone noticing until they try it. If you change the editor or the
sandbox in a way that touches NetSync, build it both ways before you push:

```bash
cmake --build out/build/gcc-nonet
```
