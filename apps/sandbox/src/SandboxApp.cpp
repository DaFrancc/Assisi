/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include "SandboxApp.hpp"
#include "SandboxImGui.hpp"

#include <Assisi/Core/AssetIdJson.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Geometry/AssetImport.hpp>
#include <Assisi/Render/RenderSystem.hpp>
#include <Assisi/Render/Vulkan/VulkanContext.hpp>
#include <Assisi/Runtime/Camera.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/Hierarchy.hpp>
#include <Assisi/Window/Key.hpp>

#include <imgui.h>

#include <nlohmann/json.hpp>

#include <cctype>
#include <cstddef>
#include <expected>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

// ---------------------------------------------------------------------------
// Setup helpers
// ---------------------------------------------------------------------------

void SandboxApp::SetupCamera()
{
    const glm::vec3 camPos{5.f, 5.f, 10.f};
    const glm::vec3 forward = glm::normalize(-camPos);

    _pitch = glm::degrees(glm::asin(forward.y));
    _yaw   = glm::degrees(glm::atan(forward.z, forward.x));

    const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3{0.f, 1.f, 0.f}));
    const glm::vec3 up    = glm::normalize(glm::cross(right, forward));

    _cameraTransform.position = camPos;
    _cameraTransform.rotation = glm::quat_cast(glm::mat3(right, up, -forward));
    // _camera keeps its header default (60 deg FOV, 0.1..200 clip, active).
    RefreshCameraMatrix();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void SandboxApp::OnStart()
{
    // Load action bindings from game.json
    {
        const auto pathResult = Assisi::Core::AssetSystem::Resolve("game.json");
        if (pathResult)
        {
            if (std::ifstream file(pathResult.value()); file.is_open())
            {
                try
                {
                    const auto json = nlohmann::json::parse(file);
                    if (json.contains("input") && json.at("input").contains("actions"))
                        _actions.LoadFromJson(json.at("input").at("actions"));
                }
                catch (const nlohmann::json::exception &e)
                {
                    Assisi::Core::Log::Warn("Failed to parse input bindings from game.json: {}", e.what());
                }
            }
        }
    }

    auto mainScene = _scenes.Create("Main");
    if (!mainScene)
    {
        // Abort for real: without a scene the per-frame hooks have nothing to
        // run on. RequestClose() here means Run()'s first ShouldClose() check
        // fails and the loop body never executes; the _scene guards in
        // OnFixedUpdate/OnUpdate are defense-in-depth for that same null.
        Assisi::Core::Log::Error("Failed to create the main scene; aborting startup");
        RequestClose();
        return;
    }
    _scene = *mainScene;

    // Editor reconcile pass: give every asset a `.aast` GUID sidecar and build
    // the GUID→path database. Runs here (not in Application) because it is
    // editor-only — a shipped game consumes a baked index, never scans/writes.
    ReimportAssets();

    SetupCamera();
    SetupScene();
    ScanLevels();

    // --- Systems ---
    _systems.Register(Assisi::App::SystemPhase::Update, "EntityPicking",
                      [this](Assisi::App::SystemContext &) { HandleEntityPicking(); });

    _systems.Register(Assisi::App::SystemPhase::Update, "CameraController",
                      [this](Assisi::App::SystemContext &ctx) { UpdateCamera(ctx.dt); })
        .After("EntityPicking");

    _systems.Register(Assisi::App::SystemPhase::PostUpdate, "ProcessEntitySelection",
                      [this](Assisi::App::SystemContext &ctx)
                      {
                          for (const auto &e : ctx.events.Read<EntitySelectionChangedEvent>())
                          {
                              _selectedEntity = e.entity;
                          }
                      });
}

