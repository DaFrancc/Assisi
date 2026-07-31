# Chiara — performance and memory capture (design notes)

**Status:** Planned 2026-07-31, branch `Chiara`. Stage 0 (this document) is the
only thing built. Everything below is settled before code, deliberately — the
previous thread on this branch was abandoned because its design changed three
times mid-implementation.

Supersedes `frame-profiler-design-notes.md` (see §12) and discharges the
"instrumentation is temporary" debt recorded in `streaming-upload-perf-plan.md`.

Named for Clare of Assisi — patron saint of television, because she reportedly
saw a Mass she was too ill to attend play out on the wall of her room. Watching
something you already missed is the whole job.

## 0. Problem

The streaming-spike hunt (2026-07-24) was won with instrumentation that was
never meant to survive it: `Log::Info` calls with hard-coded thresholds in
`Application::Run`, `Application::RenderFrame`, `VulkanContext`, and
`AssetCache::PumpPublishes`. They were decisive, and they are scattered,
always-on, and answer only the question they were cut for.

Three things made them work, and any replacement that loses one is a
regression:

- **Per-phase attribution.** Which part of the frame ate the time.
- **An explicit unaccounted figure.** `cpuMs` minus the sum of the phases. When
  it is ~0 the main thread was never descheduled, which is how contention was
  ruled out — a *negative* result that saved building the wrong fix.
- **Deferred-cost visibility.** GPU allocation churn caused in frame N surfaced
  as `runGarbageCollection` cost in a later, unrelated frame. "Neither cost is
  paid where it is caused" is the sentence the whole design must answer.

## 1. Decision

**Chiara is a capture pipeline. We write no viewer and integrate no profiler.**

Instrumented code pushes fixed-size binary events into per-thread ring buffers.
On demand, a background job serializes a recent window to a trace file. The
file opens in **Perfetto** (ui.perfetto.dev) — and, because Tracy ships
`tracy-import-chrome`, in **Tracy** as well. One exporter, two viewers.

That second viewer matters: Perfetto has no capture-diff, and Tracy's Compare
window does exactly that. We get it without linking Tracy into the engine.

**Nothing third-party goes in the process.** Measured reason, not preference:
Perfetto's SDK serializes protobuf at the call site — the best independent
measurement puts it around **~600 ns per span** (thume.ca, 2023; Perfetto's own
docs publish no number). Tracy's client stores a packed POD into a thread-local
queue — **2.25 ns** in its idealized manual benchmark, **10–50 ns** realistic
for an empty zone per its contributors. The exact figures are soft; the order
of magnitude between the two shapes is not, and our binary-into-a-ring design
is the second shape. Using the Perfetto SDK for convenience would break the
performance-first rule outright.

**Compiled out unless asked, on every configuration.** `ASSISI_ENABLE_CHIARA`
defaults **OFF** for debug, dev and ship alike; every make alias gains a `-c`
variant (`gd-c`, `gv-c`, `gs-c`, …). This matters more than a config-keyed rule
would: the build worth profiling is the *optimized* one — the streaming hunt's
central methodological finding was that ~95% of a 66 ms spike was a `-O0`
artifact — so `make gs-c` has to be first-class.

### Why not Tracy as the profiler

Tracy is better than Perfetto at nearly everything on our list: frame-native
UI, Find Zone, Statistics, capture comparison, memory with callstacks,
calibrated Vulkan GPU zones. It loses on exactly one thing, and it is our
centerpiece: **Tracy has no flow events.** Issue #149 has been open since
2020-12-17 with no activity since 2022, and the maintainer's stated reason is
architectural (thread-local queues would need serializing; zone culling breaks
arrow drawing). Its fibers feature gives a fiber its own lane, not a causal
link.

Requirement "deferred cost across frames" eliminates every tool except
Perfetto. That single requirement is what carries this decision — if a year of
use shows deferred-cost attribution is a once-a-quarter question rather than a
daily one, switching to Tracy's client becomes the right call.

## 2. Placement — a module below Core

`modules/Chiara`, added before `modules/Core`, linked **PUBLIC by Core**.

The macro must be callable from Render, Physics, App — and from Core itself:
`JobSystem::WorkerLoop` is the only place worker-thread identity exists, so it
must register threads with Chiara. That forces Core → Chiara.

Chiara links **nothing** — standard library only (plus `Threads::Threads`
where `pthread_setname_np` demands it, which is not a dependency anything can
be dragged up the stack by). A module with no dependencies can never be dragged
up the stack by one.

