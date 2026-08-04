# R6 — Adversarial Review: Codebase Verification & Coherence Pass

*Angle: verify every codebase claim in r3/r4 against the actual source on branch
`asset-upgrade`, then stress-test the combined recommendation set for internal
coherence and fit with Assisi's stated constraints (performance-first, editor/game
module split, JobSystem threading). Verdicts are CONFIRMED / REFUTED / OVERSTATED
per claim, then adopt / adopt-modified / defer / reject per recommendation.*

---

## PART 1 — ECS claims (r3 §5.1, r4 §A2)

### 1.1 SparseSet change-tick lane — **CONFIRMED, in full**

`modules/ECS/include/Assisi/ECS/SparseSet.hpp`:
- Parallel `_changeTicks` lane, `SetTracksChanges` (172-179), `Stamp` (186-190),
  `ChangeTick` (194-199), `TracksChanges` (182). All present as described.
- Swap-remove keeps the lane in lockstep: `Remove` moves `_changeTicks[removedPos]
  = _changeTicks[lastPos]` (94-97) and pops (102-103). r4's claim exact.
- Untracked pools keep `_changeTicks` empty (69, 176-178) → genuinely zero-cost
  when off. Confirmed.
- Test exists and is real: `modules/ECS/tests/TestChangeDetection.cpp` has
  `"swap-remove keeps ticks aligned"` (verified the body: gives `b` a newer tick,
  removes `a`, asserts `b`'s tick survives the swap) and a `Get`-does-not-stamp
  case. r4's "tested and shipped" is **CONFIRMED** (and the `.o` artifacts across
  seven build configs confirm it actually compiles into the test binary).

`Scene.hpp`: `_changeTick` counter (375), `Add` stamps (137-138), `GetMut` stamps
(157-166), non-const `Get` deliberately does **not** stamp (147-151, with the exact
warning comment r4 quotes), `MarkChanged`/id-path (219-223), `Changed<T>(since)`
(210-213), type-erased `StampFn` registered at pool creation only for
`meta->tracksChanges` pools (315-318, 356-361). **Every Scene claim in r4 §A2 is
accurate to the line.** r4's note that the doc's own line numbers drifted (loop now
324-328, not 311-316) is also correct — the doc citations are stale, r4's are live.

### 1.2 `Query::operator*` yields unstamped mutable refs — **CONFIRMED**

`Query.hpp:62-65`: `operator*() const` returns `std::tuple<Entity, Ts&...>`,
constructed straight from `*std::get<Ts*>(_components)` — bare mutable refs into
pool storage, **no stamp** on deref, `operator++`, or anywhere in the iterator. The
class comment (47-51) literally says "the component references are mutable so
systems can write component data in place," and the file's usage example (13-14)
demonstrates `pos.x += vel.x`. The hole is exactly as both reports describe, and
the framing that it is *one* bypass of an otherwise complete, tested system (r4) is
the fair characterization — not "change detection needs building."

### 1.3 Is `QueryMut`/`Mut<T>` implementable without pessimizing the hot path? — **CONFIRMED, with two precisions**

Iteration is **sparse-set intersection**, not archetype: `begin()` iterates the
smallest required pool's `_primary` entity array (171, 186) and `HasAll` probes the
other pools via `Get` (101-104), caching raw `Ts*` in `_components` (163). A parallel
`QueryMut` type is cleanly implementable and — crucially — **leaves the plain
`Query` hot path untouched**, so there is no pessimization of existing read/write
loops. Two precisions the reports gloss:

1. **The iterator has no Scene/tick back-pointer today.** It holds only
   `_required` pool pointers (161) and `_components` (163). A `Mut<T>` that bumps
   `++_changeTick` needs access to `Scene::_changeTick` (private). This is trivial
   to wire — `Scene` is already the sole friend/constructor of the view (177) and
   can hand the `QueryMut` iterator a `Scene*` or `uint64_t*` — but it is a real
   (small) addition the "just add a proxy" framing omits.

