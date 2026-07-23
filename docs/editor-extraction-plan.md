# Editor extraction — analysis & staged plan

Branch: `extract-editor`. Resolves the Template Phase 3 open decision in
`template-conversion-plan.md` and `remaining-work.md` §5. Adversarially
reviewed 2026-07-22 (independent codebase-verification pass); corrections
folded in — notably: editor render passes degrade rather than fail, sidecar
minting lives inside `AssetDatabase::Rebuild` (not the editor), and the E0
resolver-wiring omission.

**Decision: the editor becomes a library (`modules/Editor`, `Assisi::Editor`),
not a separate executable.** Reflection registration is link-time — the
inspector/serializer/undo system can only see components compiled into *this
binary's* link (`assisi_link_reflections` gathers OBJECT libraries whose static
initializers populate `ComponentRegistry`). A standalone editor exe could never
inspect the game components a template user writes. So the Unreal shape: one
project, two targets — `Game` links `GameLib + Assisi::App`; `GameEditor` links
`GameLib + Assisi::App + Assisi::Editor`. Ship builds physically contain no
editor code; the editor sees every game component for free.

The two-target template itself is Phase 2 (next branch). This branch does the
extraction that unblocks it, keeping `Assisi-Sandbox` building and behaving
identically at every stage.

## 1. What the analysis found

`apps/sandbox` is 6,549 LOC across 16 files (5,814 in `src/`, the rest the
EditHistory test), essentially all methods of one `SandboxApp` class (plus a
few file-local free helpers, all trivially movable). It is **three different
programs interleaved**, not two:

### (a) Missing engine runtime, mislabeled as editor code

`SceneSerializer::LoadFromFile` only deserializes component data. Everything
that turns that data into a *working* scene lives in SandboxApp:

| What | Where today |
|---|---|
| `ResolveMeshRendererAssets` — mesh + per-slot material GUIDs → GPU resources (override else manifest `SlotMaterial`, fallback material) | `SandboxAssetBrowser.cpp:343` |
| `AddPhysicsBody` — `RigidBodyDescriptor` → Jolt body + transient `RigidBody` | `SandboxPlay.cpp:34` |
| `RebindSceneAssetsAndPhysics` — wholesale rebuild after load/restore | `SandboxPlay.cpp:51` |
| `LoadLevelFromPath` core — deserialize, cache clear, binding invalidation, rebind | `SandboxLevels.cpp:156` |
| Async-streaming upgrade loop — re-resolve MeshRenderers while loads are in flight | `SandboxApp.cpp:637–643` |
| Asset-resolver wiring — `SetAssetIdHintResolver` + `AssetCache::SetAssetResolvers` against the database | `SandboxApp.cpp:161–170` |

A game built on `Assisi::App` today loads levels with **no meshes and no
physics**. This must move into engine modules regardless of any editor
decision — it is stage E0, and it ships.

### (b) The editor (→ `modules/Editor`)

Inspector (reflected field editing, 1002 LOC), asset browser + thumbnail cache,
transform gizmo (ImGuizmo), entity picking + eyedropper, fly camera + focus
animation, collider wireframes/silhouettes, levels window + save, play control
(F5/F6/F7, snapshot/`ReviveAt` restore), undo/redo (`EditHistory`, 669 LOC +
732 test LOC), history panel, entity list, diagnostics window, options overlay
(F11), reimport/reconcile pass + stale-material modal, dirty-title tracking.

The options window is a judgment call — AA/VSync/FPS is genuinely game
settings — but it is built on ImGui/ImPlot and `Application`'s protected
frame-stats API, and a shipped game won't build its settings UI in ImGui
(per the retained-UI plan in `decision-ui-strategy.md`). It goes to Editor.
Consequence, accepted: until the retained UI lands, a Game target's only
runtime graphics-settings control is editing `options.json` by hand —
`OptionsConfig`/`ApplyDisplayOptions` stay engine API precisely so a game
can build its own surface when it needs one.