Glue lives where its dependencies are: frame-loop scopes and the counter pump
in App, the capture panel in App (so games get it too), call-site scopes in
each module.

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
A plain option, deliberately not a `$<CONFIG>` genexp — configuration and
capture are independent decisions.

Ten configure presets: `{msvc,gcc,clang}-{debug,dev,ship}-chiara`, each
inheriting its base and flipping the cache variable, with mirrored build and
test presets — plus **`gcc-tsan-chiara`**, because the Stage 1 and Stage 6 race
tests must run under tsan *with Chiara compiled in*, and `gcc-tsan` inherits
`gcc-debug`, where Chiara is OFF. This copies the sanitizer presets exactly
(`gcc-asan` inherits `gcc-debug`), for the same reason: a separate build
directory. The define is PUBLIC, so toggling it in place would rebuild the
world on every flip.

The Makefile gains `-c` variants of every alias, configure-and-build in one
step like the sanitizer targets.

**Excision when OFF** — both .cpp files sit entirely inside
`#if defined(ASSISI_CHIARA_ENABLED)`, and the macros use the `Assert.hpp` house
pattern:

```cpp
#define ASSISI_PROFILE_SCOPE(name) ((void)sizeof(name))
```

Parsed and type-checked, never evaluated, no code emitted. Instrumentation
cannot bit-rot until someone next builds with `-c`.

There is no compiled-in-but-disabled *build*. Runtime enable/disable within a
Chiara build is a different thing and does exist — one relaxed atomic load.

## 3. Public API

```cpp
// Profile.hpp — what call sites use
ASSISI_PROFILE_SCOPE("name")           // RAII scope; name is a literal or interned pointer
ASSISI_PROFILE_FUNCTION()              // same, using __func__
ASSISI_PROFILE_ARG_STR("key", str)     // context attached to the innermost open scope
ASSISI_PROFILE_ARG_U64("key", value)
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

struct CaptureStats
{
    std::uint64_t totalEventsWritten = 0;
    std::uint64_t bufferWrapCount    = 0;   // events lost to overwrite
    std::uint32_t threadCount        = 0;
    double        mainWindowSeconds  = 0.0; // span the main ring currently covers
};

void Initialize(const Config &config = {});   // idempotent; registers caller as "main"; starts recording
void Shutdown();                              // stops recording; never frees (see §4)

void SetRecording(bool enabled);
[[nodiscard]] bool IsRecording();

void RegisterCurrentThread(const char *name); // also sets the OS thread name
                                              // (skipped for "main" — renaming the
                                              //  main thread renames the process in top/ps)

std::uint64_t MarkFrame();                    // main thread only; returns the new frame index
[[nodiscard]] std::uint64_t CurrentFrame();

[[nodiscard]] const char *InternString(std::string_view text); // cache the result; never per frame
[[nodiscard]] std::uint64_t NewFlowId();                       // never returns 0

void EmitCounter(const char *name, double value);
void EmitArgString(const char *key, std::string_view value);
void EmitArgU64(const char *key, std::uint64_t value);
void EmitFlowBegin(const char *name, std::uint64_t flowId);
void EmitFlowEnd(const char *name, std::uint64_t flowId);

/// Async spans for work that starts on one thread and finishes on another —
/// job continuations, streaming loads. Distinct from ScopeTimer because these
/// are NOT stack-disciplined; forcing them into a thread's scope stack is a
/// mistake every profiler that tried it had to undo.
std::uint64_t BeginAsync(const char *name);
void EndAsync(const char *name, std::uint64_t asyncId);

[[nodiscard]] CaptureStats GetCaptureStats();

class ScopeTimer { /* see §4 — complete event written at destruction */ };
class InitGuard  { /* Initialize on construct, Shutdown on destruct */ };
}
```

Every function gets an inline empty stub in the `#else` branch, so glue code
never needs an `#ifdef`.

**Before `Initialize`, every entry point is a safe no-op** — the macros,
`RegisterCurrentThread`, all of it. This is load-bearing, not defensive
politeness: Core's `JobSystem` runs in headless tests and hosts with no
`Application` and no `InitGuard`, and a `-c` build of those tests must behave
exactly like a default build — no crash, no auto-initialization, no events.

## 4. Event model

