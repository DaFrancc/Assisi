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
  clears to the configured colour. No geometry is visible because at the time this was
  written there was no way to load a level under Vulkan (ImGui — and therefore the
  Levels panel — wasn't wired up for Vulkan yet; it is now, see section 3), so the
  scene is empty by construction. That's
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
- At the time this was written, this meant ImGui/the F12 options window/AA were all
  fully non-functional under Vulkan. **ImGui itself is back — see section 3's ImGui
  bullet, done.** The F12 options window and AA specifically are still gone (they were
  deleted along with the rest of `Application`'s post-process code, not just
  disconnected — see the post-process bullet in section 3).
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
  `src/Shader.cpp`) — **deleted**. It compiled GLSL at runtime via `glCreateShader`/
  `glUniform*`; its only remaining caller, `Runtime::LightingSystem::SetupMeshShader()`,
  is gone now that lighting has its own NVRHI port (below), so nothing references it
  anymore.
  - **The NVRHI-native replacement for the mesh-drawing path**:
    `Render::ShaderModule.hpp/cpp` (`LoadSpirvShader(device, path, stage) →
    nvrhi::ShaderHandle`, extracted so any future pipeline can reuse it — post-process,
    ImGui, compute) + `Render::MeshPass` (input layout + binding layout/set + graphics
    pipeline, i.e. the "pipeline + binding-layout wrapper" half).
