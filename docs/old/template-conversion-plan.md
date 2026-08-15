# Engine-as-template conversion — plan & progress

Working branch: `cleanup/jul-08-26`.

## Product direction

Assisi is going the **Unreal route**: each game is its own project.

- **Each game is its own project.** (Dropped the earlier "multiple projects share
  one engine copy" idea — unnecessary.) Just: engine source + a sensible-default
  project you build on.
- **No `AGameModeBase`-style subclassing.** You do NOT subclass an engine base
  class to make a game — you **adopt the template project as-is and edit it
  directly**. Minimize inheritance ceremony. (This is why `GameApplication`, the
  subclass-style middle layer, was deleted rather than given a consumer.)
- **The engine is a library that does nothing on its own; the application calls
  into it.** The engine provides the loop + all the basics any game needs,
  pre-wired (physics step, transform propagation, cleanup, input, camera, a scene
  with a few cubes).
- **The template is NOT a bare hello-world `main`** — it's a real, working
  starting point. Split along that seam: reusable machinery in the engine,
  meaningful game code in the template.
- **North star:** download the engine, name the project, and it runs immediately
  into a blank scene with a few cubes — then you start building.

The sandbox (`apps/sandbox`) was scratch space for testing engine features; it is
being converted into the default/blank template project.

## Backend naming decision

**Neutral names now, Vulkan-only implementation.** NVRHI can target
D3D11/D3D12/Vulkan; only `VulkanContext` (device/swapchain bring-up) is actually
backend-specific — the rest of the render code is already `nvrhi::`-generic. So
app-facing render types are named neutrally (`RenderFrame`, `SceneRenderer`) to
keep a future D3D12 backend from churning template/game code. No D3D work planned
now. Vulkan is Windows+Linux from one backend; D3D would be Windows-only polish.

## Roadmap

### Phase 1 — DONE (pushed to origin)

- `VulkanFrame` → `Render::RenderFrame`: backend-neutral per-frame handle (its
  fields are all `nvrhi::` interfaces). Now in `modules/Render/include/Assisi/Render/RenderFrame.hpp`.
- Extracted `Runtime::SceneRenderer` (`Initialize` / `Resize` /
  `OnRenderTargetsChanged` / `Render`). It owns the clustered lighting + mesh
  pipeline + cluster-projection tracking, derives projection internally from the
  camera and frame size, and propagates the scene's transforms before drawing. A
  game's `OnRender` is now a single
  `Render(frame, scene, cameraTransform, camera)` call. The sandbox was thinned to
  use it.

⚠️ **Pending visual verify (graphics change):** run the sandbox and confirm it
still looks identical — lit cubes, fly camera (RMB-look + WASD, scroll zoom),
F12 MSAA/FXAA toggle (exercises pipeline rebuild), window resize (exercises the
froxel-grid rebuild — no rectangular lighting artifacts).

### Phase 3 — editor tooling disposition — **DECIDED & EXECUTED 2026-07-22**

(Resolved out of order because Phase 2 was blocked on it. Full analysis and
staging: `docs/editor-extraction-plan.md`, branch `extract-editor`.)

**Decision: the editor is a library — `modules/Editor` (`Assisi::Editor`) —
not a separate `apps/editor` executable.** Reflection registration is
link-time and per-binary (`assisi_link_reflections` gathers OBJECT libraries
at each final link), so a standalone editor exe could never inspect the game
components a template user writes. Instead, the Unreal shape: each project
builds `Game` (no editor code in the link) and `GameEditor` (links
`Assisi::Editor` and sees every game component for free).

What landed (stages E0–E3 of the plan doc):

- **E0** — the level runtime a *game* needs moved out of the sandbox into the
  engine: `PhysicsWorld::AddBodyFromDescriptor`/`RebuildSceneBodies`,
  `Runtime::AssetResolve`, `App::LevelRuntime`
  (`InstallAssetResolvers`/`LoadLevel`/`UpgradeStreamingAssets`). Before
  this, a game built on `Assisi::App` loaded levels with no meshes and no
  physics.
- **E1/E2** — `SandboxApp` became `Assisi::Editor::EditorApp`, moved
  wholesale into the library. `apps/sandbox` is now `main.cpp` + CMake. The
  game-side seam is `EditorConfig::registerGameSystems` — game systems tick
  only while Playing (contract documented at the declaration).
- **E3** — `SceneRenderer`'s editor overlays (outline/icons/lines) are
  opt-in via `InitParams::enableEditorVisuals`, default off, so a game
  never builds those pipelines or touches `assets/editor/**`.

### Phase 2 — NEXT (now unblocked)

Reshape `apps/sandbox` into the actual template — now with the two-target
shape Phase 3 settled:

- `apps/game/` (renamed from sandbox): `GameLib` (the user's code — systems,
  components, a `RegisterGameSystems(SystemRegistry&)`), a thin `Game` exe,
  and a `GameEditor` exe that constructs `Editor::EditorApp` with the game's
  hooks. Both exes call `assisi_link_reflections`.
- Single scene with a few cubes + a ground + a light; fly camera; physics;
  one small example gameplay system so the template visibly *does* something
  out of the box (and proves the play-mode seam).
- Move `game.json` input-binding load, Escape-to-quit, and the `ActionMap`
  from `EditorApp` to the game side (via `EditorConfig` growth, not
  subclassing).
- Game-side play-transition lifecycle hooks (start/stop notification) — the
  seam deliberately deferred them.
- Exclude `assets/editor/**` from the `Game` target's staged assets;
  generalize the staging/prune CMake (keyed on `Assisi-Sandbox` today).
- Simplify the two-scene setup down to a single scene where sensible.

This is the "download → name → runs into a blank scene with cubes" payoff.

### Later (not scoped yet)

- Project generation/naming tooling (the literal "name your project" flow).
- Possibly grow `Application` so the template's bring-up is even thinner, and/or a
  convenience `SceneRenderer::Render(frame, scene)` overload that finds the camera
  in the scene.

## Related

- Round-2 code review burndown: `docs/old/code-review-2026-07-round2.md` (this is the
  follow-on to the "GameApplication dead layer" and asset-resolution items).
