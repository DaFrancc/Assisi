# Codebase review, round 5 — 2026-07-12 (multi-model)

Fifth full review, run differently from rounds 1–4: **four independent
reviewers, no shared context** — two Claude agents reviewing cold in parallel
(A and B, same instructions, no visibility into each other), GPT-5.2 Pro
(`gpt-5.2-pro-2025-12-11`), and DeepSeek (`deepseek-v4-pro`), each given the
full source (everything except `docs/`, which no reviewer opened until all
four reviews were complete). A Gemini slot was planned and dropped (no API
quota for the pro models on the configured key). Every load-bearing claim was
verified against the code before making this document; verified false
positives are recorded at the bottom so future rounds don't rediscover them.

All reviewers were instructed to weigh INFORM.txt and did: the four deliberate
deferrals (no CI, unused Assimp, unbounded binding-set cache, hardcoded pass
sequence) are not findings here.

**Scores:** Claude A **7**, Claude B **7.5**, GPT-5.2 Pro **6**, DeepSeek
**6**. Weighted composite **6.9/10** (weights 0.3/0.3/0.25/0.15 — the Claude
agents verified findings against code with tool access and independently
converged on the same top four issues; GPT corroborated the worst single issue
but two of its three Criticals failed verification; DeepSeek's two Criticals
were both false, so its 6 rests on bad evidence). Consistent with round 4's 7.

**The strongest cross-reviewer signal:** boundaries are trusted rather than
enforced — GPU buffer caps, config values, shutdown ordering, and lifecycle
links between systems. Everything in the High list is days of work, not
structural rot.

Checkboxes so this can be burned down like rounds 1–4.

## Critical

- [ ] **Partial-init teardown hole.** `Application::Initialize()` failing
  *after* `RenderSystem::Initialize()` succeeded (e.g. missing
  `fxaa.frag.spv` makes `_postProcess.Initialize` fail at
  `Application.cpp:186`) leaves `_initialized == false`, so `~Application`
  (`Application.cpp:199-214`) skips `RenderSystem::Shutdown()` entirely — the
  Vulkan device is then destroyed during static teardown, the exact
  vulkan-1.dll loader-race crash the destructor's own comment warns about.
  Introduced by round 1's own ctor→`Initialize()` fix; unexamined for three
  rounds. Fix: unwind the subsystems that *did* initialize (scope guard or
  per-stage flags), don't gate all teardown on one bool.
  *(Claude A + GPT-5.2, found independently — strongest corroboration in the
  round.)*

## High

- [ ] **Exit-time use-after-free from `WhiteTexture()`.**
  `DefaultResources.cpp:12` caches an `nvrhi::TextureHandle` in a
  function-local static with no teardown hook; it destructs during static
  teardown, after `RenderSystem::Shutdown()` freed the device. Fires on every
  clean exit that drew an untextured mesh (every default level cube). Fix: a
  `DefaultResources::Shutdown()` called before `RenderSystem::Shutdown()`.
  *(A + B.)*

- [ ] **GPU OOB writes in cluster culling.** `cluster_cull.comp:113-120`
  reserves index slots with an unclamped `atomicAdd` against
  `MAX_LIGHT_INDICES` (65,536 per half): 3,456 clusters × up to 64 lights ≈
  221k possible reservations. Point indices overflow into the spot half;
  spot writes run past the buffer entirely. Clamp the reserved offset/count
  against capacity after the `atomicAdd`. *(A + B.)*

