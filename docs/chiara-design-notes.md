# Chiara — performance and memory capture (design notes)

**Status:** Planned 2026-07-31, branch `Chiara`. Nothing built yet; this is the
plan settled before any code, and stage 0 is writing it down.

Supersedes `frame-profiler-design-notes.md` (see §11) and discharges the
"instrumentation is temporary" debt recorded in `streaming-upload-perf-plan.md`.

Named for Clare of Assisi — patron saint of television, because she reportedly
saw a Mass she was too ill to attend play out on the wall of her room. Watching
something you already missed is the whole job.

## 0. Problem

The streaming-spike hunt (2026-07-24) was won with instrumentation that was
never meant to survive it: `Log::Info` calls with hard-coded thresholds in
`Application::Run`, `Application::RenderFrame`, `VulkanContext`, and
`AssetCache::PumpPublishes`. They were decisive — they overturned two wrong
hypotheses and found a cause nothing else would have — and they are scattered,
always-on, and answer only the question they were cut for. The plan doc that
introduced them says so outright: they are "to be **removed and replaced by a
proper performance and memory capture system**."

Three things made them work, and any replacement that loses one is a
regression:

- **Per-phase attribution.** Which part of the frame ate the time.
- **An explicit unaccounted figure.** `cpuMs` minus the sum of the phases. When
  it is ~0 the main thread was never descheduled, which is how contention was
  ruled out — a *negative* result that saved building the wrong fix.
- **Deferred-cost visibility.** GPU allocation churn caused in frame N surfaced
  as `runGarbageCollection` cost in a later, unrelated frame. "Neither cost is
  paid where it is caused" is the sentence the whole design has to answer.

## 1. Decision

**Chiara is a capture pipeline, not a viewer.** Instrumented code emits fixed
size binary events into per-thread ring buffers; on demand, a background job
serializes a recent window to a Chrome JSON trace file; the user opens it in
**Perfetto** (ui.perfetto.dev). We write no timeline UI. Perfetto is a mature
local-only trace viewer with zoomable per-thread tracks, counter graphs and
flow arrows — every display feature the deferred frame-profiler notes wanted,
none of it ours to build or maintain.

The format is a detail we own. If Perfetto ever falls short, the binary event
model is unchanged and we add a second exporter.

**Compiled out unless asked, on every configuration.** `ASSISI_ENABLE_CHIARA`
defaults **OFF** for debug, dev and ship alike. Every make alias gains a `-c`
variant (`gd-c`, `gv-c`, `gs-c`, …) that builds the same configuration with
Chiara compiled in. This matters more than a config-keyed rule would: the
build worth profiling is the *optimized* one — the streaming hunt's central
methodological finding was that ~95% of a 66 ms spike was a `-O0` artifact —
so `make gs-c` (ship + capture) has to be a first-class combination, not a
workaround.

## 2. Placement — a module below Core

`modules/Chiara`, added before `modules/Core`, linked **PUBLIC by Core**.

The macro has to be callable from Render, Physics, App — and from Core itself:
`JobSystem::WorkerLoop` is the only place worker-thread identity exists, so it
must register threads with Chiara. That forces Core → Chiara, and rules out
putting the collector inside Core.

Chiara links **nothing** — standard library only. The event core needs
`<atomic>`, `<chrono>`, `<thread>`, `<mutex>`; the serializer is flat enough
for `std::format` and does not want nlohmann. A module with no dependencies
can never be dragged up the stack by one.

Everything that does need the upper layers is glue, and lives where its
dependencies are: frame-loop scopes and the counter pump in App, the capture
panel in App (so games get it too, which is the reasoning the old frame
profiler notes applied to its widget), call-site scopes in each module.

    Chiara → Core / Math → Window / Geometry → Render → ECS → Runtime
                         → Physics / Debug → App → Editor → sandbox

### Layout

```
modules/Chiara/
  CMakeLists.txt
  include/Assisi/Chiara/
    Event.hpp        # Event struct + EventType, shared by collector, serializer, tests
    Chiara.hpp       # runtime API: init, threads, recording, intern, flows, frame id, stats
    Profile.hpp      # the macros — the only header most call sites include
    Serializer.hpp   # SerializeCapture
  src/
    Chiara.cpp
    Serializer.cpp
  tests/             # ctest name "Chiara"
```