```cpp
enum class EventType : std::uint8_t
{
    Scope = 0,      // timestampTicks = begin; payload = duration in ticks
    Counter = 1,    // payload = bit_cast<uint64_t>(double)
    FlowBegin = 2,  // payload = flow id
    FlowEnd = 3,
    FrameMark = 4,  // payload = frame index
    ArgString = 5,  // payload = interned string pointer/id; binds to enclosing scope
    ArgU64 = 6,     // payload = the value
    AsyncBegin = 7, // payload = async id
    AsyncEnd = 8,
    ClockSnapshot = 9, // payload = CLOCK_MONOTONIC_RAW ns matching timestampTicks
    Instant = 10,   // reserved — no v1 macro emits it; one lands with the first
                    // call site that needs a point-in-time marker
};

struct Event
{
    std::uint64_t timestampTicks; // raw TSC; converted at serialize time
    std::uint64_t payload;
    const char   *name;           // interned; dereferenced only when serializing
    std::uint8_t  type;
    std::uint8_t  reserved0;
    std::uint16_t reserved1;
    std::uint32_t reserved2;
};
static_assert(sizeof(Event) == 32);
```

**Raw TSC ticks, converted at dump.** Cheaper per event than `steady_clock`,
and it is what the eventual FTF export wants natively (§6). Use bare `rdtsc`
with no fence on x86-64, `CNTVCT_EL0` on ARM64, with
`clock_gettime(CLOCK_MONOTONIC_RAW)` as the fallback. Gating is **two checks,
not one**: the invariant-TSC CPUID bit guarantees constant *rate*, not
cross-socket or VM synchronization — the kernel validates sync separately and
demotes TSC when it fails — so on Linux additionally require
`/sys/devices/system/clocksource/clocksource0/current_clocksource` to read
`tsc`, and take the fallback otherwise. Frequency is calibrated against
`clock_gettime` on **both** architectures — CPUID leaf 0x15 is absent or wrong
too often on x86, and ARM's `CNTFRQ_EL0` can lie — and the ~1 Hz clock
snapshots below *are* that calibration data, refreshed for free.

**Clock snapshots** pairing TSC with `CLOCK_MONOTONIC_RAW`, emitted ~1 Hz.
Free, and the only way GPU timestamps and OS traces stay correlatable later.
`MONOTONIC_RAW` specifically: NTP adjusting the host clock is a documented
cause of CPU/GPU drift in other profilers.

**Scopes are one event, written at destruction** — begin plus duration. Half
the pushes of a begin/end pair, and it handles help-waiting for free: when
`Task::Wait` runs other tasks inline, the inner scopes are wall-clock-contained
inside the waiting scope, which is what a complete-event nesting renders, and
it is the truth.

The known cost of complete events is that a scope still open at dump time
produces nothing — the hang case. Handled by a **live shadow stack** per
thread (depth, name, begin tick) kept *outside* the ring; at serialize time it
is walked and synthesized into "still open at window end" slices. That gets the
hang case without doubling every event.

The shadow stack is live while the serializer reads it — scope destructors keep
running whether or not recording is paused, or the stack desyncs — so it gets
the same rigor as the ring: each thread's stack carries a **generation counter
bumped before and after every push and pop** (a seqlock). The walker snapshots
the stack, re-reads the generation, and retries on mismatch or an odd value.
No torn synthesis, no lock on the emit path.

**Context goes in args, never in the name.** The name is the aggregation key —
`publish-mesh` — and the asset path is an `ArgString` bound to it. Folding the
path into the name shatters cross-frame aggregation into thousands of singleton
buckets, which is the single most common way this kind of system is ruined.
It is also what makes "click a slice, see the asset" work in Perfetto with no
custom tooling.

**Names are interned pointers, never copied.** Macro sites pass literals, whose
lifetime is the program. Dynamic names go through `InternString` once and are
cached by the caller.

**Per-thread rings, single-producer.** A `thread_local ThreadBuffer *`, a
power-of-two event array, a monotonic write cursor incremented with release
*after* the slot store. The registry is mutex-guarded; the emit path takes no
lock — with **one deliberate exception**: `EmitArgString` with a dynamic value
must intern it at emit time, and interning is mutex-guarded. That is
acceptable exactly because arg-carrying events are rare by design (asset
publishes, not per-entity work); any hot site that cares pre-interns and
caches. Every other emit is lock-free, and no scope, counter, or flow ever
takes the intern lock. First emit from an unregistered thread auto-registers
it as `thread-<n>`.

Fixed-size records are why per-event overwrite is safe here: there is no
variable-length framing to tear, unlike the chunked formats (LTTng, Perfetto)
whose designs require chunk-granularity overwrite.

