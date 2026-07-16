# Code Review Notes — 2026-07-06

Findings from a full read-through of `main` (ce936ca) with samples from `origin/dev`
(d628f8c). Ordered roughly by impact. Each entry: what/where, why it matters, and a
suggested direction for the fix.

---

## 1. No tests, no CI

**Where:** Project-wide — there is no test target and no `.github/workflows/`.

**Why it matters:** The codebase has outgrown eyeball-verification-in-the-sandbox.
`SparseSet`, `Registry`, `Scene`, and `AssetSystem` are pure logic where a subtle bug
(e.g. a swap-remove edge case) corrupts state silently rather than crashing. Linux
support and the `ASSISI_WARNINGS_AS_ERRORS` flag exist but nothing exercises them
automatically, so they will rot.

**Fix direction:**
- Add a `tests/` app target using Catch2 or doctest (single-header, fits the
  module pattern: `apps/Tests` linking `Assisi::ECS`, `Assisi::Core`).
- First tests to write, in order of value:
  - `SparseSet`: add/remove/has round-trips; remove the *last* element; remove an
    element and verify the swapped-in entity's sparse index is updated; re-add after
    remove.
  - `Registry`: generation bump invalidates stale handles; slot reuse; `Destroy` on
    an already-dead entity is a no-op.
  - `Scene::Query`: entity present in some pools but not all; empty pool short-circuit.
  - `AssetSystem`: `NormalizeVirtualPath` rejection cases (`..`, absolute, drive-qualified),
    root-escape check.
- CI: a GitHub Actions matrix (windows-latest MSVC, ubuntu-latest GCC + Clang) that
  configures, builds with `ASSISI_WARNINGS_AS_ERRORS=ON`, and runs the tests. The
  sanitize presets already exist — run the test suite under ASan/UBSan on Linux.

---

## 2. ECS queries are not safe against structural changes, and nothing says so

**Where:** `modules/ECS/include/Assisi/ECS/Query.hpp`, `Scene.hpp` (`Scene::Query`).

**Why it matters:** `QueryView::Iterator` holds a pointer into the primary pool's
`_entities` vector. An `Add<T>` during iteration can reallocate `_dense`/`_entities`
(dangling pointer), and a `Remove`/`Destroy` swap-removes under the iterator's feet
(skipped or repeated elements). The dev-branch editor and spawn systems do a lot of
mid-frame entity creation, so this will eventually be hit in a loop body.

**Fix direction (pick one, cheapest first):**
1. **Document the contract** on `Scene::Query` and `QueryView`: "no structural changes
   (Add/Remove/Destroy) to any queried component type during iteration." Zero cost,
   should happen regardless.
2. **Deferred command buffer:** a small `CommandBuffer` that records
   Create/Destroy/Add/Remove during iteration and is flushed by the caller (or by
   `SystemRegistry` between systems). This is the standard ECS answer and fits the
   dev-branch `SystemPhase` model well — flush at phase boundaries.
3. Debug-build guard: an `_iterationDepth` counter on `SparseSet`; assert on
   `Add`/`Remove` while nonzero. Catches violations loudly in debug without any
   release cost.

---

## 3. Frame limiter builds up a backlog after a hitch

**Where:** `modules/App/src/Application.cpp` — `Run()`, `nextRenderTime += ...`
(line ~257).

**Why it matters:** `nextRenderTime` only ever advances by `renderStep`. After a long
stall (window drag, shader compile, breakpoint), `nextRenderTime` is far in the past,
so the sleep loops don't wait at all and the app renders a burst of uncapped frames
until the schedule catches up to `now`.

**Fix direction:** Resync when behind — after the increment:

```cpp
nextRenderTime += std::chrono::duration_cast<Clock::duration>(Seconds(renderStep));
if (nextRenderTime < now)          // fell behind; don't try to "make up" frames
    nextRenderTime = now;
```

(`dt` is already clamped to 0.25 s, so the simulation side is fine; this is purely
the pacing schedule.)

---

## 4. Fast-math defaults ON in Release

**Where:** Root `CMakeLists.txt` — `option(ASSISI_ENABLE_FAST_MATH ... ON)`;
applies `/fp:fast` / `-ffast-math` via `Assisi::Perf`, which every module links.

**Why it matters:** `-ffast-math` assumes no NaN/Inf and breaks `std::isnan`-style
checks; results differ across compilers, which undermines any future physics
determinism (Jolt cares about float semantics). It's linked into *everything*,
including physics glue and camera math, not just hot shading loops. The risk grew
with the engine — this was more defensible when it was a spinning cube.