### Build wiring

`option(ASSISI_ENABLE_CHIARA "Compile the Chiara capture system in" OFF)`, and
when ON, `target_compile_definitions(Assisi-Chiara PUBLIC ASSISI_CHIARA_ENABLED=1)`.
A plain option, deliberately not a `$<CONFIG>` generator expression — the
configuration and the capture decision are independent.

Nine configure presets `{msvc,gcc,clang}-{debug,dev,ship}-chiara`, each
inheriting its base and flipping the cache variable, with mirrored build and
test presets. This follows the sanitizer presets exactly (`gcc-asan` inherits
`gcc-debug`), and the reason is the same: a separate build directory. The
define is PUBLIC, so toggling it in place would rebuild the world on every
flip; separate directories make switching free, and the FetchContent cache is
shared across them anyway.

The Makefile gains `-c` variants of every alias, configure-and-build in one
step like the sanitizer targets.

**Excision when OFF** — the whole of `Chiara.cpp` and `Serializer.cpp` sits
inside `#if defined(ASSISI_CHIARA_ENABLED)`, and the macros expand to the
`Assert.hpp` house pattern:

```cpp
#define ASSISI_PROFILE_SCOPE(name) ((void)sizeof(name))
```

Both operands sit under `sizeof`: parsed and type-checked, never evaluated, no
code emitted. An instrumentation typo cannot bit-rot until someone next builds
with `-c`.

There is no compiled-in-but-disabled *build*. Runtime enable/disable within a
Chiara build is a different thing and does exist — one relaxed atomic load.

## 3. Public API

```cpp
// Profile.hpp — what call sites use
ASSISI_PROFILE_SCOPE("name")           // RAII scope; name is a literal or interned pointer
ASSISI_PROFILE_FUNCTION()              // same, using __func__
ASSISI_PROFILE_COUNTER("group/name", v)
ASSISI_PROFILE_FLOW_BEGIN("name", id)  // deferred-cost linkage — see §7
ASSISI_PROFILE_FLOW_END("name", id)
ASSISI_PROFILE_FRAME()                 // main thread only; Chiara owns the frame id
```

```cpp
namespace Assisi::Chiara
{
struct Config
{
    std::uint32_t mainThreadBufferBytes  = 32u << 20;
    std::uint32_t otherThreadBufferBytes = 2u << 20;
};

void Initialize(const Config &config = {});  // idempotent; registers the caller as "main"; sets the epoch
void Shutdown();                             // stops recording; buffers are not freed (§4)

void SetRecording(bool enabled);
[[nodiscard]] bool IsRecording();

void RegisterCurrentThread(const char *name); // Chiara track name + OS thread name

std::uint64_t MarkFrame();                    // ++frame id, emits FrameMark
[[nodiscard]] std::uint64_t CurrentFrame();

[[nodiscard]] const char *InternString(std::string_view text); // for dynamic names; mutex-guarded, cache the result
[[nodiscard]] std::uint64_t NewFlowId();                       // never returns 0

void EmitCounter(const char *name, double value);
void EmitFlowBegin(const char *name, std::uint64_t flowId);
void EmitFlowEnd(const char *name, std::uint64_t flowId);

class ScopeTimer
{
  public:
    explicit ScopeTimer(const char *name);
    ~ScopeTimer();
    ScopeTimer(const ScopeTimer &) = delete;
    ScopeTimer &operator=(const ScopeTimer &) = delete;
  private:
    const char   *_name;
    std::uint64_t _startNs;  // 0 means disarmed — recording was off at entry
};

struct CaptureStats
{
    std::uint64_t totalEventsWritten;
    std::uint64_t bufferWrapCount;
    std::uint32_t threadCount;
    double        mainWindowSeconds;  // time span currently held in the main ring
};
[[nodiscard]] CaptureStats GetCaptureStats();
}
```

Every function gets an inline empty stub in the `#else` branch, so glue code
never needs an `#ifdef`.