void SandboxApp::ReimportAssets()
{
    const std::expected<std::size_t, Assisi::Core::AssetError> result = _assetDatabase.Rebuild();
    if (!result)
    {
        Assisi::Core::Log::Warn("Asset reimport failed: asset root unavailable.");
        return;
    }
    Assisi::Core::Log::Info("Asset reimport: {} assets indexed (GUID sidecars reconciled).", *result);

    // Wire the (rebuilt) database into the two places that translate ids:
    //   - serialization's path hint (D2): saved GUID references carry a readable
    //     last-known path regenerated from the database.
    //   - the asset cache: id↔path so mesh/material/texture resolution and glTF
    //     import speak GUIDs. Reserved built-ins resolve without the database.
    // The lambdas capture `this`, so re-running reimport reuses the same (now
    // freshly rebuilt) database — no need to reinstall, but harmless if we do.
    Assisi::Core::SetAssetIdHintResolver([this](const Assisi::Core::AssetId &id)
                                         { return _assetDatabase.PathFor(id).value_or(std::string{}); });
    _assetCache.SetAssetResolvers(
        [this](const Assisi::Core::AssetId &id) -> Assisi::Core::AssetPath
        {
            const std::optional<std::string> path = _assetDatabase.PathFor(id);
            return path ? Assisi::Core::AssetPath{std::string_view{*path}} : Assisi::Core::AssetPath{};
        },
        [this](std::string_view vpath) -> Assisi::Core::AssetId
        { return _assetDatabase.IdFor(vpath).value_or(Assisi::Core::AssetId{}); });

    // S3: materialize glTF materials into `.amat` children + slot→material
    // manifests. The explosion writes new files, so rebuild afterwards to bring
    // them (and the manifests it stamped onto the glTF sidecars) into the DB —
    // only when something was actually exploded (steady state does nothing). The
    // hint resolver is already installed above, so written `.amat` channels carry
    // regenerated path hints (D2).
    if (ExplodeUnprocessedMeshes() > 0)
    {
        if (const std::expected<std::size_t, Assisi::Core::AssetError> rescan = _assetDatabase.Rebuild(); rescan)
            Assisi::Core::Log::Info("Asset reimport: {} assets indexed after material explosion.", *rescan);
    }

    // The browser lists by extension and never shows `.aast`, but a reimport may
    // have created sidecars, so force a re-read on next open.
    _assetBrowserDirty = true;
}

std::size_t SandboxApp::ExplodeUnprocessedMeshes()
{
    // Case-insensitive glTF-extension test on the virtual path.
    const auto isGltf = [](std::string_view path)
    {
        const auto endsWith = [path](std::string_view suffix)
        {
            if (path.size() < suffix.size())
                return false;
            const std::string_view tail = path.substr(path.size() - suffix.size());
            for (std::size_t i = 0; i < suffix.size(); ++i)
                if (std::tolower(static_cast<unsigned char>(tail[i])) != suffix[i])
                    return false;
            return true;
        };
        return endsWith(".gltf") || endsWith(".glb");
    };

    // Texture channels in the written `.amat`s resolve through the database, the
    // same map the asset cache uses — so an exploded material references the same
    // texture GUIDs the mesh would have imported.
    const auto resolveTextureId = [this](std::string_view vpath) -> Assisi::Core::AssetId
    { return _assetDatabase.IdFor(vpath).value_or(Assisi::Core::AssetId{}); };

    std::size_t exploded = 0;
    for (const auto &[id, path] : _assetDatabase.Assets())
    {
        // Reconcile-not-clobber: a glTF that already carries a manifest is done.
        if (!isGltf(path) || _assetDatabase.HasManifest(id))
            continue;

        const std::expected<std::size_t, Assisi::Geometry::MeshImportError> result =
            Assisi::Geometry::ExplodeGltfMaterials(path, resolveTextureId);
        if (result)
        {
            Assisi::Core::Log::Info("Asset reimport: exploded {} material(s) from '{}'.", *result, path);
            ++exploded;
        }
        else
        {
            Assisi::Core::Log::Warn("Asset reimport: could not explode '{}' ({}).", path,
                                    Assisi::Geometry::ToString(result.error()));
        }
    }
    return exploded;
}

void SandboxApp::SetupScene()
{
    auto *vulkanContext = Assisi::Render::RenderSystem::GetVulkanContext();
    if (!vulkanContext)
    {
        return;
    }

    nvrhi::IDevice *device = vulkanContext->GetDevice();

    const auto fbSize = GetWindow().GetFramebufferSize();

    // The engine's default scene-render path owns lighting + the mesh pipeline.
    // Built against GetSceneFramebufferInfo() rather than the swapchain's own
    // FramebufferInfo so it's already correct if options.json saved an MSAA mode.
    if (!_sceneRenderer.Initialize(device, GetSceneFramebufferInfo(), fbSize.Width, fbSize.Height, _camera))
    {
        RequestClose();
        return;
    }

    _assetCache.Initialize(device);
    // Thumbnails are drawn straight through ImGui, so they must not be sampled as
    // sRGB (which would gamma-decode them and show them too dark) — load linear.
    _thumbnailCache.Initialize(device, Assisi::Render::ColorSpace::Linear);
    if (std::expected<void, Assisi::Core::AssetError> loaded =
            // Displayed through ImGui, not the mesh shader — load linear so the
            // sampler doesn't gamma-decode it (same reason as the thumbnails).
            _helloTexture.LoadFromAssets(device, "textures/hello.png", Assisi::Render::ColorSpace::Linear);
        !loaded)
    {
        Assisi::Core::Log::Warn("Failed to load textures/hello.png for the ImGui image test.");
    }
}