### (c) The game/template residue (stays in `apps/sandbox`, thinned in Phase 2)

`main.cpp` arg parsing, `game.json` input-binding load, initial camera pose.
That's nearly nothing — confirming the sandbox *is* the editor today.

### Key structural facts the plan builds on

- **`SandboxApp` moves wholesale, it does not get dismantled.** Every panel is
  a method on one class holding shared state (`_scene`, `_selectedEntity`,
  `_history`, `_assetCache`, …). Renaming the class to `Editor::EditorApp` and
  moving it into the library preserves all of that; no `EditorContext`
  refactor needed. The extraction is a *move*, not a rewrite.
- **`EditorApp` derives from `App::Application`.** This is what makes the
  options window and diagnostics legal from a library: `GetOptions()`,
  `GetFrameStats()`, `GetSceneFramebufferInfo()` etc. are protected members.
  It is also the honest architecture — the editor *is* an application.
  (The "no subclassing" product rule is about game code; `Application` is the
  one blessed base, and the game template still derives from it directly.)
- **Module DAG stays acyclic.** Editor links `App` (which already links
  Runtime, Physics, Render, Debug, Window, Core) plus `imguizmo`, `implot`,
  `nlohmann_json`. Nothing links Editor except final editor executables.
- **Runtime does not link Physics** (Runtime: Core/ECS/Math/Render; Physics:
  Core/ECS/Jolt). So (a) cannot land as one function in Runtime — it splits
  along module lines, with App (which links both) composing them. See E0.
- **Editor-only render code is compiled into engine modules and initialized
  unconditionally.** `SceneRenderer::Initialize` always builds OutlinePass /
  IconPass / LinePass and loads `assets/editor/**` shaders + the entity icon
  (`SceneRenderer.cpp:27–45`). These are deliberately non-fatal — each failure
  is a `Log::Warn` + degrade (`SceneRenderer.cpp:105–128`), so a ship build
  *boots* without editor assets; it just wastes load attempts, spams warnings,
  and carries three dead pipelines. E3 makes them opt-in as hygiene, not as a
  boot fix.
- **`EditHistory` is already clean.** Deps: Core/ECS/Runtime/nlohmann only —
  no ImGui, no Application. Its test compiles the source directly and links
  explicit generated-object sets to avoid dragging Jolt in. Keep that pattern.
- **`AEVENT()` is annotation-only** (expands to nothing; sandbox has no
  `assisi_reflect` call), so `EntitySelectionChangedEvent` moves freely.

## 2. Stages

Each stage is one commit (or a few), `Assisi-Sandbox` builds warning-free and
runs identically after each, `ctest` green throughout.

### E0 — extract the runtime that must ship (no editor involvement) — **DONE 2026-07-22**

Landed as planned: `PhysicsWorld::AddBodyFromDescriptor`/`RebuildSceneBodies`,
`Runtime::AssetResolve.{hpp,cpp}`, `App::LevelRuntime.{hpp,cpp}` (including
`InstallAssetResolvers` and the safe-point contract on `LoadLevel`), sandbox
switched over, local copies deleted. gcc+clang 0 warnings, 8/8 suites.
Pending: a manual sandbox run (level load, play/stop, streaming pop-in).

New engine API, then the sandbox becomes its first caller:

1. **Physics**: `PhysicsWorld::AddBodyFromDescriptor(ECS::Scene&, ECS::Entity,
   const ECS::Transform&, const RigidBodyDescriptor&)` — the body of today's
   `AddPhysicsBody` (motion type, shape desc, CCD flag, attach transient
   `RigidBody`). Plus `RebuildSceneBodies(ECS::Scene&)`: `Clear()` + the
   Transform×RigidBodyDescriptor query loop.