**Drop policy: overwrite oldest.** The spike being chased is the recent one.
Wrapping is counted and per-ring coverage is reported in `CaptureStats`, so a
too-small buffer is visible rather than silently truncating history.

**Sizing.** 32 MiB main ring = 1,048,576 events. At a heavy ~350 events/frame:
~21 s at 144 fps, ~50 s at 60 fps. Fifteen workers at 2 MiB each is hours.
Both numbers are in `Config`. Perfetto's rule of thumb generalizes:
`window_seconds = buffer_bytes / write_rate`.

**Buffers and the intern table are never freed.** Standard profiler pattern; it
makes late-thread teardown ordering (Jolt's pool, the NVML worker, static
destructors) a non-issue. `Shutdown()` only stops recording. Keep the string
table in a non-wrapping arena so interning is never evicted.

## 5. Capture model — always recording, dump the last N

Recording starts at `Initialize` and runs continuously. By the time you decide
something is worth recording, the spike already happened. The panel offers
*dump the last 5 s / 15 s / everything*, plus a pause toggle for A/B
experiments; explicit start/stop is the same mechanism and needs no extra API.

`SerializeCapture(path, lastSeconds)` pauses recording, walks each ring within
the window, streams the trace through a buffered `ofstream`, resumes, and
returns counts. It runs on a JobSystem worker with a `Then(Main, …)`
continuation for panel status, so the frame never hitches on a dump. Capture is
blind while it writes — acceptable, because a dump happens after the
interesting part.

A thread mid-push when its cursor `C` is sampled can complete at most one
straggler past the observed cursor — and that straggler lands in slot
`C mod capacity`, which in a full ring is the *oldest* slot. So the reader
takes **`[C − capacity + 1, C − 1]`**, sacrificing one slot per ring, and can
never observe a torn event. The obvious `[C − capacity, C − 1]` is wrong
precisely on the full-ring "dump everything" path; this is tested in Stage 1
with a writer racing the reader.

## 6. Export format

**Chrome JSON in v1. Fuchsia Trace Format (FTF) when size hurts. Never
protobuf.**

- **Chrome JSON** — ~150 lines of *emission* (the serializer's real work is
  elsewhere — see below), documented by Perfetto as supporting flow events as
  connecting arrows, and read by the widest set of tools (Perfetto, Tracy via
  `tracy-import-chrome`, Firefox Profiler, others). ~120 bytes/event: a 10 s
  dump is 20–60 MB, a full ring 130–150 MB, within Perfetto's browser budget —
  and `trace_processor server http trace.json` removes even that ceiling.
- **FTF** is the natural second exporter and is close to the binary format we
  were going to invent anyway: 64-bit words, interned strings, flows, counters,
  typed args, and an initialization record carrying `ticks_per_second` — so
  **raw TSC ticks go out with no conversion pass**. 3–5× smaller. Perfetto
  auto-detects it by magic bytes; Tracy imports it too via
  `tracy-import-fuchsia` (dropping flows). Caps to know: 32,767 interned
  strings, 255 table-indexed threads — and dynamic arg *values* (asset paths)
  are what would exhaust the string table in a long session, so the FTF
  exporter must count interned strings and warn, never truncate silently. A
  windowed dump makes this mostly theoretical.
- **Protobuf is rejected.** The reason anyone would reach for it — Perfetto's
  real frame-timeline UI — is **Android-only**: `frame_timeline_event.proto`
  requires SurfaceFlinger display-frame tokens, layer names, and Choreographer
  jank classifications. A desktop app cannot get it, and switching formats does
  not change that. Protobuf is also the hardest to hand-roll.

Frame-shaped analysis is served by ordinary slices named `Frame` plus
Perfetto's SQL, which works identically from JSON and FTF.

### Mapping (Chrome JSON)

| Chiara | JSON |
| --- | --- |
| File | `{"displayTimeUnit":"ms","traceEvents":[…]}` |
| Metadata | `ph:"M"` `process_name`, per-thread `thread_name`, `thread_sort_index` (main first) |
| Scope | `ph:"X"`, `ts`/`dur` in fractional µs (three decimals keeps ns precision) |
| Args | folded into the owning `X` event's `args` object |
| Counter | `ph:"C"`, `args:{"v":…}`; `group/name` is expected to group the tracks — unverified, confirm in the Stage 2 smoke test |
| Flow | `ph:"s"` at the cause, `ph:"f"` with `bp:"e"` at the effect, both inside their enclosing scopes |
| Async | `ph:"b"`/`ph:"e"` with `id` **and a constant `cat:"async"`** — Chrome JSON pairs async events by `(cat, id)`, so the `cat` field is load-bearing, not decoration |
| Frame | `ph:"i"` instant plus a monotone `frame` counter |