- [ ] **Light counts sent to shaders unclamped.** `Buffer::Upload` truncates
  at capacity (1,024 point/spot, 16 dir — round 3's warn-once fix), but
  `ClusterGrid.cpp:124-136` / `MeshPass.cpp:160` push the *raw* counts, and
  `robustBufferAccess` is never enabled (`VulkanContext.cpp:301`). Exceeding
  a cap is GPU out-of-bounds reads, not just wrong lighting. `std::min` the
  counts with the same caps `Upload` applies. This is the missing half of
  round 3's "silent capacity ceilings" item. *(A + B.)*

- [ ] **Re-parenting never dirties world matrices.** `Parent` is a plain
  untracked `ACOMP()` (`Hierarchy.hpp:28`), `Scene::MarkChanged` is a no-op
  for untracked pools, and `PropagateTransforms` (`Hierarchy.cpp:68-93`)
  only tests `Transform` ticks — so an inspector re-parent leaves the child
  rendering at its stale world matrix until its own TRS changes. Track
  `Parent` (`ACOMP(tracked)`) and fold its tick into the dirty test, or
  force-recompute on hierarchy edits. Hole in the change-detection feature
  that landed 07-11/12. *(A + B.)*

- [ ] **No entity→physics-body destruction path.** `PhysicsWorld` exposes
  only `AddBox`/`Clear()`; nothing observes entity destruction, so
  destroying an entity carrying a `RigidBody` leaves an invisible,
  still-simulating Jolt body (plus a leaked snapshot entry) until the next
  level load. First gameplay code that despawns a physics entity corrupts
  the simulation silently. *(A + B.)*

## Medium

- [ ] **`physicsHz` unvalidated.** `AppConfig.cpp:62` + `Application.cpp:279-307`:
  zero silently disables fixed update (step = inf); negative makes the
  accumulator loop non-terminating — one bad `game.json` line hangs the app.
  Validate on load, warn and fall back to the default. *(A + B + GPT — three
  of four reviewers.)*

- [ ] **Lights use local position, not the world matrix.**
  `LightingSystem.cpp:35,46` reads `transform.position` while meshes render
  from `worldMatrix` — a parented light illuminates from the wrong place.
  Also `glm::normalize(light.direction)` at :47,56 NaN-poisons the buffer on
  a zero direction. Read `worldMatrix[3]` after propagation; guard the
  normalize. *(A + B.)*

- [ ] **Per-renderer propagation-tick bookmark.** `SceneRenderer.hpp:105`
  keeps `_lastPropagationTick` in the renderer while the tick is per-Scene;
  rendering a different or recreated Scene whose counter is lower skips
  propagation for everything. Latent until multi-scene / scene reload is
  used — which `SceneRegistry` already ships. Key the bookmark by scene (or
  store it on the Scene). *(A + B.)*

- [ ] **Crash handlers log before dumping.** `Application.cpp:53,73`:
  `CrashHandler`/`AbortHandler` call `Log::Fatal` (heap-allocating
  `std::format` + logger mutex) *before* `MiniDumpWriteDump` — a crash on a
  corrupt heap or with the logger mutex held loses the dump exactly when it
  matters. Write the dump first; log after (or use pre-formatted static
  buffers). *(A + B; DeepSeek circled the same code with wrong semantics —
  see false positives.)*

- [ ] **reflectgen silently drops unknown field types.**
  `reflectgen.py:149,374-407`: the "fail loudly on unsupported types" guard
  checks only the deliberately-empty `UNSUPPORTED_TYPES` dict; any type not
  in `TYPES` becomes `FieldType::Unknown` and is quietly omitted from
  serialize/deserialize — silent save-data loss for the next new field type.
  Make unknown non-transient types a generation error, as the docstring
  already promises. Survived round 2's reflectgen test suite. *(A + B.)*

- [ ] **`PickEntity` ray is wrong twice.** `SandboxCamera.cpp:170-178`
  divides window-coordinate mouse position by framebuffer pixel size (wrong
  on HiDPI — the exact case `WindowContext.hpp:102-109` warns about) *(A +
  B)*, and builds the ray at clip-space Z = -1 under
  `GLM_FORCE_DEPTH_ZERO_TO_ONE`, where the Vulkan-style near plane is Z = 0
  *(GPT)*. Also treats every `Transform` entity as a unit OBB, so invisible
  entities are clickable *(B)*.

- [ ] **Physics treats local TRS as world pose.**
  `PhysicsWorld.cpp:333-383` writes body world poses into local
  `Transform` fields and `SandboxLevels.cpp:133-142` creates bodies from
  local TRS — a `RigidBody` entity under a `Parent` double-transforms. At
  minimum assert/document "physics entities must be parentless". *(B.)*

- [ ] **`ComponentRegistry` name/id fragility.** Duplicate bare struct names
  from different namespaces register both and bind ambiguously (non-stable
  `std::sort` on names, `ComponentRegistry.cpp:16-31`); a post-startup
  `Register` re-sorts and invalidates every outstanding id/`ComponentMeta*`
  with only a comment as the contract (:25-45). Reject duplicates; assert
  no-register-after-finalize. New code from the 07-12 ComponentId work.
  *(B.)*

- [ ] **Sandbox level Save writes into the build-directory asset copy.**
  `SandboxLevels.cpp:105-111` + `apps/sandbox/CMakeLists.txt:33-42`: the
  asset-copy build step overwrites the saved level on the next rebuild —
  authored levels silently reverted, never in the source tree. Save through
  a path that round-trips to source assets (or block Save with a message).
  *(A.)*

- [ ] **Swapchain `imageUsage` may be too narrow.** `VulkanContext.cpp:546`
  requests `COLOR_ATTACHMENT_BIT` only; NVRHI clears/MSAA resolves can
  require transfer usage on some backends. Plausible, driver-dependent —
  run the validation layers over the MSAA path and add the flag if it
  fires. *(GPT; unverified.)*

- [ ] **DebugUI descriptor pool has no headroom and no error check.**
  `DebugUI.cpp` `Initialize()`: `vkCreateDescriptorPool` result unchecked;
  `maxSets = kMaxDebugTextures` leaves nothing for ImGui's own allocations
  (font texture etc.). *(GPT.)*

- [ ] **Latent thread-safety debts, cheap to retire now.** Logger's single
  mutex spans formatting and sink writes — any sink that logs recursively
  deadlocks *(GPT)*; `AssetSystem` globals are unsynchronized against the
  future async loader *(GPT)*; `g_joltRefCount` (`PhysicsWorld.cpp:161`) is
  a non-atomic int *(DeepSeek)* — fine single-threaded today, one-line
  `std::atomic` fix. Document the threading assumption or fix while small.

- [x] **`-mavx2 -mfma -mf16c …` forced engine-wide** (root
  `CMakeLists.txt`): SIGILL on pre-AVX2 CPUs. Almost certainly a deliberate
  perf default — but it's documented nowhere. One INFORM/README line
  ("requires AVX2, by decision") converts a finding into a spec.
  *Done (2026-07-12):* documented in the README's requirements section as the
  deliberate minimum-CPU baseline (x86-64 + AVX2, Haswell/Zen or newer),
  including the SIGILL failure mode on older hardware. *(DeepSeek;
  related: A notes `-ffast-math` strips the `isfinite` guard in
  `DefaultMeshes.hpp:51` — that risk was explicitly accepted in round 3 with
  a trigger condition (untrusted mesh data lands); trigger not yet fired, so
  the round-3 decision stands.)*

## Low (bundled)

- [ ] `InputContext.cpp:28-31` calls `glfwGetKey` for key codes 0–31 (below
  `GLFW_KEY_SPACE`) → `GLFW_INVALID_ENUM` 32×/frame. *(A + B.)*
- [ ] No `glfwSetErrorCallback` anywhere — GLFW error detail (including the
  above) is silently discarded. *(A.)*
- [ ] Options overlay is bound to F11; help text, file comments, and
  `Application.hpp` docs all say F12. *(A + B.)*
- [ ] `AssetCache.cpp:48-51`: failed mesh paths aren't negative-cached
  (textures are, :68-70) → per-entity per-frame warning spam. *(A.)*
- [ ] `PhysicsWorld.cpp:370-382`: `InterpolateTransforms` writes + marks
  changed every frame for every non-static body, including at rest —
  re-dirties subtrees and defeats the new dirty-skip propagation. *(A.)*
- [ ] `AddBox`/`ReshapeBox` accept half-extents below Jolt's convex radius
  (0.05) — an ordinary inspector drag asserts inside Jolt. *(A.)*
- [ ] `VulkanContext.cpp:782-787`: `VK_ERROR_OUT_OF_DATE_KHR` recovery
  depends solely on a resize callback; out-of-date without a size change
  freezes the image until a manual resize. *(A.)*
- [ ] `WindowContext.cpp:159-166`: `ShouldClose`/`RequestClose`/`SetTitle`
  pass a null `GLFWwindow*` when construction failed / moved-from. *(A + B.)*
- [ ] `DebugUI.cpp:241-267`: ImGui texture badges keyed on raw `ITexture*`
  with no invalidation hook — `MeshPass` got `InvalidateBindingSets()` for
  the identical hazard, DebugUI didn't. *(B + GPT.)*
- [ ] `Hierarchy.cpp:82`: parent-cycle `Log::Error` fires every frame —
  unbounded spam for a one-time data mistake. *(B.)*
- [ ] `Registry.cpp:63-67`: `IsAlive` doesn't consult the free list (unlike
  `EntityAt`) — a forged handle with a post-bump generation passes. *(B.)*