**Clock:** `steady_clock`, stored as `uint64_t` nanoseconds since `Initialize`.
Every existing timing site already uses it, and the vDSO `clock_gettime` is
~20–25 ns. rdtsc would save maybe 15 ns per read and cost calibration and
cross-core validity worries; not worth it.

**Cost, stated rather than hidden** (the self-measurement caveat inherited from
the old notes): while recording, roughly **60–90 ns per scope** — two clock
reads, one relaxed load, one 32-byte store, one cursor bump. A heavy main
thread frame at ~350 events is ~25–30 µs, under 0.2% of a 16 ms frame. Not
recording: one relaxed load and a branch at construction, nothing at
destruction.

## 4. Event model

```cpp
enum class EventType : std::uint16_t
{
    Scope = 0,      // timestampNs = begin; payload = duration ns
    Counter = 1,    // payload = bit_cast<uint64_t>(double)
    FlowBegin = 2,  // payload = flow id
    FlowEnd = 3,
    FrameMark = 4,  // payload = frame index
    Instant = 5,    // reserved
};

struct Event
{
    std::uint64_t timestampNs;
    std::uint64_t payload;
    const char   *name;      // interned; resolved only at serialize time
    std::uint16_t type;
    std::uint16_t reserved0;
    std::uint32_t reserved1;
};
static_assert(sizeof(Event) == 32);
```

**Scopes are one event, written at destruction** — begin plus duration, which
is Chrome's `ph:"X"` complete event. Half the pushes of a begin/end pair, and
it handles the engine's help-waiting for free: when `Task::Wait` runs other
tasks inline, the inner scopes are wall-clock-contained inside the waiting
scope, which is exactly what a complete-event nesting renders. The truth is
that the thread really did do that work inside that window.

Events therefore land ordered by *end* time. The Chrome format does not
require sorted input; Perfetto sorts on import.

**Names are interned pointers, never copied.** Macro sites pass string
literals, whose lifetime is the program. Dynamic names (asset paths) go
through `InternString` once and get cached by the caller — never per frame.
The serializer is the only thing that dereferences a name.

**Per-thread rings, single-producer.** A `thread_local ThreadBuffer *`, a
power-of-two event array, and a monotonic write cursor incremented with
release *after* the slot store. The registry (thread list, names) is
mutex-guarded; the emit path takes no lock. First emit from an unregistered
thread auto-registers it as `thread-<n>` — that is how a Jolt worker or the
NVML thread would appear if it ever emits.

**Drop policy: overwrite oldest.** The spike being chased is always the recent
one. Wrapping is counted and the per-ring coverage window is reported in
`CaptureStats` and shown in the panel, so a too-small buffer is visible rather
than silently truncating history.

**Sizing.** 32 MiB main ring = 1,048,576 events. At a heavy ~350 events/frame:
~21 s at 144 fps, ~55 s at 60 fps. Workers emit little in v1, so 2 MiB is
hours. Fifteen workers plus main is ~62 MiB — acceptable in a build you opted
into, and both numbers are in `Config`.

**Buffers and the intern table are never freed.** The standard profiler
pattern, and it makes late-thread teardown ordering (Jolt's pool, the NVML
worker, static destructors) a non-issue. `Shutdown()` only stops recording.

## 5. Capture model — always recording, dump the last N

Recording starts at `Initialize` and runs continuously. This is settled by the
use case: by the time you have decided the thing is worth recording, the spike
already happened. The panel offers *dump the last 5 s / 15 s / everything*,
plus a pause toggle for A/B experiments. Explicit start/stop is the same
mechanism and needs no extra API.

`SerializeCapture(path, lastSeconds)` pauses recording, walks each thread's
ring newest-first within the window, streams JSON through a buffered
`ofstream` (no whole-file string in RAM), resumes recording, and returns
counts. It runs on a JobSystem worker with a `Then(Main, …)` continuation for
the panel's status, so the frame never hitches on a dump. Capture is blind
while it writes; acceptable, because a dump happens after the interesting part.

A thread mid-push when the cursor is sampled can complete at most one straggler
event past the observed cursor; it simply isn't read.

## 6. Perfetto mapping — Chrome JSON