- **Clustered forward lighting — done, and it's real: `Test.alvl`'s directional +
  three colored point lights now actually shade the scene** (previously the mesh
  shader had a single hardcoded directional light and `LightingSystem`/`ClusterGrid`
  were fully unused dead code — this was the first time any of it was exercised end
  to end, GL or Vulkan).
  - `Render::Buffer` (`Buffer.hpp/cpp`) — new generic fixed-capacity NVRHI structured
    buffer (SRV, optionally UAV), replacing the old GL SSBO wrapper stub. Capacity is
    fixed at `Create()` time (e.g. 1024 point lights, 1024 spot lights) and never
    resized — `Upload()` writes only the live prefix each frame, avoiding the
    per-frame `glNamedBufferData` reallocation the old GL version did. Distinct from
    `Render::MeshBuffer` (vertex/index only).
  - `Render::ComputeShader` — rewritten as an NVRHI compute-pipeline wrapper
    (`nvrhi::IComputePipeline` + a caller-supplied `BindingLayoutDesc`; `Dispatch()`
    takes the binding set and optional push constants). The old GL version's named
    uniform setters (`SetUVec3`, `SetMat4`, ...) have no NVRHI equivalent — Vulkan has
    no loose uniforms, so per-dispatch data now goes through push-constant blocks
    instead (see `cluster_build.comp`/`cluster_cull.comp`'s `layout(push_constant)`).
  - `Render::ClusterGrid` — ported to NVRHI: owns two `ComputeShader` instances
    (build, cull) and seven `Buffer`s (cluster AABBs, point/spot/dir light data, light
    index list, light grid, atomic global counters), all allocated once in
    `Initialize()`. `BuildClusters()`/`CullLights()` now take an `nvrhi::ICommandList*`
    instead of issuing GL calls directly.
  - **The SRV/UAV binding-offset split applies to structured buffers too, not just
    textures/samplers**: NVRHI's `VulkanBindingOffsets` puts `StructuredBuffer_SRV`
    in the same descriptor space as `Texture_SRV` (+0) and `StructuredBuffer_UAV` in
    its own space (+384) — confirmed by reading
    `vulkan-resource-bindings.cpp:getRegisterOffsetForResourceType`, not documented
    anywhere obvious. `cluster_build.comp` writes `clusterAABBs` as a UAV at
    `binding = 384`; `cluster_cull.comp` reads it back as an SRV at `binding = 0` in a
    *different* binding layout (build and cull are separate pipelines, so this isn't a
    collision) while writing its own UAV outputs at `384`/`385`/`386`.
  - `Runtime::LightingSystem` — ported: queries `PointLightComponent`/
    `SpotLightComponent`/`DirectionalLightComponent` from the scene exactly as
    before, but `Initialize()`/`Resize()`/`Update()` now take an
    `nvrhi::ICommandList*` and route through `ClusterGrid`'s NVRHI calls instead of GL.
  - **`MeshPass` redesigned, not just extended, to make room for lighting data**: push
    constants were maxed out at exactly 128 bytes (the portable Vulkan minimum)
    holding `modelViewProjection` + `model` (each needed — the latter for
    world-space position/normal in the lighting math); there was no room left for
    per-frame camera/cluster-grid data, so that moved into a proper constant buffer
    (`ConstantBuffer(0)`, +256 offset) updated once per frame via the new
    `MeshPass::UpdateFrameConstants()` rather than once per draw. Five
    `StructuredBuffer_SRV` bindings (slots 1-5, since slot 0 is the albedo texture)
    bind `ClusterGrid`'s light buffers directly — `MeshPass::Initialize` now takes a
    `const ClusterGrid&` it must outlive, and every cached per-texture binding set
    includes them.
  - `cube_min.vert/frag` gained real lighting: vertex outputs world position,
    transformed normal (`mat3(model) * normal`, uniform-scale assumption — no
    material system needs non-uniform scale yet), and view-space Z (for the
    logarithmic cluster Z-slice, computed from the constant-buffer view matrix rather
    than passing it as a second push-constant matrix). Fragment shader reuses the
    Cook-Torrance BRDF (GGX distribution, Smith geometry, Schlick Fresnel) from this
    engine's pre-migration `mesh.frag` (see git history, commit `445b077`), adapted
    for the current no-normal/metallic/roughness-maps reality with fixed
    `roughness = 0.6`, `metallic = 0.0` constants — a real material system for those
    maps is still future work, not reintroduced here. Directional lights loop
    unconditionally (never culled); point/spot lights look up their cluster's
    `LightGrid` entry and only iterate the lights actually assigned to it.
  - Verified against `assets/levels/Test.alvl` (pre-existing sun + three colored point
    lights, left over from the pre-migration GL renderer, never actually lit until
    now) — user confirmed correct shading including the colored point lights, and
    confirmed a window resize (which rebuilds the cluster AABBs via
    `SandboxApp::OnResize` → `LightingSystem::Resize`) stays stable.
- `Application::RebuildPostProcess`/the MSAA/FXAA post-process pass — **currently
  deleted outright**, not just dormant (see section 2's judgment-call notes). Needs
  NVRHI render-target + FXAA pass equivalents if brought back, or could stay dropped
  in favor of just shipping single-sample rendering — still an open decision.
- ImGui backend — **done**. `DebugUI` now uses `imgui_impl_glfw.cpp` (Vulkan-flavored
  init) + `imgui_impl_vulkan.cpp` against `VulkanContext`'s raw Vulkan handles (new
  `GetVkInstance()`/`GetVkPhysicalDevice()`/`GetVkDevice()`/`GetVkGraphicsQueue()`/
  `GetVkGraphicsQueueFamily()`/`GetSwapchainImageCount()`/`GetSwapchainFormat()`
  accessors). Multi-viewport (`ImGuiConfigFlags_ViewportsEnable`) deliberately left
  off — would need per-viewport swapchains, out of scope; docking stays on. No Vulkan
  SDK here, same as everywhere else in this migration, so `imgui_impl_vulkan.cpp` is
  built with `IMGUI_IMPL_VULKAN_NO_PROTOTYPES` (set target-wide on `Assisi-Debug`) and
  its function pointers are loaded via `ImGui_ImplVulkan_LoadFunctions` routed through
  the same global `VULKAN_HPP_DEFAULT_DISPATCHER` `VulkanContext.cpp` bootstraps.
  NVRHI's Vulkan backend turned out to use **dynamic rendering**
  (`vk::CommandBuffer::beginRendering`/`vkCmdBeginRendering` inside
  `CommandList::beginRenderPass`, not traditional `VkRenderPass`/`VkFramebuffer`
  objects) — confirmed by reading `vulkan-graphics.cpp`, not documented anywhere —
  so ImGui is initialized with `UseDynamicRendering = true` +
  `VkPipelineRenderingCreateInfoKHR` (color format from `GetSwapchainFormat()`, depth
  hardcoded to `VK_FORMAT_D24_UNORM_S8_UINT` matching `VulkanContext`'s depth texture)
  rather than a `VkRenderPass`.
  - **The empty-scene problem**: NVRHI's Vulkan backend only calls
    `vkCmdBeginRendering` from inside `ICommandList::setGraphicsState` (there's no
    lighter-weight public "just open the render target" call), and
    `clearTextureFloat`/`clearDepthStencilTexture` explicitly close any open render
    target before clearing outside a pass. So if the scene has nothing to draw (no
    entities → `MeshPass::Draw` never called → `setGraphicsState` never called), no
    render target is open for ImGui to draw into. Fixed with a tiny "opener" pipeline
    owned by `DebugUI` (`assets/shaders/imgui_opener.vert/frag` — trivial, no inputs,
    no bindings, never actually drawn with) that `DebugUI::BeginFrame` binds via
    `setGraphicsState` every frame before `OnRender()`/ImGui run, guaranteeing the
    target is always open. This mattered a lot in practice: an empty/fresh scene —
    exactly when you most need the Levels panel to load something — is the single
    most common state to hit this in.
  - **Real correctness bug found and fixed while verifying this, not really an ImGui
    bug**: dragging/resizing/focusing ImGui panels produced visible rendering
    corruption. Root cause: `VulkanContext` had no CPU-GPU frame-pacing (only
    per-image-index semaphores, no fences), so the CPU could start recording a new
    frame — including ImGui's `imgui_impl_vulkan.cpp`-owned vertex/index buffers,
    which it round-robins across frames on the assumption the caller throttles
    submission — while the GPU was still reading those same buffers from an earlier
    frame. Invisible with the mostly-static scene (section 1/section-3-textures
    milestones); glaringly visible the moment per-frame content actually varies a lot
    (dragging an ImGui window changes its vertex data every frame). Fixed with
    `nvrhi::IDevice::waitForIdle()` at the top of `VulkanContext::BeginFrame()` —
    correctness-first and simple, at the cost of CPU/GPU overlap; a real
    multi-frame-in-flight fence would get some of that back if it's ever needed.
    Also fixed a ~500ms-1s delay between clicking "Load" in the Levels panel and the
    level actually appearing, which turned out to be the same root cause.
- `Render::DefaultResources` / `Render::Texture2D` (material textures) — **done**.
  `Render::Texture` (`Texture.hpp/cpp`, header owns an `nvrhi::TextureHandle`, loads via
  `stb_image` — same vendored library the GL version used) replaces
  `OpenGL::Texture2D` (deleted, along with `OpenGL::DefaultTextures.hpp` — both fully
  unreferenced now). `DefaultResources::WhiteTexture(device)` replaces the GL
  singleton-texture-ID version; trimmed to just white for now (normal/black/grey
  return when normal maps and metallic/roughness are actually wired, not before —
  no unused fallbacks sitting around). `MeshRendererComponent`'s four raw-GL-uint
  texture ID fields collapsed to one `const Render::Texture *albedoTexture` (the
  other three had zero consumers anywhere in the engine — removed rather than
  left as dead GL-typed fields; they'll come back paired with real PBR/lighting
  work). `MeshPass` gained a texture+sampler binding (see its header comment: NVRHI's
  Vulkan backend keeps SRV and sampler as **separate descriptors** — HLSL
  t-register/s-register style, not GLSL's combined `sampler2D` — offset by
  `VulkanBindingOffsets` defaults, shaderResource at +0 and sampler at +128; GLSL
  shaders declare `texture2D`/`sampler` separately and combine them with
  `sampler2D(tex, samp)` at the point of use) plus a texture→`BindingSet` cache
  (binding sets reference concrete resources, so unlike the mesh — bound directly via
  vertex/index buffer bindings, no binding set needed — one binding set is needed per
  distinct texture). `cube_min.vert/frag` updated to pass UVs through and sample
  albedo. Proved out with a generated 256x256 checker PNG
  (`assets/textures/checker.png`) loaded by `SandboxApp` and assigned to every
  entity in `LoadLevel()` alongside the shared cube mesh.
  - **Two real Vulkan bugs found and fixed while verifying this, both invisible until
    there was real textured geometry with a distinguishable orientation to look at**:
    (1) The `projection[1][1] *= -1.f` Y-flip in `SandboxApp::OnRender()` (inherited
    from the vk_triangle spike, present since section 1) was **wrong and has been
    removed** — NVRHI's Vulkan backend already flips the viewport internally
    (`VKViewportWithDXCoords`, a negative-height viewport, standard Vulkan technique
    to match D3D's top-left-origin convention) to undo Vulkan's native Y-down clip
    space. The manual flip on top of that double-compensated, rendering upside down.
    (2) That same internal viewport flip also flips the winding order the rasterizer
    perceives, so with `cullMode = Back` and CCW-authored meshes (standard
    convention, matches `CreateUnitCubeMesh()`), back-face culling was culling the
    actual front faces — fixed by setting `renderState.rasterState.frontCounterClockwise
    = true` in `MeshPass::Initialize` (see its inline comment). Both bugs are
    detailed on `MeshPass`/`SandboxApp::OnRender`'s comments so they don't get
    silently reintroduced by future pipeline code — **any new NVRHI graphics pipeline
    in this codebase needs the same `frontCounterClockwise = true`, and must NOT
    apply a manual projection Y-flip.**

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
