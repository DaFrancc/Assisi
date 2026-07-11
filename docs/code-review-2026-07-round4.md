# Codebase review, round 4 — 2026-07-10

Fourth full review (all of `modules/`, `apps/sandbox`, shaders,
`tools/reflectgen`, build system, tests), done independently and *then*
compared against rounds 1–3. Verified that round 3's burn-down is real in the
code: `Scene`/`Registry` copy+move deletion with trait `static_assert`s, the
DebugUI descriptor mark-and-sweep, the dep pinning (stb/implot/ImGui to
commits), the README rewrite, and the `ShortNames.hpp` `__INCLUDE_LEVEL__`
guard are all present. Nothing checked off in round 3 was fake. The texture
pipeline (sRGB albedo, mipmaps, anisotropy — one of round 3's "to 9" items)
landed after round 3 closed.

Scored **7/10** against round 3's 7.5. Not a regression in the code — the
delta is a weighting disagreement, recorded under Scoring below: this review
treats missing CI as a missing *gate* on standards the repo already claims
(not a missing feature), and the fixed-timestep interpolation gap as a latent
defect in the frame loop's core contract. Both reviews agree on the facts and
on the direction; the trajectory (6 → 7 → 7.5 across three days, no faked
checkboxes) remains the strongest signal in the repo.

Checkboxes so this can be burned down like the previous three docs.

## Correctness bugs (real, not taste)

- [x] **Fixed timestep renders un-interpolated state.** `Application::Run()`
  (`Application.cpp:287-293`) implements the accumulator loop but `OnRender`
  draws the raw latest physics state. At 60 Hz physics on a 144+ Hz display
  (or with the FPS cap off), physics-driven motion visibly stutters — the
  render rate beats against the step rate. The classic second half of "Fix
  Your Timestep" is missing. Fix: pass `accumulator / physicsStep` as an
  interpolation alpha into the render path and blend previous/current
  transforms for physics-owned entities — or make an explicit, documented
  decision that Assisi games render at the physics rate's granularity and
  accept the judder. Either resolves the promissory note; silence doesn't.
  *Done (2026-07-11):* took the interpolation route. `Application::Run()`
  now computes `accumulator / physicsStep` as an alpha after the fixed-update
  loop, exposed via `GetInterpolationAlpha()`. `PhysicsWorld` splits the old
  `SyncTransforms` into `CaptureState()` (snapshots each dynamic body's pose
  per fixed step) and `InterpolateTransforms(scene, alpha)` (blends the last
  two snapshots — `mix` for position, `slerp` for rotation — into the ECS
  Transform per render frame); snapshots reset on teleport so an inspector
  edit snaps instead of sliding. Verified interactively: physics motion is
  smooth with the FPS cap off.

- [x] **`SceneSerializer::Load` leaks its thread-local context and leaves the
  scene half-loaded on a throwing component.** Pass 2 calls each component's
  `addToScene`, whose generated deserialize code indexes JSON with `j.at(...)`
  / `_v[0].get<T>()` — malformed field data throws. `LoadFromFile` caught at
  the outer level and returned false, but by then `s_context` was still
  engaged (the tail `s_context.reset()` never ran) and the scene held a
  partial entity population, so any `EntityToIndex`/`IndexToEntity` call in
  between resolved against stale state.
  *Done (2026-07-10):* added a `ScopedContextReset` RAII guard in the anon
  namespace, used in both `Save` and `Load`, so `s_context` is torn down on
  every exit path including a mid-pass throw (previously `Save` also leaked on
  a throwing serialize lambda — now covered too). `LoadFromFile`'s catch now
  calls `scene.Clear()`, so a failed load yields an empty scene rather than a
  corrupt half-populated one. Behaviour on the success path is unchanged.
  **Unbuilt — build + `ctest -R Runtime` before committing.**

- [x] **`ChoosePhysicalDevice` selects a device it never validated.**
  (`VulkanContext.cpp:140-200`) checks only graphics+present queue support.
  It never confirms the device exposes `VK_KHR_SWAPCHAIN_EXTENSION_NAME`,
  nor the Vulkan 1.2/1.3 features `CreateLogicalDevice` hard-requires
  (timelineSemaphore, synchronization2, dynamicRendering), nor apiVersion >=
  1.3. On conforming desktop drivers this is academic; on a machine where
  the first enumerated device is a compute-only or pre-1.3 adapter, device
  creation fails with a generic error instead of falling through to a
  capable device. Fix: fold the extension/feature/apiVersion checks into the
  candidate filter so selection and creation agree on requirements.
  *Done (2026-07-11):* added `DeviceMeetsRequirements()` (apiVersion >= 1.3,
  `VK_KHR_swapchain`, and the three NVRHI features queried via
  `vkGetPhysicalDeviceFeatures2`), folded into the candidate loop so an
  unsuitable adapter is logged with its reason and skipped in favour of a
  capable one. Also added `Core::ShowErrorDialog` (native modal on Windows,
  log fallback elsewhere): when no device qualifies — or `vkCreateInstance`
  itself fails — the user now gets a clear "update your drivers / GPU too old"
  dialog instead of a silent exit. Both dialog paths smoke-tested by forcing
  the failures; happy-path device selection verified unchanged.

## Hygiene

- [x] **Runtime-generated files are tracked in git.** `imgui.ini` and
  `options.json` at the repo root are written by every sandbox run — the
  working tree was perpetually dirty on them.
  *Done (2026-07-10):* `git rm --cached` both (working copies kept) and added
  them to `.gitignore`. Verified both are read/written under the *user* root
  (`OptionsConfig` via `ReadUserText`/`WriteText`; ImGui via CWD), with graceful
  fallback to defaults when absent — the repo-root copies were pure artifacts of
  running the app from the repo root, so untracking changes no runtime behaviour
  and a fresh clone gets defaults. If a curated default ImGui layout is ever
  wanted, ship it under `assets/` and copy it to the user root on first run.

- [ ] **No CI.** Carried from rounds 2–3, escalated here: tests across five
  modules, golden-file codegen tests, `ASSISI_WARNINGS_AS_ERRORS`, sanitizer
  options, and 3×3 toolchain presets all exist — and none of it gates a
  commit. A public repo inviting PRs with ungated tests is standards-as-
  decoration. One GitHub Actions workflow (windows-msvc + ubuntu-gcc/clang,
  configure + build + ctest) is an afternoon and is the single highest-
  leverage item in this document.
  *Deferred by decision (2026-07-11):* with a single contributor building and
  testing locally on Windows every commit, CI's core value — gating others'
  PRs and catching cross-toolchain breakage before merge — doesn't yet pay for
  the per-preset runner setup. Revisit the moment a second contributor lands or
  the repo starts accepting external PRs; until then this stays an explicit
  "not yet," not a gap to burn down.

- [ ] **Assimp: fetched, compiled, linked, unused.** Carried from round 3,
  where it was deferred by decision (mesh loader is near-term). Kept on the
  ledger so the decision has a review-cycle expiry rather than becoming
  permanent by default: if the loader hasn't landed by the next review
  round, delete the dep.

## Scaling cliffs (decide deliberately, not urgently)

None of these are bugs, and per the round-2 principle missing features don't
cap the score. They're listed because each is a *shape* decision that gets
more expensive every time a new system builds on the current one.

- [ ] **ECS lookup costs are linear-in-use with no cheap growth path.**
  `Scene::Get/Has/Remove` do an `unordered_map<type_index>` find per call
  (`Scene.hpp:87-117`); `Query` iteration does one sparse-set probe per
  required component per candidate entity (`Query.hpp:91-105`). Correct,
  cache-friendly enough at sandbox scale — but there are no compile-time
  component IDs and no grouping/archetype story, so the design has no
  incremental path to tens of thousands of entities. Worth a deliberate
  decision (accept the ceiling / add static component IDs / group hot pairs)
  before more systems assume the current shapes.

- [ ] **`PropagateTransforms` rebuilds every world matrix every frame.**
  (`Hierarchy.cpp:13-75`) — allocation-free after warmup (round 3's fix is
  real) but hash-heavy: one map insert per entity per frame, no dirty
  flags. Fine at editor scale; a dirty-flag pass or ordered parent-before-
  child storage is the eventual answer.

- [ ] **Draw submission has no culling, sorting, or instancing.**
  `DrawScene` (`Renderer.cpp:15-27`) submits every `MeshRenderer`
  unconditionally; `MeshPass::Draw` sets full graphics state per draw.
  Acceptable today (one pipeline, few meshes). Frustum culling is the first
  cheap win when scenes grow.

- [ ] **`MeshPass::_bindingSetCache` is still unbounded within a level.**
  Round 2 added `InvalidateBindingSets()` called from `LoadLevel` — cross-
  level lifetime is solved. Within a level, every distinct albedo texture
  ever drawn holds a binding set (and its texture) alive until the next
  level load (`MeshPass.cpp:164-190`). Becomes real the day asset streaming
  lands (see `asset-streaming-design-notes.md`); the DebugUI mark-and-sweep
  from round 3 is the pattern to mirror.

- [x] **Contracts still enforced by doc comments where debug asserts could
  live.** Carried from round 3's "to 9": `Scene::Query`'s structural-change-
  during-iteration UB and `EventQueue::Read`'s push-while-reading hazard are
  documented, not asserted. A debug-build structural-change counter on the
  pools (bumped in Add/Remove/Clear, checked by the iterator) turns "hope
  the caller read the header" into a loud assert.
  *Done (2026-07-11):* both hazards are now enforced in debug, compiled out in
  release.
  - **`Scene::Query`:** `SparseSet` carries a debug-only `StructureVersion()`
    bumped on every Add/Remove/Clear; the `Query::Iterator` snapshots the summed
    version of its required+excluded pools at construction and asserts it
    unchanged on every deref/advance. Versions only ever rise, so any
    mid-iteration structural mutation of a queried pool trips the assert instead
    of silently reallocating the dense array.
  - **`Scene::Destroy` made deferred (in the same pass):** it queues the entity
    (still fully alive — IsAlive true, Query yields it) and the removal is
    applied by the new `Scene::FlushDestroyed()`, called once per frame from
    `Application::Run` via the `Application::FlushDeferred()` hook. So
    destroy-during-iteration is now correct-by-construction; only same-type
    `Add`/`Remove`-during-iteration relies on the assert.
  - **`EventQueue::Read`:** now returns a checked `EventSpan<E>` (a span-like
    view — range-iterable, indexable, convertible to `std::span<const E>`)
    instead of a raw `std::span`. Each `TypedQueue` carries a debug-only version
    bumped on Push/Clear; the view snapshots it and asserts on every element
    access, so a same-type Push while iterating trips a loud assert rather than
    reading a reallocated buffer.

- [ ] **Single hardcoded pass sequence in `Application::RenderFrame`.**
  (`Application.cpp:375-422`) clear → OnRender → PostProcess → ImGui is
  wired inline. Fine now; the first shadow map forces a pass-ordering
  abstraction. No action needed yet — just don't let a third pass land by
  widening this function.

## What's good (don't regress it)

Everything rounds 1–3 listed still holds, plus specifically earned since:

- The texture pipeline landed with its reasoning intact: sRGB albedo formats
  (so filtering/mip-blending happen in linear space, with the shader comment
  explaining why the decode left `cube_min.frag`), mip generation, anisotropy
  clamped to device limits and gated on device support.
- The editor camera's move to plain state (dropping its dedicated ECS scene)
  is the right altitude call — it was a scene with exactly one entity.
- Comment discipline is still the codebase's defining strength, and round 3's
  fixes came *with* their why-comments (the DebugUI retire-delay constant
  documents its relationship to `kFramesInFlight`).
- The review docs themselves: three rounds of scored, checkboxed, verified
  burn-down where every checked item was actually fixed. Most professional
  teams do not have this.

## Scoring and the path up

**7/10.** Same facts as round 3's 7.5; different weights, recorded so future
rounds can arbitrate. Round 3 scores by the debt ledger ("provisional caps
the score, missing doesn't"). This review adds two clauses: (1) an ungated
standard is itself a promissory note — the repo *claims* warnings-as-errors,
tests, and multi-toolchain support, and nothing enforces any of it on a
commit; (2) a documented-but-unresolved quality defect in the frame loop's
core contract (interpolation) is provisional, not missing. Under round 3's
own principles, both clauses arguably already apply.

**To 8 — fix this document's short list.** CI (build + ctest, three
toolchains), the interpolation decision, the serializer scope guard, the
device-selection filter, untrack the two runtime files. Mechanical; an
afternoon or two, CI included.

**To 9 — unchanged from round 3.** Enforced contracts (asserts, not doc
warnings), a second consumer for the App/reflection/SystemRegistry layers,
and the first real renderer feature (shadows or a material system) landing
*with* its debt paid — no new promissory notes.

**To 10 — same asymptote as rounds 2–3:** shipped scar tissue, measured
performance work, an explicit threading model, owned failure modes. The
score follows the debt ledger, not the feature count.