**The serializer's real work is not the JSON — it is arg binding.** Args land
in the ring *before* their enclosing scope's complete-event, because the scope
is written at destruction. Folding args into their owning `X` event means
rebuilding each thread's scope tree at serialize time: sort by begin tick,
bind by interval containment, innermost wins. The edge cases are the point —
an arg whose scope is still open binds to the shadow-stack synthesis; an arg
whose scope began before the window is dropped and counted. This is the hard
part of Stage 2 and is tested as such; the ~150 lines above describe only the
emission.

**What `tracy-import-chrome` actually does** (verified from its source): `X`,
`B/E`, `b/e`, `i`, `C` and `M` thread names all import; **args become zone
text**, so asset paths survive into Tracy; counters become plots; flow `s`/`f`
events are silently ignored, as expected. Tracy byte-formats plots whose names
end `_bytes` — ours end `-bytes`; if that formatting ever matters, rename at
export, not in the event model.

**Files** land in `captures/chiara-YYYYMMDD-HHMMSS.json` under the AssetSystem
user root (the `options.json` precedent: per-user writable state is not asset
content).

### What Perfetto actually gives us (verified)

- **Worst frames, click-through.** `SELECT id, ts, dur FROM slice WHERE
  name='Frame' ORDER BY dur DESC LIMIT 20` — query results including `id`
  link into the timeline; clicking jumps to the slice.
- **Self vs total time.** stdlib `slices.self_dur` → `slice_self_dur(id, self_dur)`.
  Also `slices.hierarchy`, `slices.flat_slices`, `slices.with_context`.
- **Flow traversal, not just arrows.** stdlib `slices.flow` exposes
  `_slice_following_flow` — transitive reachability over the flow graph. "Given
  these alloc slices in frame N, find everything downstream" is a query. The UI
  draws only immediate neighbours; the chain lives in SQL.
- **Area-selection aggregation** — drag the timeline, get a pivot table over
  those tracks/timestamps. Still behind a feature flag in stable; press `p`.
- **Debug tracks** — any SQL result with ts/dur/name becomes a timeline track,
  optionally pivoted per distinct value. The cheap path to engine-aware views
  without forking anything.
- **Deep linking** — `postMessage` an ArrayBuffer plus `visStart`/`visEnd`/
  `query`/`startupCommands` URL params. The engine could open a capture
  pre-zoomed to the worst frame with our queries loaded.
- **No capture diff.** Covered by Tracy's Compare window on the same file.
  Secondary options: Trace Summarization metrics via
  `trace_processor_shell summarize` for scripted regression detection, or
  merging two traces onto one timeline.

**Open spike (30 minutes, before committing to FTF):** hand-write ~20 FTF
records with one flow and confirm the arrows render in ui.perfetto.dev. The
parser is confirmed to handle `kFlowBegin/Step/End`; end-to-end UI rendering is
not. Chrome JSON is the documented fallback and costs nothing.

## 7. Deferred cost — the GC case, end to end

Flow events carry it. Each staging batch parked behind an `EventQuery` in
`AssetCache` gains a `chiaraFlowId`. Parked inside frame N's `flush-uploads`
scope — the *cause* — it emits `FLOW_BEGIN("staging-lifetime", id)`. Reclaimed
in frame N+k inside `recycle-staging` — the *effect* — it emits `FLOW_END`.
Perfetto draws the arrow. Same shape for an arena `Grow` to the frame whose
submit retires it.

**The honest limit:** nvrhi owns its garbage collection and does not tag
resources per caller, so Chiara cannot attribute *inside* `runGarbageCollection`.
What it gives is the `gpu-gc` scope (when, how much) and the flows terminating
near that frame (what was in flight) — precisely the evidence chain the
streaming investigation assembled by hand.

## 8. Re-expressing what exists