**Fix direction:**
- Flip the default to `OFF`.
- If profiling later shows a win, apply it per-target instead of via the global
  `Assisi::Perf` interface (e.g. only on `Assisi-Render`), or better, per-file on the
  hot loops. Keep everything touching Jolt and gameplay logic at strict/precise.

---

## 5. (dev) Unconditional `-mavx2 -mfma -mf16c -mlzcnt -mbmi` on GCC/Clang

**Where:** dev-branch root `CMakeLists.txt`, `Assisi-Options` non-MSVC branch.

**Why it matters:** Hard-requires an x86-64 CPU with AVX2. On ARM (Raspberry Pi,
Apple Silicon via Linux VMs) the flags are simply invalid and configure/compile fails
with a confusing error. Also crashes with SIGILL on pre-2013 x86 CPUs. Fine as a
deliberate baseline, but it's currently implicit.

**Fix direction:**
- Gate on architecture: `if (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")`.
- Expose as an option (`ASSISI_SIMD_BASELINE=avx2|sse2|native|off`) so the Jolt
  requirement stays satisfiable while other platforms can build. Note Jolt itself
  must be compiled with the *same* instruction set as the code that includes its
  inline headers — keep the two in lockstep in one place.

---

## 6. Crash handler allocates before writing the minidump

**Where:** `modules/App/src/Application.cpp` — `CrashHandler()`: `Log::Fatal(...)`
(which runs `std::format` → heap allocation, plus file-sink I/O) executes *before*
`MiniDumpWriteDump`.

**Why it matters:** If the crash is heap corruption or an allocation failure, the
`std::format` call inside the handler can itself crash, deadlock on a heap lock, or
recurse — and then the minidump (the artifact you actually need) is never written.

**Fix direction:** Reorder: write `crash.dmp` first, log after. For the log line
itself, prefer allocation-free output in the handler (`WriteFile` to `STD_ERROR_HANDLE`
with a fixed-size `snprintf` buffer). Same concern applies to `AbortHandler`.

---

## 7. Sandbox override hygiene

**Where:** `apps/sandbox/src/main.cpp` — `SandboxApp` declares `OnStart`,
`OnFixedUpdate`, `OnUpdate`, `OnRender`, `OnImGui` **without** `override`
(only `OnResize` has it).

**Why it matters:** A signature drift in `Application` (e.g. changing `OnUpdate(float)`
to `OnUpdate(double)`) would silently turn these into shadowing non-overrides for
`OnImGui` (non-pure), and hard-to-read errors for the pure ones. `/w14263` is enabled
but only fires in some cases.

**Fix direction:** Add `override` to all five; consider `-Wsuggest-override` /
`/w14263` + WX in CI to enforce it repo-wide.

---

## 8. Stale/diverging docs between `main` and `dev`

**Where:** `main` README + `CLAUDE.md` describe the Conan workflow; dev's
cmake-migration removes Conan entirely and **deletes CLAUDE.md**. Profiles were
renamed (`release`→`ship`, `sanitize`→`dev`).

**Why it matters:** `main` is the default branch — a visitor follows setup docs for a
build system the project has abandoned. And losing CLAUDE.md means losing the
architecture map that keeps agent/tooling assistance accurate.

**Fix direction:**
- Merge `dev` → `main` (it looks stable: its own PR flow, "Linux builds stable"), or
  short-term add a README banner on `main` pointing at `dev`.
- Regenerate CLAUDE.md after the merge (`/init`) instead of deleting it — the current
  one's build instructions are the only part that's wrong; the module table and
  conventions are still valuable.

---

## 9. Minor nits (batch these with other work)

- `modules/ECS/src/Registry.cpp` (~line 25): comment says "new slot at index 0" but
  the slot is appended at the end. Fix the comment.
- `Application::RenderFrame`: the `AaMode::None` and else branches are identical
  except for the bind target — collapse to a single path
  (`mode == None ? BindDefault() : _mainFB.Bind()`).
- `Shader::LoadFromAssets` returns success (`return {};`) on GLSL compile/link
  failure by design (documented, `IsValid()` exists). Consider widening the error
  type to `std::expected<void, ShaderError>` (wrapping AssetError + CompileFailed +
  LinkFailed) so callers get one uniform failure channel instead of two.
- `mesh.frag` / `ClusterGrid` GPU struct comments say layouts "must match exactly" —
  add `static_assert(sizeof(PointLight) == 32)`-style checks on the C++ mirror structs
  so a padding change fails at compile time instead of rendering garbage.