The Chrome trace event format, for v1: trivially writable with `std::format`,
zero dependencies, and Perfetto opens it natively including counters and flow
arrows. All events use `pid` 1.

| Chiara | JSON |
| --- | --- |
| File | `{"displayTimeUnit":"ms","traceEvents":[…]}` |
| Metadata | `ph:"M"` `process_name` = "Assisi"; per thread `thread_name`, plus `thread_sort_index` so main pins to the top |
| Scope | `ph:"X"` with `ts`/`dur` in fractional microseconds (three decimals keeps ns precision) |
| Counter | `ph:"C"`, `args:{"v":…}`; names are `group/name` so Perfetto groups the tracks |
| Flow | `ph:"s"` at the cause, `ph:"f"` with `bp:"e"` at the effect, both emitted inside their enclosing scopes so Perfetto binds them to those slices |
| Frame | `ph:"i"` instant plus a monotone `frame` counter |

Frames are also delimited visually by the top-level `"Frame"` scope, which
matters because Perfetto's dedicated frame-timeline UI is protobuf-only. That
is the one thing given up by choosing JSON, and it is a fair trade.

**Size.** ~120 bytes per event of JSON. A typical 10 s dump is 20–50 MB; a
completely full ring is 130–150 MB, which Perfetto handles. If that ever hurts:
Perfetto reads gzipped JSON, and the protobuf `TrackEvent` exporter is the
designed second exporter — **neither needs a change to the binary event
model**, which already carries stable tids, ns timestamps, interned names and
flow ids.

**Files** land in `captures/chiara-YYYYMMDD-HHMMSS.json` under the AssetSystem
user root, following the precedent that per-user writable state is not asset
content (`options.json`).

## 7. Deferred cost — the GC case, end to end

The centerpiece requirement. Flow events carry it.

Each staging batch parked behind an `EventQuery` in `AssetCache` gains a
`chiaraFlowId`. When it is parked — inside frame N's `flush-uploads` scope,
the *cause* — the code emits `FLOW_BEGIN("staging-lifetime", id)`. When it is
reclaimed in frame N+k, inside that frame's `recycle-staging` scope, the
*effect*, it emits `FLOW_END`. Perfetto draws an arrow from the frame that
created the work to the frame that paid for its cleanup. The same shape
applies to any deferred cost: an arena `Grow` to the frame whose submit
retires it, a pooled command list to its recycle.

**The honest limit.** nvrhi owns its garbage collection and does not tag
resources per-caller, so Chiara cannot attribute *inside* `runGarbageCollection`.
What it gives is the `gpu-gc` scope (when, and how much) and the flows
terminating in or near that frame (what was in flight to release) — which is
precisely the evidence chain the streaming investigation assembled by hand.
Chiara automates that chain; it does not pretend to per-resource GC
attribution.

## 8. Re-expressing what exists

| Site | Disposition |
| --- | --- |
| `Application::Run` phase brackets | **Keep.** They compute `cpuMs` and the counters; reusing them is cheaper than reading back from scopes. Scopes are added alongside for the capture detail. |
| Slow-frame `Log::Info` | **Shrink** to one line naming the frame id — "dump a capture for the breakdown". The thirteen-figure breakdown is deleted; it *is* the capture now. |
| `_cpuHistory` / `_gpuHistory` / `_frameTimeHistory` + FPS rollup | **Keep, untouched.** They feed the F11 ImPlot graphs and the Diagnostics window, which remain the live at-a-glance view. Chiara is the deep dive, not a replacement for a glance. |
| `RenderPhaseTimings` struct + brackets | **Delete.** Replaced by scopes. |
| `GetLastGpuWaitMs` + its three accumulation sites | **Keep** — the `cpuMs` formula and the graphs need it. Scopes added inside the waits. |
| `_lastGcMs` / `GetLastGcMs` | **Delete.** Its only consumer was the slow-frame log; the `gpu-gc` scope and `render/gc-ms` counter replace it. |
| `PumpPublishes` ≥2 ms log | **Delete**, re-expressed as always-on scopes and counters. This satisfies R5 of the streaming plan ("the pump diagnostic is the stability regression test's sensor"): the sensor survives, readable from a capture instead of scraped from a log. |