void SandboxApp::OnResize(int width, int height)
{
    _sceneRenderer.Resize(width, height, _camera);
}

void SandboxApp::OnRenderTargetsChanged(const nvrhi::FramebufferInfo &framebufferInfo)
{
    if (!_sceneRenderer.OnRenderTargetsChanged(framebufferInfo))
    {
        Assisi::Core::Log::Error("Failed to rebuild the mesh pass pipeline after a render-target change.");
    }
}

void SandboxApp::OnRender(Assisi::Render::RenderFrame &frame)
{
    if (!_sceneRenderer.IsValid() || !_scene)
    {
        return;
    }

    // Blend physics-driven Transforms between their last two fixed-step poses
    // before the scene's world matrices are propagated in Render(), so bodies
    // move smoothly at the display's refresh rate rather than the physics rate.
    _physics.InterpolateTransforms(*_scene, GetInterpolationAlpha());

    // Refresh the editor camera's world matrix from its TRS before the view
    // matrix is derived from it; SceneRenderer propagates the game scene it draws.
    RefreshCameraMatrix();
    _sceneRenderer.Render(frame, *_scene, _cameraTransform, _camera);
}

void SandboxApp::OnFixedUpdate(float dt)
{
    if (!_scene)
        return;
    _physics.Update(dt);
    // Snapshot the new poses for render interpolation; OnRender blends them.
    _physics.CaptureState();
}

void SandboxApp::OnUpdate(float dt)
{
    auto &input = GetInput();
    if (input.IsKeyPressed(Assisi::Window::Key::Escape) && !ImGuiWantsKeyboard())
        RequestClose();

    if (!_scene)
        return;

    _systems.Run(Assisi::App::SystemPhase::Update,    {*_scene, dt, input, _actions, GetEvents()});
    _systems.Run(Assisi::App::SystemPhase::PostUpdate, {*_scene, dt, input, _actions, GetEvents()});
}

void SandboxApp::FlushDeferred()
{
    // End-of-frame: apply entities queued by Scene::Destroy() this frame. Runs
    // after RenderFrame, so a destroyed entity lives out its final frame before
    // its pools are touched — keeping structural changes out of any mid-frame Query.
    if (_scene)
        _scene->FlushDestroyed();
}

// ---------------------------------------------------------------------------
// ImGui panels
// ---------------------------------------------------------------------------

void SandboxApp::DrawDiagnosticsWindow()
{
    ImGui::Begin("Diagnostics");
    ImGui::Text("FPS: %d", GetFps());
    ImGui::Text("CPU: %.2f ms   GPU: %.2f ms", GetCpuFrameMs(), GetGpuFrameMs());
    ImGui::Separator();

    // Physics collision substeps per fixed step. 1 is a single solve (relies on
    // speculative contacts + per-body CCD, like Unity/Unreal); raising it trades
    // CPU for shallower impact penetration. Watch the CPU ms above as you change it.
    int collisionSteps = _physics.GetCollisionSteps();
    if (ImGui::InputInt("Physics collision steps", &collisionSteps))
        _physics.SetCollisionSteps(collisionSteps);

    ImGui::Separator();
    ImGui::TextDisabled("RMB: look  |  WASD: move  |  Space/Ctrl: up/down");
    ImGui::TextDisabled("Scroll: FOV  |  LMB: select  |  Esc: quit");
    ImGui::TextDisabled("F11: graphics settings");
    ImGui::End();
}

void SandboxApp::OnImGui()
{
    DrawOptionsWindow();
    DrawDiagnosticsWindow();
    DrawLevelsWindow();
    DrawInspector();
    DrawHelloImageWindow();
    DrawAssetBrowser();
}
