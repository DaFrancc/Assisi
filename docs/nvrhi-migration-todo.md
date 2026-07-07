# OpenGL → NVRHI Migration TODO

Status as of 2026-07-07. This branch (`dev`, working toward a dedicated NVRHI backend)
is a **one-way conversion, not a dual-backend toggle** — the end state has zero OpenGL
left anywhere in the engine. Everything below assumes that goal.

## Done so far

- NVRHI + Vulkan-Headers + glslang wired into CMake via `FetchContent`.
- `Render::Vulkan::VulkanContext` owns real instance/device/swapchain/depth-buffer
  bring-up (`modules/Render/include|src/.../Vulkan/VulkanContext.*`), including resize
  handling. Uses `vk::detail::DynamicLoader` + `VULKAN_HPP_DEFAULT_DISPATCHER` since
  there's no Vulkan SDK installed in this dev environment (driver-only) — see that
  file's header comment for the exact pattern, mirrors NVIDIA's Donut framework.
- `apps/vk_triangle` — standalone spike, proved the whole pattern before it went into
  the real engine. Now that the real engine has it working, this can probably be
  deleted (or kept as a minimal reference — worth deciding, low stakes either way).
- A hardcoded test cube renders correctly through the real `Assisi-Sandbox` app
  (`SetupVulkanCube()`/`OnRenderVulkan()` in `apps/sandbox/src/main.cpp`) — this proved
  vertex/index buffers, push-constant matrices, depth-tested pipelines, and SPIR-V
  shader compilation all work end-to-end. **This code is scaffolding, not the final
  design** — the real renderer needs to draw whatever the ECS scene contains (arbitrary
  meshes/entities from level files), not one hardcoded mesh. See "Real scene rendering"
  below for the actual fix.
- A dual-backend toggle (`AppConfig::backend`, `GraphicsBackend::OpenGL` vs `Vulkan`)
  exists right now purely because OpenGL and Vulkan needed to coexist *during* the
  migration so the working app didn't break. **This entire toggle should be deleted**
  once OpenGL is gone — see "Remove the toggle" below. `assets/game.json` now sets
  `render.backend: "vulkan"` so the sandbox runs the real path by default; the C++
  default in `AppConfig.hpp` is still `OpenGL` pending that section.
- **Real scene rendering is done, NVRHI-native (not an OpenGL translation)** — see
  "1. Real scene rendering" below for what shipped and why the original bolt-on plan
  was abandoned mid-flight.

## Remaining work, roughly in dependency order

### 1. Real scene rendering — DONE (redesigned NVRHI-native, not the OpenGL bolt-on)