- [ ] `VulkanContext.cpp:513-521`: surface-format fallback can pick sRGB
  while `cube_min.frag:220` also gamma-encodes manually → double gamma on
  such devices. *(B.)*
- [ ] `PostProcess.cpp:158-171`: `MSAA_FXAA` allocates a full-res
  `_sceneDepth` that is never used — VRAM waste. *(B.)*
- [ ] `IsVSyncEnabled()` lies after the IMMEDIATE→FIFO fallback in
  `ChoosePresentMode()` — state and actual present mode disagree. *(GPT.)*
- [ ] `SceneRenderer::Render()` compares projection matrices with exact
  float equality to decide cluster-grid rebuilds — brittle. *(GPT.)*
- [ ] `ActionMap::Unbind` allocates a `std::string`, defeating the
  heterogeneous-lookup design the class already has. *(GPT.)*
- [ ] `Registry::EntityAt` linear-searches `_freeSlots` — O(N) per call
  under churn; editor UIs that call it per-frame will feel it. *(GPT.)*

## Verified false positives (do not re-litigate)

- **DeepSeek Critical "CrashHandler infinite loop":** `EXCEPTION_EXECUTE_HANDLER`
  returned from a `SetUnhandledExceptionFilter` filter *terminates* the
  process; it's `EXCEPTION_CONTINUE_EXECUTION` that would re-run the faulting
  instruction. The handler is correct.