Frame anatomy after integration:

```
Frame                       (top-level scope; ASSISI_PROFILE_FRAME at loop top)
  input                     PollEvents + input poll
  fixed-update              the whole substep loop — N substeps sum, as decided before
  drain-main                JobSystem main queue
  update                    OnUpdate
  [pacing sleep]            unscoped: deliberate idle, excluded from cpuMs
  [vsync reconcile]         unscoped
  render
    begin-frame / scene / post-process / imgui / end-frame
  flush
```

New instrumentation landing with the same stages:

- **`SystemRegistry::RunPhase`** — a scope per phase and per system. Names are
  interned once at registration into a `const char *` on the entry; never
  `entry.name.c_str()`, whose backing `std::string` moves when the vector
  grows. At ~100 ns per system this retires the old notes' plan to gate
  per-system timing behind an open window. Note `SystemRegistry` is per-world
  now: several worlds' phases run in sequence on main, which renders correctly;
  if the tracks read ambiguously, the phase name can intern `world/phase` —
  decide by looking at a real capture, not in advance.
- **`JobSystem`** — worker indices, `RegisterCurrentThread("worker-NN")` at
  `WorkerLoop` entry (which finally gives the engine OS thread names), and
  queue-depth accessors for counters. Per-task scopes are *not* v1: tasks are
  type-erased `std::function`s with no names, and a named-task API is the v2
  seam.
- **Jolt** — `JobSystemThreadPool::SetThreadInitFunction` (present in v5.2.0)
  registers `jolt-NN`. **GpuTelemetry** — its NVML worker registers itself.

## 9. Memory and counters — v1

No `operator new` override and no callstack attribution. That is a real
profiler subsystem and pretending otherwise is how a half-tool gets built. V1
memory is byte-source counters, polled once per frame by one App-side
`PumpChiaraCounters()`, plus the one allocator hook that is nearly free:

| Track | Source |
| --- | --- |
| `mem/process-rss-bytes` | new `Core::Platform::ProcessResidentBytes()` (`/proc/self/statm`, `GetProcessMemoryInfo`); every 15 frames — it is a syscall and sub-frame cadence buys nothing |
| `mem/vram-used-bytes`, `mem/vram-total-bytes` | `GpuTelemetry` (NVML), emitted only when its `sequence` advances |
| `mem/arena-vertex-used` / `-capacity`, `mem/arena-index-*` | `GeometryArena` (gains trivial getters), emitted from the pump where they change |
| `mem/staging-parked-bytes` | sum over `AssetCache::_stagingInFlight` |
| `physics/alloc-count-per-frame`, `physics/alloc-bytes-per-frame` | replace `JPH::RegisterDefaultAllocator()` with counting wrappers. **Churn, not residency** — `JPH::FreeFunction` takes no size, so live-byte tracking would need headers that break aligned allocation. Churn is the perf-relevant signal anyway |
| `jobs/worker-queue-depth`, `jobs/main-queue-depth` | new JobSystem accessors |
| `stream/pending-publishes`, `stream/pump-bytes`, `-mesh-count`, `-mat-count` | AssetCache |
| `frame/cpu-ms`, `-gpu-ms`, `-gpu-wait-ms`, `-sleep-ms`, `-unaccounted-ms` | `Application::Run` accounting |
| `render/gc-ms`, `render/draw-calls` | VulkanContext; the mesh render system accumulates locally and emits once per frame |
| `ecs/entity-count`, `chiara/wrap-count` | pump |

`frame/unaccounted-ms` is `cpuMs − Σphases`, clamped at zero — the
descheduling discriminator, now a track you can scrub instead of a number in a
log line.

## 10. Capture control

`App::DrawChiaraPanel()` in `modules/App/src/ChiaraPanel.cpp` — App level, so
games get it and not just the editor, which is the placement argument the old
frame-profiler notes made for their widget. It needs only ImGui (via Debug,
which App already links), Chiara, and AssetSystem for the path.