The original plan here (adding `GetSourceData()` to `OpenGL::MeshBuffer` so the Vulkan
path could read back CPU mesh data through it) turned out to be dead on arrival: under
the Vulkan backend the window is created with `CreateClientApiContext = false` and
`RenderSystem::InitializeOpenGL` (which loads glad's GL function pointers) never runs.
Calling `OpenGL::MeshBuffer::Upload()` — raw `glGenVertexArrays`/`glBufferData` — under
Vulkan would call null function pointers. Decision made when this was hit: skip the
interim bolt-on and go NVRHI-native immediately, accepting that this breaks the OpenGL
scene-rendering path outright rather than keeping it limping. What shipped instead:

- `Assisi::Render::MeshBuffer` (`modules/Render/include/Assisi/Render/MeshBuffer.hpp`,
  header-only) — NVRHI vertex + index buffer pair, retains the source `MeshData`.
  Lives directly under `Assisi::Render`, not `Render::Vulkan`, since NVRHI resource
  creation is graphics-API-agnostic once you have an `nvrhi::IDevice*`.
- `Assisi::Render::MeshPass` (`MeshPass.hpp`/`src/MeshPass.cpp`) — owns the one shared
  graphics pipeline (input layout, push-constant binding layout/set, `cube_min.vert/frag`)
  and a `Draw(commandList, framebuffer, w, h, mvp, MeshBuffer&)` call. Shader SPIR-V is
  still read via a raw `ReadSpirvFilePath` (not `AssetSystem::ReadBinary`) because the
  build places `.spv` output next to the executable, not under `assets/` — see section 4.
- `MeshRendererComponent::mesh` (`modules/Runtime/include/Assisi/Runtime/Components.hpp`)
  is now `const Render::MeshBuffer*`. Since the mesh is uploaded once (at scene-setup
  time, not lazily per-draw), **no separate mesh cache was needed** — simpler than the
  original plan, which only needed a cache because it was reading CPU data back out of
  an already-GL-uploaded buffer.
- `Runtime::DrawScene` (`Renderer.hpp`/`Renderer.cpp`) was rewritten to be NVRHI-only:
  iterates `Query<TransformComponent, MeshRendererComponent>()`, computes
  `projection * view * transform.worldMatrix` per entity, calls `MeshPass::Draw`. The
  old OpenGL version (raw `glDrawElements`, per-draw texture binding) is gone — it
  couldn't coexist with the new mesh type. Materials/textures are still unwired
  (`MeshRendererComponent`'s texture ID fields are dead for now) — deferred as planned.
- `SandboxApp` (`apps/sandbox/src/main.cpp`): `SetupVulkanCube()` → `SetupScene()`
  (builds `_meshPass` + uploads the shared `_cubeMesh`), `SetupCamera()` now runs
  unconditionally in `OnStart()`, `OnRenderVulkan()` computes view/projection from the
  real camera entity every frame (no cached `_projection` member — recomputed via
  `Runtime::ProjectionMatrix()` each use, including in `PickEntity()`). The OpenGL
  scene-rendering path (`_shader`, `_lighting`, GL `_cubeMesh`, `SetupLighting()`) was
  deleted outright rather than left half-working; `OnRender()` (the OpenGL hook) is now
  an intentional no-op.
- **Follow-up bug found by actually running it**: registering `EntityPicking`/
  `CameraController` systems unconditionally (previously skipped under Vulkan via the
  early return this change removed) exposed that `ImGui::GetIO()` is called from
  `UpdateCamera()`/`HandleEntityPicking()`/`OnUpdate()` with no ImGui context under
  Vulkan (`Debug::DebugUI::Initialize()` — which calls `ImGui::CreateContext()` — only
  runs for the OpenGL backend; see section 3's ImGui bullet). Fixed with two small
  guarded helpers, `ImGuiWantsMouse()`/`ImGuiWantsKeyboard()` (local to `main.cpp`),
  that check `ImGui::GetCurrentContext() != nullptr` first. Camera movement and entity
  picking now work under Vulkan even with no ImGui overlay.
- **Verified by running the built app**: no crash, camera systems tick every frame,
  clears to the configured colour. No geometry is visible because there's currently no
  way to load a level under Vulkan (ImGui — and therefore the Levels panel — isn't
  wired up for Vulkan yet, see section 3), so the scene is empty by construction. That's
  expected, not a bug.

### 2. Remove the OpenGL/Vulkan toggle entirely — DONE

All bullets below landed as planned:

- Deleted `AppConfig::backend` and the `render.backend` parsing in `AppConfig.cpp`.
  `game.json`'s `render.backend` key is gone too (Vulkan is simply the only option now).
- Deleted `Render::Backend::GraphicsBackend` entirely (the enum header, not just the
  field) — `RenderSystem::Initialize(window)` now always sets up Vulkan, no parameter.
- Deleted `RenderSystem::InitializeOpenGL` and `modules/Render/src/RenderSystemOpenGL.cpp`.
- Deleted every `if (isOpenGL)` / `GetBackend() == OpenGL` guard in `Application.cpp`
  and `apps/sandbox/src/main.cpp`; deleted `Application::GetBackend()` itself.
- Collapsed `Application::RenderFrame()` + `RenderFrameVulkan()` into one `RenderFrame()`
  (body is what `RenderFrameVulkan()` used to be). Collapsed the two render hooks into
  one: `Application::OnRender(Render::Vulkan::VulkanFrame&)` is now pure virtual
  (replaces both the old no-arg `OnRender()` and `OnRenderVulkan()`); `SandboxApp` and
  `GameApplication` updated to match (`GameApplication::OnGameRender` now also takes the
  frame, so a future game can actually record draws from it — see its updated docstring
  example).
- `Window::WindowConfiguration::CreateClientApiContext` is gone; `WindowContext` always
  hints `GLFW_CLIENT_API, GLFW_NO_API`. `WindowContext::SwapBuffers()` was deleted
  outright (Vulkan presents via `VulkanContext::EndFrame()`, not `glfwSwapBuffers`).
  `SetVSyncEnabled()`/`IsVSyncEnabled()` were kept (still called from `Application::Run()`)
  but now just store the preference — vsync isn't wired to the Vulkan swapchain's present
  mode yet, that's still open.

**Judgment calls made along the way, not explicitly spelled out above:**
- `Application`'s MSAA/FXAA post-process (`_mainFB`/`_resolveFB`/`_screenQuad`/
  `_fxaaShader`, `RebuildPostProcess()`, the F12 `DrawOptionsWindow()` overlay, and the
  `Debug::DebugUI::Initialize/Shutdown/BeginFrame/EndFrame` calls that drove it) were
  **deleted from `Application`**, not just left dead — they only existed to serve the
  now-gone OpenGL `RenderFrame()` branch, and kept as OpenGL-typed members they'd be
  "a hint of OpenGL left" in the core base class, which earlier direction ruled out.
  `OptionsConfig`/`AaMode` (`modules/App/include|src/OptionsConfig.*`) and
  `modules/Debug/DebugUI.*` themselves were **left in place, just unwired** — unlike the
  post-process glue, these are standalone modules explicitly slated for an NVRHI port
  (ImGui backend is section 3's own bullet below), not dead ends.
- This means **ImGui/the F12 options window/AA are now fully non-functional** under the
  single remaining (Vulkan) backend, not just "OpenGL-only" as before — there is
  currently no way to see any ImGui UI at all in the running app (confirmed: only the
  clear-colored viewport renders, verified by actually launching `Assisi-Sandbox.exe`).
  Restoring any of this requires section 3's ImGui-Vulkan port.
- `apps/vk_triangle/src/main.cpp` had one stale `windowConfig.CreateClientApiContext =
  false;` line (from before this section) that needed deleting to keep building — it
  still builds and links standalone, untouched otherwise.

### 3. Delete OpenGL rendering code

- `modules/Render/include/Assisi/Render/OpenGL/` — entire folder (`Framebuffer.hpp`,
  `MeshBuffer.hpp`, `Texture2D.hpp`, `DefaultTextures.hpp`, `ScreenQuad.hpp`).
  `OpenGL::MeshBuffer.hpp` specifically is already fully unreferenced by the engine
  (superseded by `Render::MeshBuffer`, see section 1) — safe to delete any time,
  doesn't need to wait for the rest of this list.
- `modules/Render/src/RenderSystemOpenGL.cpp`.
- `Render::Shader` (`modules/Render/include/Assisi/Render/Shader.hpp` +
  `src/Shader.cpp`) — currently compiles GLSL at runtime via `glCreateShader`/
  `glUniform*`. Needs an NVRHI-native replacement (shader + pipeline + binding-layout
  wrapper).
- `Render::ComputeShader` / `Render::ClusterGrid` (`ClusterGrid.cpp`,
  `cluster_build.comp`, `cluster_cull.comp`) — clustered light-culling compute
  pipeline, currently GL compute shaders. Needs an NVRHI compute pipeline port.
- `Render::Buffer` (`Buffer.hpp`/`Buffer.cpp`) — GL uniform/storage buffer wrapper for
  the clustered-lighting SSBOs, needs an NVRHI buffer wrapper. Distinct from
  `Render::MeshBuffer` (vertex/index only, done in section 1) — this one is for
  arbitrary structured/storage data.
- `Render::DefaultResources` (`DefaultResources.hpp/cpp`) — default white/black/
  flat-normal/grey placeholder textures as raw GL texture IDs → NVRHI textures.
- `Runtime::Renderer.cpp`'s `DrawScene` — **done**, see section 1. Already NVRHI-only.
- `Runtime::LightingSystem` — manages clustered-lighting framebuffers/shaders, GL-only
  today. Needs a full NVRHI port (compute-based light culling + a light data buffer).
- `Application::RebuildPostProcess`/the MSAA/FXAA post-process pass in `RenderFrame()`
  — currently built on `OpenGL::Framebuffer`. Needs NVRHI render-target + FXAA pass
  equivalents (or could be dropped initially and re-added later — worth deciding
  priority vs. just shipping single-sample rendering first).
- ImGui backend: `imgui_impl_opengl3`/`imgui_impl_glfw`-for-OpenGL in
  `modules/Debug/src/DebugUI.cpp` → needs `imgui_impl_vulkan.cpp` wired against the
  raw `VkDevice`/`VkCommandBuffer` NVRHI wraps (fetch native handles via
  `getNativeObject`), or a small custom NVRHI-based ImGui renderer. No official
  "ImGui + NVRHI" backend exists upstream, so this is real work, not just relinking.
- `Render::Texture2D` (OpenGL texture wrapper, used for material textures loaded via
  `stb_image`) → NVRHI texture + sampler equivalent.
- `MeshRendererComponent`'s texture ID fields (`albedoTextureId` etc., currently raw GL
  `unsigned int` handles in `modules/Runtime/include/Assisi/Runtime/Components.hpp`) →
  NVRHI texture handles or an index into a texture registry.

### 4. Shader compilation pipeline

Right now GLSL→SPIR-V compilation is wired ad hoc per-app (`apps/vk_triangle`,
`apps/sandbox`'s `cube_min.vert/frag`) via a CMake custom command calling
`glslang-standalone`. Once every shader (`mesh.vert/frag`, `fxaa.frag`, `screen.vert`,
`cluster_build.comp`, `cluster_cull.comp`) needs this, decide:
- **Build-time compilation for everything** (current approach, extended to cover all
  shaders) — simple, but no shader hot-reload without a rebuild.
- **Runtime compilation via glslang's library API** (not just the standalone tool) —
  keeps shader edit/reload working without a full rebuild, more integration work.

Worth an explicit decision once real content development starts — not urgent while
still proving the pipeline out with hardcoded/test shaders.

### 5. Vendor/dependency cleanup (final pass, do last)

- Remove `glad` (vendored OpenGL loader) from `vendor/` and CMake once nothing
  references it.
- Remove `find_package(OpenGL)` / `OpenGL::GL` linkage in `apps/sandbox/CMakeLists.txt`.
- Double check nothing else pulls in raw `<GL/...>` or `glad/glad.h` anywhere
  (`grep -rl "glad/glad.h" modules apps` should come back empty at the end).

### 6. Housekeeping

- Decide the fate of `apps/vk_triangle` (delete now that the real engine works, or keep
  as a minimal reference/smoke-test app).
- Update module-level docs/comments that still describe the OpenGL renderer once the
  port is done.
- Confirm branch strategy — user referred to "the nvrhi branch"; currently this work is
  on `dev` (no separate `nvrhi` branch exists yet as of this writing).

## Key gotchas to remember (carried over from earlier in this migration)

- **No Vulkan SDK installed here** — only the driver's runtime `vulkan-1.dll`. Any new
  Vulkan-touching code needs to go through `VULKAN_HPP_DEFAULT_DISPATCHER` (bootstrapped
  via `vk::detail::DynamicLoader`), not statically-linked `vkCreateInstance`-style
  symbols. See `VulkanContext.cpp`'s header comment for the exact sequencing.
- **Crash output is unreliable** — the crash handler in `Application.cpp` logs before
  flushing, and SEH tears the process down before buffers flush, so a real crash can
  produce a `crash.dmp` with **zero** accompanying log/console text. Bisect with raw
  `std::fprintf(stderr, ...); std::fflush(stderr);` checkpoints instead of trusting
  `Core::Log`/`assisi.log` during crash debugging.
- **`FramebufferSizeCallback`-style callbacks can fire before app state is ready** —
  GLFW can trigger the framebuffer-resize callback during window creation/show (e.g. a
  DPI-driven `WM_SIZE` on Windows), before `Application`'s constructor finishes. Any
  future callback needs to guard on full initialization up front, not just piecemeal —
  this exact bug caused an intermittent crash earlier in the migration.
- Dev machine has two GPUs (Intel Iris Xe integrated, NVIDIA GeForce MX450 discrete) —
  good for catching vendor-specific driver quirks, since NVRHI's own test coverage
  skews NVIDIA-centric.
