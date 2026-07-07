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
  once OpenGL is gone — see "Remove the toggle" below.

## Remaining work, roughly in dependency order

### 1. Real scene rendering (the immediate next step, in progress when paused)

The blocker: `MeshRendererComponent::mesh` (in `modules/Runtime/include/Assisi/Runtime/Components.hpp`)
is typed `const Render::OpenGL::MeshBuffer*`, and `OpenGL::MeshBuffer` (in
`modules/Render/include/Assisi/Render/OpenGL/MeshBuffer.hpp`) does **not** retain the
CPU-side `MeshData` after uploading to the GPU — only the GL handles + index count. So
there's currently no way to go from an entity's mesh pointer back to the geometry needed
to build an NVRHI buffer.

Today, every entity in the sandbox scene actually points at the same single
`_cubeMesh` (`apps/sandbox/src/main.cpp:799`, `mrc.mesh = &_cubeMesh;`) — so "multiple
different meshes per entity" isn't exercised yet even on the OpenGL side, but the
`DrawScene`/ECS query design (`modules/Runtime/src/Renderer.cpp`) is already generic
over however many entities/meshes exist. The Vulkan path needs to match that, not
hardcode a single mesh/transform.

Plan (was about to start when paused):
- Add `GetSourceData()` to `OpenGL::MeshBuffer` (retain a copy of the `MeshData` it was
  built from) — small, low-risk change, unlocks everything else.
  - **Superseded if OpenGL is deleted first** (see below) — if `MeshBuffer` is rebuilt
    NVRHI-native from the start, it should just retain/own the source `MeshData`
    directly rather than needing this bolted on.
- Build `Render::Vulkan::MeshBuffer` (or just `Render::MeshBuffer` once OpenGL is gone):
  NVRHI vertex + index buffers from `MeshData`, mirroring the existing class's API
  shape (`IndexCount()`, etc).
- Write `Runtime::DrawSceneVulkan` (mirrors `Runtime::DrawScene`): same
  `Query<TransformComponent, MeshRendererComponent>()`, real camera view/projection,
  real per-entity `transform.worldMatrix`, one shared pipeline/shader for the whole
  scene (matching how `DrawScene` takes one `Shader&`, not per-entity shaders).
  - Needs a mesh-buffer cache (map from CPU mesh identity → lazily-created NVRHI
    buffers) so meshes aren't re-uploaded every frame.
  - Materials/textures (albedo/normal/metallic/roughness) are explicitly **deferred** —
    first pass should get real geometry + real transforms + real camera rendering
    correctly using the existing unlit/simple-lit `cube_min` shader approach, extended
    to take a shared pipeline instead of a one-off hardcoded cube.
- Re-enable `SetupCamera()` for the Vulkan path in `SandboxApp::OnStart()` (currently
  skipped along with everything else GL-specific) so the real camera entity drives
  view/projection instead of the hardcoded `lookAt`/`perspective` in `OnRenderVulkan()`.
- Delete `SetupVulkanCube()`/the hardcoded cube members once `DrawSceneVulkan` replaces
  it.

### 2. Remove the OpenGL/Vulkan toggle entirely

Per direction: no more "pick a backend" — this is Vulkan-only from here.

- Delete `AppConfig::backend` field and the `render.backend` parsing in `AppConfig.cpp`.
- Delete `Render::Backend::GraphicsBackend` enum, or collapse it away entirely —
  `RenderSystem::Initialize` shouldn't take a backend parameter anymore, it should just
  always set up Vulkan.
- Delete `RenderSystem::InitializeOpenGL` and `modules/Render/src/RenderSystemOpenGL.cpp`.
- Delete all the `if (isOpenGL)` / `GetBackend() == OpenGL` / `GetBackend() != Vulkan`
  guards added in `Application.cpp` and `apps/sandbox/src/main.cpp` during the
  transition — once there's only one path, these become dead branches.
- Collapse `Application::RenderFrame()` + `RenderFrameVulkan()` back into one function;
  same for `OnRender()` vs `OnRenderVulkan()` — one hook, not two.
- `Window::WindowConfiguration::CreateClientApiContext` becomes always-false — probably
  delete the flag and hardcode `GLFW_NO_API` in `WindowContext`.
- Remove `Application`'s `GetBackend()` accessor once nothing branches on it.

### 3. Delete OpenGL rendering code

- `modules/Render/include/Assisi/Render/OpenGL/` — entire folder (`Framebuffer.hpp`,
  `MeshBuffer.hpp`, `Texture2D.hpp`, `DefaultTextures.hpp`, `ScreenQuad.hpp`).
- `modules/Render/src/RenderSystemOpenGL.cpp`.
- `Render::Shader` (`modules/Render/include/Assisi/Render/Shader.hpp` +
  `src/Shader.cpp`) — currently compiles GLSL at runtime via `glCreateShader`/
  `glUniform*`. Needs an NVRHI-native replacement (shader + pipeline + binding-layout
  wrapper).
- `Render::ComputeShader` / `Render::ClusterGrid` (`ClusterGrid.cpp`,
  `cluster_build.comp`, `cluster_cull.comp`) — clustered light-culling compute
  pipeline, currently GL compute shaders. Needs an NVRHI compute pipeline port.
- `Render::Buffer` (`Buffer.hpp`/`Buffer.cpp`) — GL uniform/storage buffer wrapper,
  needs an NVRHI buffer wrapper (may partly already be covered by whatever
  `Render::Vulkan::MeshBuffer`/a general buffer helper ends up looking like).
- `Render::DefaultResources` (`DefaultResources.hpp/cpp`) — default white/black/
  flat-normal/grey placeholder textures as raw GL texture IDs → NVRHI textures.
- `Runtime::Renderer.cpp`'s `DrawScene` — raw GL calls (`glActiveTexture`,
  `glBindTexture`, `glDrawElements`) → replaced by whatever `DrawSceneVulkan` becomes
  (see section 1); once OpenGL is gone this just becomes `DrawScene` again.
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