| Site | Disposition |
| --- | --- |
| `Application::Run` phase brackets | **Keep.** They compute `cpuMs`; reusing them beats reading back from scopes. Scopes added alongside. |
| Slow-frame `Log::Info` | **Shrink** to one line naming the frame id — "dump a capture for the breakdown". |
| `_cpuHistory` / `_gpuHistory` / `_frameTimeHistory` + FPS rollup | **Keep, untouched.** They feed the F11 ImPlot graphs and Diagnostics window, which stay the live at-a-glance view. Chiara is the deep dive, not a replacement for a glance. |
| `RenderPhaseTimings` struct + brackets | **Delete.** Replaced by scopes. |
| `GetLastGpuWaitMs` + 3 accumulation sites | **Keep** — the `cpuMs` formula and the graphs need it. Scopes added inside the waits. |
| `_lastGcMs` / `GetLastGcMs` | **Delete.** Only consumer was the slow-frame log; `gpu-gc` scope + `render/gc-ms` counter replace it. |
| `PumpPublishes` ≥2 ms log | **Delete**, re-expressed as always-on scopes and counters. This satisfies R5 of the streaming plan (the pump diagnostic is the regression sensor): the sensor survives, readable from a capture instead of scraped from a log. |

Frame anatomy after integration:

```
Frame                       (top-level scope; ASSISI_PROFILE_FRAME at loop top)
  input                     PollEvents + input poll
  fixed-update              the whole substep loop — N substeps sum
  drain-main                JobSystem main queue
  update                    OnUpdate
  [pacing sleep]            unscoped: deliberate idle, excluded from cpuMs
  [vsync reconcile]         unscoped
  render
    begin-frame / scene / post-process / imgui / end-frame
  flush
```

New instrumentation landing with the same stages:

- **`SystemRegistry::RunPhase`** — a scope per phase and per system. Names
  interned once at registration into a `const char *` on the entry; never
  `entry.name.c_str()`, whose backing string moves when the vector grows. At
  ~100 ns/system this retires the old notes' plan to gate per-system timing
  behind an open window. **This is the chokepoint that makes instrumentation
  feel automatic** — every system written from then on is profiled with no
  further work, which is how Unreal's coverage actually works (framework
  chokepoints, not function reflection). Note `SystemRegistry` is per-world
  now; several worlds' phases run in sequence on main, which renders correctly.
  If tracks read ambiguously, intern `world/phase` — decide from a real
  capture, not in advance.
- **`JobSystem`** — worker indices, `RegisterCurrentThread("worker-NN")` at
  `WorkerLoop` entry (finally giving the engine OS thread names), queue-depth
  accessors. Per-task scopes are *not* v1: tasks are type-erased
  `std::function`s with no names. A named-task API is the v2 seam, and it is
  what `BeginAsync`/`EndAsync` exist to serve.
- **Jolt** — `JobSystemThreadPool::SetThreadInitFunction` (present in v5.2.0)
  registers `jolt-NN`. **Trap, known in advance:** the header requires the
  function be set *before* `Init`, and `JoltRuntime` currently constructs the
  pool with its thread-starting constructor in a member initializer — so
  set-after-construct compiles and silently does nothing. The runtime
  restructures slightly: default-construct the pool, set the init function,
  then `Init` in the constructor body. **GpuTelemetry** — its NVML worker
  registers itself.

## 9. Memory and counters — v1

No `operator new` override and no callstack attribution. That is a real
subsystem, and pretending otherwise is how half-tools get built. V1 memory is
byte-source counters polled once per frame by an App-side
`PumpChiaraCounters()`, plus the one allocator hook that is nearly free:

| Track | Source |
| --- | --- |
| `mem/process-rss-bytes` | new `Core::Platform::ProcessResidentBytes()`; every 15 frames — it is a syscall |
| `mem/vram-used-bytes`, `-total-bytes` | `GpuTelemetry` (NVML), emitted when its `sequence` advances |
| `mem/arena-vertex-used` / `-capacity`, `mem/arena-index-*` | `GeometryArena` (gains trivial getters) |
| `mem/staging-parked-bytes` | sum over `AssetCache::_stagingInFlight` |
| `physics/alloc-count-per-frame`, `-bytes-per-frame` | replace `JPH::RegisterDefaultAllocator()` with counting wrappers — **all five hooks** (`Allocate`, `Reallocate`, `AlignedAllocate`, `AlignedFree`, `Free`), counting with relaxed atomics because Jolt allocates from its workers, installed before the pool and temp allocator construct. **Churn, not residency** — `JPH::FreeFunction` takes no size, so live-byte tracking needs headers that break aligned allocation. Churn is the perf-relevant signal anyway |
| `jobs/worker-queue-depth`, `jobs/main-queue-depth` | new JobSystem accessors |
| `stream/pending-publishes`, `-pump-bytes`, `-mesh-count`, `-mat-count` | AssetCache |
| `frame/cpu-ms`, `-gpu-ms`, `-gpu-wait-ms`, `-sleep-ms`, `-unaccounted-ms` | `Application::Run` accounting |
| `render/gc-ms`, `render/draw-calls` | VulkanContext; mesh render system accumulates locally, emits once per frame |
| `ecs/entity-count`, `chiara/wrap-count` | pump |
| `ecs/components/<Type>` | runtime loop over `ComponentRegistry` — see below |

