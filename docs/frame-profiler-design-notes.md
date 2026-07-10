# Frame profiler — design notes (deferred)

**Status:** Deferred (2026-07-10). Not scheduled. Captured so the thinking isn't lost.

## Why deferred

The initial ask was small — "show how long each phase takes." Discussion revealed
the *actually useful* version is closer to Unreal Insights: an instrumentation API
with scoped timers, per-thread tracks, ring-buffered history, and a real zoomable
timeline UI. That's a subsystem, not a widget. A cut-down version is buildable, but
past a certain point a half-profiler is worse than none (nobody trusts a number they
can't drill into). Parking it until we can plan it properly and give it appropriate
scope.

The narrower "per-phase budget bar" below is still a legitimate, much smaller first
step if we want *something* before the full system — but only if we consciously accept
it as a stopgap.

---

## The long-term vision (Unreal Insights-like)

- A macro-based scoped-timer instrumentation API (`ASSISI_PROFILE_SCOPE("name")`)
  that any code — engine or game — can drop in, not just registered systems.
- Hierarchical/nested scopes (a scope inside a scope), so drill-down is inherent
  in the data rather than a special per-system case.
- Per-thread tracks (once we have worker threads / Jolt job threads worth showing).
- Ring-buffered history so you can scrub back to a spike frame, not just see "now."
- A zoomable timeline UI (pan/zoom, hover for exact ns, click to expand a scope).
- GPU timeline alongside CPU (we already have per-frame GPU timer-query time; a real
  version wants per-pass GPU timestamps).

This is the thing worth building. Everything below is the small version.

---

## The small version: per-phase budget bar (if we ever want a stopgap)

A one-row horizontal bar = a budget timeline of the frame's CPU phases.

### Concept
- Phases run **sequentially on the main thread** (PreUpdate → FixedUpdate → Update →
  PostUpdate → Render), so stacking segments left→right by duration *is* a real
  timeline; width ∝ duration.
- Full bar width = a **frame budget** (configurable; default 16.6 ms = 60 fps, with
  60/120/144 presets). We run uncapped, so there's no natural budget otherwise.
- **Under budget:** phases fill part of the bar; blank space to the right = headroom.
  Budget line sits at the right edge.
- **Over budget:** expand the domain to `max(budget, total)`; the budget value becomes
  an interior **red dashed vertical line**, phases overflow past it.

### The "Other (CPU)" segment (decided: include it)
The five phases only cover work inside registered systems. `cpuFrameMs` (already
measured — isolated CPU cost, minus sleep and GPU-wait) is larger, because a lot of
CPU work isn't in any system. Add a trailing segment:

    Other = clamp(cpuFrameMs - Σ(phase times), >= 0)

so the colored segments sum to the *real* CPU frame cost and the blank tail is
*honest* idle headroom (not "unmeasured work misread as slack").

Caveat to preserve: "Other" is not contiguous in time — it's slivers scattered
before/between/after phases. We draw a **budget/proportion bar, not a wall-clock
timeline**, so a single trailing "Other" block is an honest *total* as long as the UI
doesn't imply it all happens last.

### Where "Other" CPU actually goes (frame anatomy)
From `Application::Run` / `Application::RenderFrame` (as of 2026-07-10):

    PollEvents + input->Poll          input / OS events (spiky)
    OnFixedUpdate (xN)                PHASE FixedUpdate
    OnUpdate                          PHASES PreUpdate/Update/PostUpdate
    [FPS-limit sleep]                 excluded from cpuMs
    SetVSync reconcile                ~free
    RenderFrame:
      BeginFrame                      acquire + query resolve (GPU wait excluded)
      clear color/depth               command recording
      OnRender                        PHASE Render (RunRender)
      PostProcess.Resolve             AA command recording
      DebugUI::BeginFrame + OnImGui   ImGui build (incl. the profiler panel itself)
      DebugUI::EndFrame               ImGui render-command emit
      EndFrame                        vkQueueSubmit + present
    _events.Flush                     event dispatch

Two buckets dominate "Other": **ImGui build+emit** (largest; grows with how much debug
UI is open) and **Submit/Present** (`EndFrame`). Smaller/spikier: input polling, stray
command glue.

- **Self-measurement gotcha:** the profiler panel (ImPlot plots etc.) adds real CPU to
  "Other," so the tool inflates what it reports. Label it; don't try to hide it.
- **Start lumped** as one "Other (CPU)". Leave a clean seam to later peel off exactly
  two named sub-buckets — **"UI"** and **"Present"** — since those are the only big,
  stable, recognizable ones. Don't split until the lumped number proves it's worth it.

### Tiny-phase visibility & interaction (decided approach)
A 0.05 ms phase next to a 3 ms one is sub-pixel: invisible and unclickable. Don't
distort widths to fix it (that breaks the graph). Instead:
- **Legend/table is the primary interaction surface, not the bar.** One row per phase +
  Other: color swatch, exact ms, %. Clicking a legend row opens that phase's per-system
  drill-down window. Guarantees every phase is visible and clickable regardless of
  segment width.
- Keep bar proportions exact, but give each nonzero phase a **min ~1–2 px sliver** so it
  never fully vanishes.
- **Thin boundary ticks** between segments so you can see there are N phases even when
  some are hairlines.
- **Hover tooltip** on hittable segments: name + ms + %.
- Net: bar = at-a-glance "over budget?" visual; legend = reliable readout + click target.
  Clicking a fat segment *or* its legend row opens the drill-down.

### Per-system drill-down
Clicking a phase (segment or legend row) opens a window listing that phase's systems by
time (its own mini budget bar). **Gate per-system measurement behind "is any drill-down
window open"** so we don't pay a `clock` read per system every frame when nobody's
looking. Per-phase timing stays always-on (≈5 reads/frame, free).

### Locked implementation decisions
- **Rendering:** custom `ImDrawList` (colored rects + manual dashed budget line +
  per-segment `InvisibleButton`/hit-testing), **not** ImPlot. Full control over the
  over-budget domain expansion and the click regions.
- **Reusable widget**, not sandbox-only. Something like
  `DrawPhaseTimeline(registry, budgetMs, cpuFrameMs)`. Must live where it's allowed to
  depend on `SystemRegistry` (App module — Debug is lower-level than App). This fits the
  engine-as-template model: every game gets it.
- **Smoothing:** display smoothed widths (reuse the CPU/GPU rolling average, or an EMA).
  Raw per-frame is unreadable at high FPS. Optional faint raw ghost behind — skip
  initially.
- **Label CPU-only:** "Render" here is command *recording*, not GPU execution. Don't let
  it be confused with GPU load. (Existing per-frame GPU timer already covers GPU.)

### Optional stretch
A second stacked row under the CPU bar for **GPU** against the same domain (CPU phases on
top, GPU time below) → at-a-glance CPU-vs-GPU-bound answer. Build the CPU row first with a
clean seam for it.

### Data-model shape this implies
- `SystemRegistry`: time each `RunPhase` call; **accumulate per phase** for the current
  frame (so FixedUpdate's N substeps sum). Add `ResetFrameTimings()` the loop calls at
  frame top. Expose `PhaseMs(SystemPhase)` / `RenderMs()`. Per-system: a gated `name→ms`
  map behind the open-window flag.
- App loop: call the reset at frame top; feed `cpuFrameMs` into the widget for the
  "Other" segment; draw the reusable widget.
- Timing chokepoint is already ideal: **all phases (game + render) funnel through the one
  `SystemRegistry::RunPhase`** (`modules/App/src/SystemRegistry.cpp`).

### Ownership note
`SystemRegistry` is owned by the **app** (`SandboxApp._systems`), not the engine
`Application` base — so the base can't see it. Mirror the existing GPU-timer pattern:
the producer (`SystemRegistry`) self-times and exposes accessors; something up the stack
reads and draws. The reusable widget takes the registry as a parameter.

---

## Open questions for the real version (when we plan it properly)
- Instrumentation API surface: macro scopes vs manual begin/end; how nesting is stored.
- Storage: fixed ring buffer size, per-frame vs continuous; memory budget.
- Threads: do we need multi-track now, or is main-thread-only fine until Jolt jobs matter?
- GPU: per-pass timestamps (needs NVRHI timer-query-per-pass plumbing) vs the current
  single per-frame GPU time.
- Overhead + gating: profiling must be near-free when the UI is closed.
- Do we adopt an existing lib (Tracy) instead of hand-rolling? Tracy is the obvious
  "don't reinvent Insights" answer — worth evaluating before building anything custom.