- **DeepSeek Critical "AbortHandler UB":** a returning SIGABRT handler still
  lets `abort()` terminate; the handler is a deliberate pre-death log line.
- **GPT High "TrivialString UTF-8 back-off checks the wrong byte":** checking
  `text[count]` (the first *excluded* byte) is the correct algorithm — back
  up while the boundary splits a codepoint; the code and its comment are
  right.
- **GPT Critical "Jolt thread count can be 0/-1":** Jolt treats -1 as
  autodetect and supports 0 workers (the calling thread executes jobs). At
  most a clamp for clarity; not a crash.

## Cross-reviewer signal

Claude A and Claude B, reviewing cold with identical instructions, shared 13
findings including an identical top four (WhiteTexture UAF, cluster-cull
overflow, unclamped light counts, re-parenting hole) — that convergence is
the round's high-confidence core. GPT independently found the partial-init
Critical that A also found. Single-reviewer findings above were spot-verified
where load-bearing; the rest are marked with their sole reporter.

## Against rounds 1–4

- **Burn-down integrity confirmed:** no reviewer found any checked-off item
  from rounds 1–4 to be fake or regressed.
- **Caught that prior rounds missed:** the partial-init hole (introduced by
  round 1's own fix), the unclamped-counts half of round 3's capacity-ceiling
  item, reflectgen's empty `UNSUPPORTED_TYPES` guard, crash-handler ordering,
  and the ComponentRegistry duplicate-name ambiguity (in code from 07-12).
- **New-code debt:** the change-detection holes (untracked `Parent`,
  per-renderer tick, physics re-dirtying) are all in the feature that landed
  after round 4 — cuts against round 4's "to 9: features land with debt paid".
- **Deliberate positions respected:** CI and Assimp stayed deferred; the
  `-ffast-math` risk-acceptance from round 3 stands (rediscovered, not
  refuted).

## Scoring and the path up

**6.9/10 composite** (A 7 × 0.3, B 7.5 × 0.3, GPT 6 × 0.25, DeepSeek 6 ×
0.15; the two 6s rest partly on findings that failed verification, hence the
lower weights). Same facts as round 4's 7 — a genuinely strong solo codebase
whose recurring weakness, seen by all four reviewers from different angles,
is trusting boundaries instead of enforcing them.

**To 8 — burn down Critical + High.** Partial-init unwind, a
`DefaultResources::Shutdown()`, two clamps (cull shader + light counts),
track `Parent`, and an entity→body destroy hook. Days, not weeks.

**To 9 — unchanged from rounds 3–4,** plus: new features land with their
change-detection/lifecycle edges paid up front, and the threading assumptions
get written down (or fixed) before the first background thread lands.