`GpuTelemetry` is owned by `EditorApp` today; in Stage 5 it moves to
`Application` and the editor reads through it — one NVML worker, not two.

`frame/unaccounted-ms` is `cpuMs − Σphases` — **with `gpuWait` subtracted from
the render phase before summing**. All three `_lastGpuWaitMs` accumulation
sites sit inside `RenderFrame`'s callees, so the raw render bracket contains
the wait while `cpuMs` already excludes it; summing raw phases biases the
figure low by the full wait, and under VSync (where the wait is large every
frame) would pin a clamped track to zero and hide real descheduling. Emitted
**unclamped**, keeping the old log's honesty: a persistently negative track
means the accounting itself is wrong, which is worth seeing. The descheduling
discriminator, now a track you scrub instead of a number in a log line.

**Per-component counters need no codegen.** `ComponentRegistry` already
enumerates every registered meta with a stable name, and
`Scene::ComponentCount(ComponentId)` exists — a runtime loop in the pump (names
interned once, cached) gives one counter per component type and stays correct
as components are added. An earlier draft had reflectgen generate this;
generation buys nothing the loop doesn't. (An `AFUNC()` that auto-instruments
functions remains impossible either way: reflectgen emits side files and
cannot inject into a function body, and routing calls through a generated
thunk to time them would cost more than it measures.)

## 10. Capture control

`App::DrawChiaraPanel()` in `modules/App/src/ChiaraPanel.cpp` — App level so
games get it, not just the editor. Needs only ImGui (via Debug, which App
links), Chiara, and AssetSystem for the path.

Contents: recording toggle — **disabled while a dump is in flight**, since the
serialize job and the toggle flip the same recording flag; per-ring coverage
("main: 14.2 s held"); wrap count; *Dump 5 s / 15 s / all*; a spinner while a
serialize job runs; the last capture's path and size. Compiled out with everything else; an inline no-op
stub keeps call sites unconditional.

`Application` gains a `Chiara::InitGuard` as its **first declared member**,
above `_jobs`, so Initialize and main-thread registration happen before any
worker spawns and shutdown runs last.

## 11. Not building a viewer (and what would change that)

The ImGui timeline-widget ecosystem is effectively empty: the best flame-graph
widget is 148 lines with no zoom, pan, or click; the best small profiler widget
has no depth field, so no nesting. A real zoomable trace timeline is
~1,500–2,500 lines minimum (Valve's `gpuvis_graph.cpp`, MIT and worth reading,
is 4,777). Godot 4.6 and Bevy both integrate Tracy/Perfetto and shipped no
viewer of their own.

The one thing a custom tool would uniquely buy is **engine-aware navigation** —
click a slice, select that entity in *our* editor. Perfetto can display the
data as args and pivot debug tracks by it, but cannot link back into the editor.

If it is ever built, the viewer does not need to be C++ — it is a dev tool, not
a runtime. Python for anything CLI or analysis-shaped; HTML/JS if it needs a UI
(which also gives a double-click-to-open artifact and sidesteps picking a GUI
toolkit). The capture format being ours is what keeps that open.

**Revisit after six months of real use, when three missing views can be named.**
Things to steal then: puffin's `ProfileView` (recent-frames deque plus two
parallel sorted sets, so worst frames are queryable by cost and renderable in
time order); Godot's Category→Item two-level model; Unreal Insights'
selection-range → aggregation coupling; and **max-reduce, never mean, when the
frame graph zooms out** — mean-reduction silently destroys worst-frame hunting.

**Free complement, adopt regardless:** `samply` — sampling profiler, one
command, no integration, runs on an unmodified binary, Firefox Profiler UI.
It covers exactly Chiara's blind spot: code nobody instrumented. Also
`heaptrack` for allocator ground truth, and Nsight/RGP when GPU work gets deep.

## 12. Reconciliation with the deferred frame-profiler notes