Contents: recording toggle; per-ring coverage ("main: 14.2 s held"); wrap
count; *Dump 5 s / 15 s / all*; a spinner while a serialize job runs; the last
capture's path and size. Compiled out with everything else; the function has
an inline no-op stub so call sites are unconditional.

`Application` gains a `Chiara::InitGuard` as its **first declared member**,
above `_jobs`, so `Initialize` and main-thread registration happen before any
worker spawns and shutdown runs last.

## 11. Reconciliation with the deferred frame-profiler notes

**Carried over:** the `ASSISI_PROFILE_SCOPE` macro name; `SystemRegistry::RunPhase`
as the per-system chokepoint; FixedUpdate's substeps summing into one phase;
the self-measurement caveat, now quantified; the insistence that unmeasured
work be an explicit number rather than blank space ("Other (CPU)" became
`frame/unaccounted-ms`).

**Superseded:** the open question "do we adopt Tracy instead of hand-rolling?"
is answered — neither. We hand-roll only the capture side and take Perfetto's
viewer, which is the part Tracy would mostly have been for. The `ImDrawList`
budget bar, the per-phase drill-down windows, and the smoothing rules all go
with it: Perfetto is the deep dive, and the F11 ImPlot graphs remain the
glance. So does the "gate per-system timing behind an open window"
optimization — always-on scopes are cheap enough that the complexity has no
buyer.

**Stale in that doc, worth noting:** its ownership section says `SystemRegistry`
belongs to the app rather than the engine `Application`. Per-world system
binding (2026-07-28) moved it again — it is per world now.

## 12. Stages

Each stage builds green on `gd`, `gv`, `gs` (all Chiara-off, proving excision)
and `gd-c`, `gs-c` (compiled in, at both optimization extremes), keeps ctest
green, and is one commit.

0. **Docs.** This file; the superseded note atop the frame-profiler notes.
1. **Module skeleton and event core.** Rings, registry, intern, scopes,
   counters, flows, frame mark, stats; the CMake option, the nine presets, the
   `-c` targets. Tests: layout `static_assert`s, ring wrap and overwrite,
   nested and interleaved scope containment, disabled-recording emits nothing,
   eight threads emitting concurrently (also the tsan target), and one
   unguarded case proving the macros compile to no-ops in a default build.
2. **Chrome JSON serializer.** Window filtering, metadata, the whole mapping.
   Tests emit a known scene, serialize, parse with nlohmann and assert the
   shapes, flow ids and window trim. One manual drag into ui.perfetto.dev.
3. **Frame loop, threads, render.** InitGuard ordering; the loop and
   RenderFrame scopes; delete `RenderPhaseTimings`; slim the slow-frame log;
   the `frame/*` counters; JobSystem registration, depths and OS names;
   VulkanContext scopes and the `GetLastGcMs` deletion; RunPhase scopes.
   Verified by eye in Perfetto: named threads, frame slices, unaccounted track.
4. **Streaming and flows.** AssetCache scopes and counters, the pump log
   deleted, flow ids through park/recycle, arena getters. Verified by preloading
   `car_lod` and confirming the arrows cross frames.
5. **Memory and the counter pump.** `ProcessResidentBytes` and its test, the
   Jolt allocator hook, `PumpChiaraCounters`, VRAM, entity count, draw calls.
6. **Capture panel.** The panel, editor wiring, background serialize. Race test:
   serializing on a worker while main emits, tsan-clean.
7. **Docs reconcile.** Update the streaming plan's temporary-instrumentation
   block and R5; sweep §8's table; record the measured per-scope cost from a
   micro-benchmark rather than leaving §3's estimate unchecked.

## 13. Decided, not to be re-litigated

- Perfetto as the viewer; no custom viewer app.
- Off by default everywhere; `-c` targets to opt in.
- Always-on recording with dump-the-last-N, not explicit start/stop.
- Complete events at scope exit, not begin/end pairs.
- Overwrite-oldest, with wrapping counted and surfaced.
- The F11 graphs and Diagnostics window stay; Chiara does not replace the
  glance.
- No callstack or `operator new` tracking in v1.
- Per-system scopes always on, ungated.

Open, deferred to stage 6: whether the panel earns a global hotkey the way F11
did.