2. **The "stamps only on *actual* mutable deref" precision (r4) is OVERSTATED.**
   In the natural call site `pos->x += vel->x`, *both* `pos` and `vel` are accessed
   through a non-const `Mut<T>::operator->`, so a `Mut`-wrapped read (`vel`) stamps
   too unless the author threads const-correctness the ergonomic call site will not
   provide. In practice you only wrap the *written* types in `QueryMut<Written...>`
   and read the rest via a separate `Query`, so per-entity over-report is limited —
   but within a touched entity, read-vs-write deref cannot be distinguished for
   free. Since over-reporting is *safe* (the codebase's own stated stance: "safe
   over-reporting, never a missed change"), this does not threaten correctness; it
   just means the "surgical precision" selling point is softer than advertised. Fine
   for replication.

**r4's "1-2 days / 150-250 LOC" estimate — CONFIRMED as credible for the ECS
change itself**, precisely because the stamping substrate already exists and is
tested. Caveat: that estimate covers `Query.hpp` + a `Mut<T>` header; it does **not**
include auditing/migrating every gameplay + editor system that currently mutates a
*tracked* component through a raw `Query` ref. That migration is mechanical but its
size depends on how many systems write `Transform`/tracked components through
queries — unbounded by this audit. Call the ECS mechanism 1-2 days; budget the
migration + tests separately.

### 1.4 Codec substrate (FieldMeta) — **CONFIRMED**

`Core/Reflect/FieldMeta.hpp`: `FieldType` includes `Vec3` (25), `Quat` (27), `Enum`
(29), `EntityRef` (31), `AssetId` (34), `AssetIdVector` (35); `offset` (63),
`transient` (64), `hasMin/hasMax/minValue/maxValue` (69-72), `enumSize/enumSigned`
(82-83). Everything r3's FieldMeta codec (§3.4) and r4 lean on is present, and it is
in **Core** (render-free) — correct home for a codec both ends link. Confirmed.

---

## PART 2 — The link-DAG claim (r4 §A5) — the biggest structural finding

**Claim (a): reflection object libs are force-linked globally into every exe.
CONFIRMED.** `cmake/AssisiReflect.cmake`: `assisi_reflect` appends each
`${TARGET}-Generated` OBJECT lib to a GLOBAL property `ASSISI_REFLECT_OBJECT_TARGETS`
(108); `assisi_link_reflections` (113-118) iterates that global list and
`target_sources(target PRIVATE $<TARGET_OBJECTS:...>)` for **all** of them — no
subset mechanism. All-or-nothing, exactly as claimed. Each `-Generated` lib
`target_link_libraries PRIVATE ${TARGET}` (85).

**Claim (b): MeshRenderer/Camera/lights live in Runtime, Runtime links Render.
CONFIRMED.** `modules/Runtime/CMakeLists.txt` links `Assisi::Render` PUBLIC and
`assisi_reflect`s `Components.hpp` (MeshRenderer/Camera), `Hierarchy`, `Lifecycle`,
`LightComponents`, `NameComponent`. `modules/Render/CMakeLists.txt` links
`Assisi::Window` (GLFW), `nvrhi_vk`, `nvrhi`, `Vulkan::Headers`. `modules/App`
links `Assisi::Render`, `Assisi::Window`, `Assisi::Debug`, `Assisi::Runtime` — all
PUBLIC, unconditionally.

**Claim (c): a headless server exe would pull Vulkan/GLFW today. CONFIRMED — but
via a *different dominant mechanism* than r4 emphasizes.** r4 leads with the global
reflection link as culprit. Precision: `$<TARGET_OBJECTS:X>` injects **compiled
object files only — it does NOT propagate X's transitive link interface.** So the
reflection sweep alone does not "pull Render's `.so` in" via CMake dependency
propagation; it injects `Runtime-Generated`'s `.o` files, which then need
`Assisi-Runtime`'s symbols *resolved* at link — dragging Render only if the exe
otherwise links Runtime. The **actually decisive** culprit is simpler and r4 does
name it: **`Assisi::App` links `Assisi::Render`/`Window`/`Debug` PUBLIC (App
CMakeLists)**, so any server reusing `Application`'s loop links Vulkan/GLFW
unconditionally, reflection sweep or not. Bottom line unchanged (**server is fat
today**); the mechanism writeup should lead with App, not the reflection link.

**Remedy verdict.** r4's two options, adjudicated:

- **Option 1 (accept fat link, forbid only init):** right for **N2-N6 / v1**. Cheapest,
  unblocks everything, and the pimpl/dlopen posture *probably* lets a GPU-less
  container run it — but that is **unverified** and must be an N2 DoD ("boots with
  no `libvulkan` present, no GPU"), exactly as r4 says.
- **Option 2 (relocate component structs to a render-free module + selective
  reflection linking):** right for the **eventual dedicated-server binary**, but
  **incomplete as stated** — relocating the structs does nothing while `Assisi::App`
  still links Render. Option 2 only yields a genuinely render-free binary if it is
  paired with **splitting `App` itself** into a render-free loop/lifecycle library
  (the `InitializeCore` half + the fixed-step loop + `SystemRegistry`) and a
  presentation library. That App-split is the same seam N2 already opens for
  `Initialize()`; extend it to the link graph.

**Third/best option (recommended): do both, staged.** Ship Option 1 for v1 with the
"boots without Vulkan" DoD as the gate; commit Option 2 = (i) move the *pure-data*
component structs (`MeshRenderer`, `Camera`, light components, `Name`, `Hierarchy`,
`Lifecycle` — GUID/`AssetId` fields only, no Render types) down into `ECS` or a new
Core-level `GameComponents` module, leaving `SceneRenderer`/binding caches in
Runtime; (ii) split `Assisi-App` into `Assisi-AppCore` (render-free) + presentation;
(iii) add a `SUBSET` argument to `assisi_link_reflections` so the server links only
render-free `-Generated` libs — as the *pre-condition for shipping a container
server*, not a v1 blocker. This aligns with the plan's own `ScopedRawEntityContext`
relocation (same shape, larger) and its "neither Net module may depend on Runtime"
principle.

---

## PART 3 — App / SystemContext / LevelRuntime claims (r4 §A1, §A4)

- **Fixed-step block is presentation-independent — CONFIRMED.**
  `Application.cpp:323-328`: `accumulator += dt; while (accumulator >= physicsStep)
  { OnFixedUpdate(...); accumulator -= physicsStep; }` — touches only `accumulator`,
  `physicsStep`, `OnFixedUpdate`. No window/render/input. This is the crux that
  makes N2/N3 tractable and it holds.
- **Loop termination is a raw `_window` deref — CONFIRMED.** `while
  (!_window->ShouldClose())` at line 313; `_input->Poll()` at 321 (per **render**
  frame, so N3's "sample once per fixed tick" is a genuine behavioral change, as
  r4 says); `SleepUntil(nextRenderTime)` at 352.
- **`SystemContext` hard-codes input — CONFIRMED.** `SystemRegistry.hpp:48-55`
  bundles `Window::InputContext &input; Window::ActionMap &actions;` by reference,
  and the header `#include`s `<Assisi/Window/ActionMap.hpp>` + `InputContext.hpp`
  (34-35), so the game-logic scheduler transitively depends on Window. A headless
  server has neither. r4's point that this is a `SystemContext` **shape** change +
  a `SystemRegistry`→Window **link** change (not merely "systems migrate off
  `input.IsKeyDown`") is correct and is the sharpest under-specification in the plan.
- **`LevelRuntime::LoadLevel` is render-coupled — CONFIRMED.** `LevelRuntime.cpp:41-55`:
  `LoadLevel` takes `Render::AssetCache&` + `Runtime::SceneRenderer&`, calls
  `cache.Clear()` (50) and `sceneRenderer.InvalidateAssetBindings()` (51). The
  render-free core (`SceneSerializer::LoadFromFile` + `physics.RebuildSceneBodies`,
  the latter via `RebindSceneAssetsAndPhysics`, 34-39) is genuinely separable. This
  is a real, concrete N2 task the plan omits. Confirmed.

r4 §A3 (Physics render-free, `RigidBody` transient, `SetLinearVelocity` present but
no public setter, `SaveState` absent) and §A4 (no `Pool::IO` in JobSystem; poll GNS
on main thread) are consistent with the module layout and I found no contradiction;
these are lower-risk and I spot-checked rather than line-verified. Treat as
plausible-confirmed.

---

## PART 4 — Coherence pass over the union of recommendations

Assembling all four reports' plan-changes and testing them against each other and
against Assisi's constraints. **The single most important finding is an internal
conflict the four reports do not acknowledge:**

### CONFLICT — r3 vs r4 on what happens to plain `Query`

- **r3 §7 CHANGE-1** wants plain `Query<Ts...>()` to **yield `const Ts&`** (flip the
  default to read-only).
- **r4 §A2(a)** wants plain `Query` **left untouched** (still yields mutable refs,
  still serves untracked writes); only tracked-write systems move to `QueryMut`.

These are incompatible. r3's const-flip is a **breaking change to the single most
ergonomic, most-used API in the engine** — every existing gameplay/transform/editor
loop that writes through a `Query` ref stops compiling, for a benefit (compile-time
enforcement) that the additive `QueryMut` already delivers where it matters.
**Verdict: adopt r4's additive `QueryMut`; REJECT r3's const-default flip** (or at
most schedule it as a much later, separate hygiene migration — not part of netcode).
This directly serves the performance-first + low-churn posture.

### Recommendation-level verdicts

| # | Recommendation | Source | Verdict | Note |
|---|---|---|---|---|
| 1 | `QueryMut`/`Mut<T>` additive stamping query | r3/r4 | **adopt** | r4's additive form, not r3's const-flip |
| 2 | Re-order the stamping fix to after N3 / before N5 | r3/r4 | **adopt** | Independently useful (PropagateTransforms); land standalone with tests |
| 3 | Flip plain `Query` to const refs | r3 | **reject** | Breaks every mutating loop; `QueryMut` already covers the need |
| 4 | Bit-capable codec (`BitWriter/Reader`) from day 1 | r3 | **adopt-modified** | Build the bit primitive; keep v1 field encoders simple (whole-value); quantizers land later. Note tension: per-bit bounds-checking/fuzzing (B8) is harder than per-byte — keep the reader's bounds checks per-read regardless |
| 5 | ComponentId-keyed wire format | r3 | **adopt** | Matches the existing dense `ComponentId`; names stay for disk/debug |
| 6 | Delta-against-empty-baseline unifies spawn/delta/keyframe | r3 | **adopt** | Elegant, one code path; no conflict |
| 7 | Default snapshot rate = 20-30 Hz divisor of sim | r3 | **adopt** | Sim/input stay 60; state at 20-30. Cheap, aligned with performance-first |
| 8 | Sortable priority-list send loop (accumulator later) | r1/r3 | **adopt** | Costs nothing now; do not implement the accumulator yet |
| 9 | Explicit clock-sync module in N3 | r3 | **adopt-modified** | Name the module (est. server time, lead input). **Defer adaptive time-dilation** — it is Stage-7-class robustness; folding it into N3 is scope creep |
| 10 | Pin a specific GNS master SHA | r2 | **adopt-modified** | Just pin *something* explicit + reproducible. v1.6.0 is fine since NAT is deferred to N7; master only if you want ICE/protobuf-compat fixes early. Low stakes |
| 11 | Testing infra as first-class (loopback fixture, codec fuzz, ε-convergence oracle) | r4 | **adopt-modified** | Adopt the content; make it **DoD-teeth + one short hardening milestone**, not a sprawling parallel stage tree. The ε-convergence oracle is mandatory (engine is non-deterministic) |
| 12 | Security stage (rate-limit, per-read bounds, version string) | r4 | **adopt-modified** | Fold into N5 server work / a short "N5.5 hardening". Per-connection command-rate cap + per-field bounds check are the load-bearing items |
| 13 | Headless link-DAG decision (N2.5) | r4 | **adopt** | Option 1 for v1 (with "boots w/o Vulkan" DoD); Option 2+App-split committed before a container server ships (Part 2) |
| 14 | Per-body transform-history ring for lag comp | r3 | **defer** | Correctly Stage 7; cap rewind ~200-250 ms |
| 15 | Predicted-bit / generation bits in NetId | r1/r3 | **defer** | uint32 dense id won't exhaust at 32 players; don't recycle in-session, don't over-design NetId in v1 |
| 16 | Interest management (grid/PVS) | r3 | **defer** | Confirmed unnecessary at 2-32 players; `Replicated{}` marker only |
| 17 | Deferred-resolve for EntityRef→NetId | r3 | **adopt** | Real ordering hazard; ties to the `ScopedRawEntityContext` relocation the plan already notes |

### Constraint conflicts — assessment

- **Performance-first:** No conflict once #3 is rejected. `QueryMut` keeps the hot
  read path untouched; 20-30 Hz snapshots keep the codec off the 60 Hz sim path;
  priority-list is a no-op until needed. Aligned.
- **Editor/game module split:** #13's component relocation must keep the editor
  building (the inspector already writes via `MarkChanged`, which is unaffected).
  Moving pure-data structs down is compatible; watch that Editor doesn't include
  Render-side Runtime types it doesn't need. Manageable, not blocking.
- **JobSystem threading:** All four reports converge on "poll GNS on the main
  thread, no JobSystem in v1"; verified there is no `Pool::IO` and `DrainMain` is
  the existing marshal seam. No conflict; the codec/send work sits inside the
  16.67 ms main-thread budget with wide margin at indie scale.

### Scope-bloat verdict

The union does **not** blow up the plan **if** the additions are folded rather than
multiplied into new numbered stages: #1-2 = a pre-N5 ECS task; #9 (minus dilation) =
a named piece of N3; #11-12 = DoD-teeth + one short hardening milestone; #13 = an
explicit N2 decision + a committed pre-server refactor. Treated that way, N0-N6
remains a sane, incrementally-committable staged plan. The failure mode to avoid is
promoting every "ADD" into its own stage (testing-stage + security-stage +
clock-stage + N2.5 + adaptive-clock) — *that* would bloat a clean plan into a
dozen ceremonial milestones. Reject the sprawl, keep the content.

### The one-paragraph version

Every codebase claim in r3 and r4 checks out against the source, down to the line
(the change-tick substrate is real and tested; `Query::operator*` is the one
unstamped hole; the reflection sweep + `App`-links-Render genuinely make today's
server binary fat). The QueryMut fix is implementable and cheap (1-2 days for the
mechanism; migration extra), though the "surgical precision" pitch is softer than
sold and r3's competing proposal to flip plain `Query` to const refs should be
**rejected** in favor of r4's additive form. The link-DAG needs the explicit
decision r4 demands — but Option 2 is incomplete without also splitting `App`, and
the honest mechanism is "App links Render," not the reflection sweep. The combined
recommendation set is coherent and stays a sane staged plan *provided* testing,
security, clock, and link-DAG work is folded into existing stages/DoDs rather than
spun out as a stage sprawl, and provided adaptive time-dilation, transform-history,
NetId bit-packing, and interest management stay deferred where the plan already
puts them.