**Carried over:** the `ASSISI_PROFILE_SCOPE` name; `SystemRegistry::RunPhase`
as the chokepoint; FixedUpdate substeps summing into one phase; the
self-measurement caveat; the insistence that unmeasured work be an explicit
number ("Other (CPU)" became `frame/unaccounted-ms`).

**Superseded:** "do we adopt Tracy instead of hand-rolling?" is answered —
neither, we hand-roll capture and take Perfetto's viewer (plus Tracy's as a
free importer). The `ImDrawList` budget bar, the drill-down windows and the
smoothing rules go with it. So does gating per-system timing behind an open
window — always-on scopes are cheap enough that the complexity has no buyer.

**Stale in that doc:** its ownership note says `SystemRegistry` belongs to the
app rather than `Application`. Per-world system binding (2026-07-28) moved it
again — it is per world now.

## 13. Stages

Each stage builds green on `gd`, `gv`, `gs` (Chiara off, proving excision) and
`gd-c`, `gs-c` (compiled in, both optimization extremes), keeps ctest green,
and is one commit.

0. **Docs.** This file; the superseded note atop the frame-profiler notes.
   *(Done — commit `c8f76d4`; amended twice, after the tooling research and
   after an adversarial external review.)*
1. **Module skeleton and event core.** Rings, registry, intern, scopes, args,
   counters, flows, async spans, frame marks, clock snapshots, shadow stack
   with its generation counter, stats; the CMake option, the ten presets
   (including `gcc-tsan-chiara`), `-c` targets. Tests: layout `static_assert`s,
   ring wrap and overwrite, the one-slot sacrifice with a writer racing the
   reader, nested and interleaved scope containment, args binding to the
   enclosing scope, every entry point a safe no-op before `Initialize`,
   disabled-recording emits nothing, eight threads emitting concurrently (also
   the tsan target), and one unguarded case proving the macros compile to
   no-ops in a default build.
2. **Chrome JSON serializer.** Window filtering, metadata, the full mapping,
   scope-tree reconstruction for arg folding, shadow-stack synthesis of
   still-open scopes. Tests emit a known scene, serialize, parse with nlohmann,
   assert shapes, arg folding (including an arg whose scope is still open and
   one whose scope began outside the window), flow ids and window trim.
   Manual: one drag into ui.perfetto.dev (also confirming counter tracks group
   by `/`), and one round-trip through `tracy-import-chrome`.
3. **Frame loop, threads, render.** InitGuard ordering; loop and RenderFrame
   scopes; delete `RenderPhaseTimings`; slim the slow-frame log; `frame/*`
   counters; JobSystem registration, depths and OS names; VulkanContext scopes
   and the `GetLastGcMs` deletion; RunPhase scopes. Verified by eye in
   Perfetto: named threads, frame slices, unaccounted track.
4. **Streaming and flows.** AssetCache scopes, args (asset path as an arg, not
   a name) and counters; pump log deleted; flow ids through park/recycle; arena
   getters. Verified by preloading `car_lod` and confirming arrows cross frames.
5. **Memory and the counter pump.** `ProcessResidentBytes` + test, the
   five-hook Jolt allocator wrappers, `PumpChiaraCounters`, VRAM (GpuTelemetry
   moves to `Application`, editor reads through it), entity count, draw calls,
   and the `ComponentRegistry` per-component counters.
6. **Capture panel.** Panel, editor wiring, background serialize. Race test:
   serializing on a worker while main emits, tsan-clean.
7. **Docs reconcile.** Update the streaming plan's temporary-instrumentation
   block and R5; sweep §8's table; record measured per-scope cost from a
   micro-benchmark rather than leaving an estimate unchecked.

## 14. Decided, not to be re-litigated

- Perfetto as primary viewer; Tracy as a free second viewer via
  `tracy-import-chrome`; no custom viewer; nothing third-party linked in.
- Chrome JSON now, FTF later, protobuf never.
- Off by default on every configuration; `-c` targets to opt in.
- Always-on recording with dump-the-last-N, not explicit start/stop.
- Complete events at scope exit, plus a shadow stack for still-open scopes.
- Raw TSC ticks stored, converted at dump; clock snapshots at ~1 Hz.
- Context in args, never in the scope name.
- Async spans as a distinct type from stack-disciplined scopes.
- Overwrite-oldest, with wrapping counted and surfaced.
- The F11 graphs and Diagnostics window stay; Chiara does not replace the
  glance.
- No callstack or `operator new` tracking in v1.
- Per-system scopes always on, ungated.

Open, deferred to stage 6: whether the panel earns a global hotkey like F11.