2. **Runtime**: new `AssetResolve.{hpp,cpp}` —
   `ResolveMeshRendererAssets(MeshRenderer&, Render::AssetCache&, const
   Core::AssetDatabase&)` and a whole-scene loop
   `ResolveSceneAssets(Scene&, …)`. (Runtime already links Core + Render.)
3. **App**: new `LevelRuntime.{hpp,cpp}` composing 1+2 — the layer that needs
   both Physics and Runtime, which only App links:
   - `InstallAssetResolvers(cache, db)` — the `SetAssetIdHintResolver` +
     `AssetCache::SetAssetResolvers` wiring from `SandboxApp.cpp:161–170`.
     Without it a game's `ResolveMesh`/`ResolveMaterial` can't translate
     GUIDs to paths and `LoadLevel` loads nothing useful. (It currently
     lives inside the editor-only `ReimportAssets`; the editor keeps calling
     the engine version after each `Rebuild`.)
   - `RebindSceneAssetsAndPhysics(scene, cache, db, physics)`
   - `LoadLevel(scene, vpath, cache, db, physics, sceneRenderer&) -> bool`:
     `SceneSerializer::LoadFromFile` + `cache.Clear()` +
     `renderer.InvalidateAssetBindings()` + rebind. Editor-state resets
     (history wipe, eyedropper disarm, play-state reset) stay at the caller —
     note they thereby run *after* the rebind instead of before it as today;
     verified harmless (the rebind reads no editor state and OnFixedUpdate
     can't interleave), but check again at the diff.
   - **Header contract, prominently:** `LoadLevel` frees GPU assets
     (including the bindless table) that already-recorded draws may still
     reference — calling it mid-frame faults the GPU. It must run at a safe
     point (the main-thread drain / top of update), which is exactly why the
     editor marshals loads via `_pendingLevelLoad` + `Jobs().RunOnMain`
     (`SandboxLevels.cpp:52–64`, `SandboxApp.hpp:538–540`). This footgun
     becomes public API here; the doc comment is not optional.
   - `UpgradeStreamingAssets(scene, cache, db, bool& wereLoading)` — the
     per-frame re-resolve while `HasPendingLoads()`.
4. Sandbox call sites switch over; `SandboxPlay.cpp`'s and
   `SandboxAssetBrowser.cpp`'s local copies are deleted.

Interim caveat to document in the header: until asset-DB S5 (cooker/
`PakProvider`), a shipped game still builds its GUID→path index by scanning
sidecars at startup via `AssetDatabase::Rebuild`. Be precise about what that
means: **minting lives inside `Rebuild` itself** (missing sidecar → mint +
write, `AssetDatabase.cpp:253–263`, plus collision re-mint and authoring-root
mirroring) — so `Rebuild` is write-free only when every asset already has a
`.aast`; a game shipped with sidecar-less assets will write files at startup.
That's acceptable interim behavior (S5 replaces the scan with a baked index)
but it is not an editor/engine boundary — only the material reconcile
(`ReconcileMeshMaterials`) is genuinely editor-only.

### E1 — create `modules/Editor`, move `EditHistory` first — **DONE 2026-07-22**

Landed as planned; namespace is `Assisi::Editor`, the test suite is named
`Editor` (replacing `Sandbox`, count stays 8), and the library links only
Runtime + nlohmann until E2 grows it.

Smallest piece, proves the module + test wiring before the big move:

- `modules/Editor/{include/Assisi/Editor,src,tests}`, target `Assisi-Editor`,
  alias `Assisi::Editor`, links: `Assisi::App`, `imguizmo::imguizmo`,
  `implot::implot`, `nlohmann_json::nlohmann_json` (imgui comes via
  `Assisi::Debug` through App).
- `EditHistory.{hpp,cpp}` → `Assisi/Editor/EditHistory.{hpp,cpp}`; namespace
  `Sandbox` → `Assisi::Editor`.
- `TestEditHistory.cpp` → `modules/Editor/tests/`, keeping the current
  lightweight linkage (compile the source directly, link `Assisi::Runtime`,
  explicit `$<TARGET_OBJECTS:Assisi-ECS-Generated>` +
  `Assisi-Runtime-Generated`) so the suite doesn't drag Jolt/GLFW/ImGui. Keep
  the GCC `-Wno-maybe-uninitialized` scoping. CTest name stays distinct.
- `apps/sandbox/tests/` is deleted once empty.

### E2 — move the application: `SandboxApp` → `Editor::EditorApp` — **DONE 2026-07-22**

Landed as planned, plus two small engine additions the seam needed:
`Application::GetConfig()` (protected — the dirty-title marker now uses the
game.json title instead of a hardcoded "Assisi Sandbox") and
`SystemRegistry::HasRenderSystems()` (so OnStart can warn about game render
systems instead of silently never running them). `apps/sandbox` is main.cpp +
CMake only. Pending: a manual run (full editor loop).

The bulk move. `apps/sandbox/src/*` → `modules/Editor/src/` with renames
(`SandboxApp.*` → `EditorApp.*`, `SandboxInspector.cpp` →
`EditorInspector.cpp`, `SandboxImGui.hpp` → `ImGuiQueries.hpp`, etc.);
class `SandboxApp` → `Assisi::Editor::EditorApp`; header goes public at
`Assisi/Editor/EditorApp.hpp` (the per-TU split stays private in `src/`).
`EntitySelectionChangedEvent` moves with it.

The one new seam — the game hook:

```cpp
struct EditorConfig
{
    std::function<void(App::SystemRegistry &)> registerGameSystems; // may be null
    std::string startupLevel; // CLI -l passthrough
};
explicit EditorApp(EditorConfig config);
```

Game-registered systems tick **only while `PlayState::Playing`** (editor
systems — picking, camera, selection — keep running in every state, as
today). Implementation: game systems go into a second `SystemRegistry` run
behind the `IsSimulating()` gate, mirroring how `_physics.Update` is already
gated. Seam contract, stated up front:

- The editor runs the game registry's `Update` and `PostUpdate` phases from
  `OnUpdate` and its `FixedUpdate` phase from `OnFixedUpdate`. Anything else
  a game registers (`PreUpdate`, `RegisterRender`) either also runs there or
  the editor rejects/warns — silent never-runs are not acceptable.
- `.After()` ordering resolves within one registry only
  (`SystemRegistry.hpp:96–100`); editor systems and game systems cannot
  order against each other. Fine by design — say so in the header.
- Game code gets **no play-transition notification** in this branch
  (Start/Stop snapshot-restore happens around it, `SandboxPlay.cpp:74–191`);
  a game system holding entity handles across a play session has no hook to
  reset them. Deferred to Phase 2 alongside the rest of the game-side
  surface — recorded here so the seam isn't declared finished.

Sandbox passes a null hook — zero behavior change.

Deliberately kept in `EditorApp` for now (moves to the game side in Phase 2,
noted here so it isn't forgotten): the `game.json` input-binding load, the
Escape-to-quit, and the `ActionMap`.

After E2, `apps/sandbox/` is `main.cpp` (arg parsing + `EditorApp`
construction) and its CMakeLists — the exe target, asset staging, shader
compilation, and `assisi_link_reflections` stay exe-side, unchanged.

### E3 — make the engine's editor visuals opt-in — **DONE 2026-07-22**

Landed: `SceneRenderer::Initialize(const InitParams&)` with
`enableEditorVisuals` defaulting to false; overlay entry points no-op when
off; `RebuildPipeline` on never-built passes was already a safe no-op, so
render-target changes need no extra gating. The editor passes true.
Pending: the off-path visual check (local flag flip) rides along with the
manual run.

`SceneRenderer::Initialize` gains `bool enableEditorVisuals`. Its parameters
are positional today (`SceneRenderer.cpp:61–63`) — this is the excuse to give
it an `InitParams` struct like `MeshPass` already has, rather than growing the
positional list. When false: OutlinePass/IconPass/LinePass are not
initialized, no `assets/editor/**` shader or icon loads happen, and
`SubmitLines` / `SubmitOutline*` / `SetHighlightedEntity` /
`SetEditorIconsVisible` become no-ops. `EditorApp` passes true; the flag
defaults to **false** so the Phase 2 game template gets the cheap path
without knowing the flag exists.

Verifying the off-path without violating the "sandbox identical" invariant:
flip the flag in a local build (not committed) and confirm icons/outlines/
wireframes are gone and no editor asset loads are logged; the committed
sandbox always passes true. The off-path gets a real consumer in Phase 2.

Asset staging still copies `assets/editor/**` for every target; excluding it
from game targets belongs to Phase 2 (there is no game target yet to test
against) — noted in `remaining-work.md`, not done here.

### E4 — docs & close-out

- `template-conversion-plan.md`: Phase 3 decision recorded (library, this
  layout), Phase 2 rewritten against the two-target shape:
  `apps/game/{GameLib, Game, GameEditor}`, editor-asset exclusion, staging
  generalization (the prune/copy machinery is keyed on `Assisi-Sandbox`
  today), input bindings + example system into GameLib.
- `remaining-work.md` §5: Phase 3 closed; §2b/§4c cross-refs updated.
- Memory + this doc marked done.

## 3. Verification per stage

- `make gd` and clang-debug: **0 warnings** (the bar set on `hygiene/round6`).
- `ctest --preset gcc-debug` green; after E1 the suite count changes (sandbox
  suite replaced by the Editor one).
- Manual sandbox run after E0, E2, E3: load `levels/Materials.alvl`, verify
  meshes + physics + play/stop + undo + gizmo + browser thumbnails + F11.
- After E2: a fresh-clone configure+build (the `016181a` lesson — moves are
  exactly where untracked-file/partial-commit mistakes happen).
- End of branch: one ASan run of the sandbox while playing a level.

## 4. Risks / footguns

- **Biggest churn is includes and names, not logic** — the move itself is
  `git mv` + namespace/rename sweeps. Review diffs with `--find-renames`.
- **Static-init order**: moving TUs into a static library must not lose
  reflection registrations — they were never in sandbox TUs (all engine-side
  `*-Generated` objects, linked by the exe), so nothing changes; but the
  Editor *tests* rely on explicit generated-object linking — keep it.
- **`_thumbnailCache` / `DebugUI::ReleaseTexture`**: Editor calls Debug
  directly; fine (App already links Debug publicly), just don't let it become
  an excuse for Editor→Render backdoors that bypass App.
- **Protected `Application` API**: anything the options/diagnostics windows
  need must stay protected (not private) — no friend declarations.
- **Phase 2 temptation**: renaming `apps/sandbox` → `apps/game`, splitting
  GameLib, thinning the scene — all *next* branch. This branch ends with the
  sandbox as a thin `EditorApp` consumer, nothing else.
- **The two-target shape has a real constraint, name it now**: in
  `GameEditor`, the application object *is* `EditorApp` — the game's own
  `Application` subclass never runs there. Everything the user wants working
  in-editor must therefore be expressible as registered systems (plus the
  Phase 2 hooks). The `Game` target should itself be a thin systems-host over
  the same `GameLib` registration, or edit-time and ship-time behavior
  diverge. This shapes Phase 2's template layout and is the actual price of
  the Unreal shape.
- **`EditorApp` will need to grow config, not stay frozen**: Escape-to-quit,
  `game.json` loading, and the `ActionMap` move into the library where a
  template user can't edit them. Phase 2 reopens this via `EditorConfig`
  growth (not subclassing) — plan for the struct to gain fields rather than
  pretending the E2 shape is final.
