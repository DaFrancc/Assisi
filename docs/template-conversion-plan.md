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

### Phase 2 — NEXT

Reshape `apps/sandbox` into the actual thin template:

- Single scene with a few cubes + a ground + a light.
- Fly camera, physics enabled, input bindings.
- One small example gameplay system so the template visibly *does* something out
  of the box.
- Simplify the sandbox's current two-scene setup (separate `_cameraScene` +
  `SceneRegistry`) down to a single scene where sensible.

This is the "download → name → runs into a blank scene with cubes" payoff.

### Phase 3 — editor tooling disposition

`apps/sandbox` currently also carries editor tooling: the reflection-based
inspector, level save/load, entity picking + eyedropper, and the diagnostics
window.

**OPEN DECISION (unanswered):** preserve it as a separate `apps/editor`
(recommended — it's recent, real work, and matches the editor-tooling-vs-game-
project split) **or** shelve it in git history. Decide before stripping the
template down.

### Later (not scoped yet)

- Project generation/naming tooling (the literal "name your project" flow).
- Possibly grow `Application` so the template's bring-up is even thinner, and/or a
  convenience `SceneRenderer::Render(frame, scene)` overload that finds the camera
  in the scene.

## Related

- Round-2 code review burndown: `docs/old/code-review-2026-07-round2.md` (this is the
  follow-on to the "GameApplication dead layer" and asset-resolution items).
