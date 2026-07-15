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

#include <algorithm>
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

    // S3/S4: bring glTF materials up to date — explode new meshes, reconcile
    // already-exploded ones against their current source. Any write means new/
    // updated files + manifests, so rebuild afterwards to bring them into the DB
    // (only when something changed — steady state does nothing). The hint
    // resolver is already installed above, so written `.amat` channels carry
    // regenerated path hints (D2).
    if (ReconcileMeshMaterials())
    {
        if (const std::expected<std::size_t, Assisi::Core::AssetError> rescan = _assetDatabase.Rebuild(); rescan)
            Assisi::Core::Log::Info("Asset reimport: {} assets indexed after material reconcile.", *rescan);
    }

    // Liveness (D5): a stale mesh currently drawn in the open scene can't wait for
    // the author to click its badge — queue it for an immediate resolution prompt.
    // Non-live stale meshes just keep their badge (resolved on click / next load).
    // At startup the scene has no MeshRenderers yet, so this finds nothing then;
    // it fires on the manual Reimport button with a level loaded.
    _staleResolveQueue.clear();
    if (_scene != nullptr && !_staleMeshes.empty())
    {
        for (auto [entity, mrc] : _scene->Query<Assisi::Runtime::MeshRenderer>())
        {
            const std::optional<std::string> meshPath = _assetDatabase.PathFor(mrc.mesh);
            if (meshPath && _staleMeshes.contains(*meshPath) &&
                std::find(_staleResolveQueue.begin(), _staleResolveQueue.end(), *meshPath) ==
                    _staleResolveQueue.end())
            {
                _staleResolveQueue.push_back(*meshPath);
            }
        }
        if (_staleResolveTarget.empty()) // don't interrupt a prompt already open
        {
            AdvanceStaleQueue();
        }
    }

    // The browser lists by extension and never shows `.aast`, but a reimport may
    // have created sidecars, so force a re-read on next open.
    _assetBrowserDirty = true;
}

bool SandboxApp::ReconcileMeshMaterials()
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
    // Existing `.amat` files, by their GUID, so the reconciler can load and
    // compare them against the fresh material table.
    const auto resolveMaterialPath = [this](const Assisi::Core::AssetId &id) -> std::string
    { return _assetDatabase.PathFor(id).value_or(std::string{}); };

    _staleMeshes.clear();
    bool changed = false;
    for (const auto &[id, path] : _assetDatabase.Assets())
    {
        if (!isGltf(path))
            continue;

        if (!_assetDatabase.HasManifest(id))
        {
            // First sight: materialize the glTF's materials into `.amat` children.
            const std::expected<std::size_t, Assisi::Geometry::MeshImportError> result =
                Assisi::Geometry::ExplodeGltfMaterials(path, resolveTextureId);
            if (result)
            {
                Assisi::Core::Log::Info("Asset reimport: exploded {} material(s) from '{}'.", *result, path);
                changed = true;
            }
            else
            {
                Assisi::Core::Log::Warn("Asset reimport: could not explode '{}' ({}).", path,
                                        Assisi::Geometry::ToString(result.error()));
            }
            continue;
        }

        // Already exploded: reconcile against the current source (S4/D5).
        const Assisi::Geometry::ReconcileResult result =
            Assisi::Geometry::ReconcileGltfMaterials(path, resolveTextureId, resolveMaterialPath);
        switch (result.outcome)
        {
        case Assisi::Geometry::ReconcileOutcome::UpToDate:
            break;
        case Assisi::Geometry::ReconcileOutcome::Stamped:
            Assisi::Core::Log::Info("Asset reimport: recorded source hash for '{}'.", path);
            break;
        case Assisi::Geometry::ReconcileOutcome::GeometryOnly:
            Assisi::Core::Log::Info("Asset reimport: '{}' source changed but materials are unchanged; refreshed.",
                                    path);
            break;
        case Assisi::Geometry::ReconcileOutcome::AdditiveSlots:
            Assisi::Core::Log::Info("Asset reimport: '{}' gained {} material slot(s); wrote defaults.", path,
                                    result.addedSlots);
            break;
        case Assisi::Geometry::ReconcileOutcome::ConflictStale:
            _staleMeshes.insert(path);
            Assisi::Core::Log::Warn("Asset reimport: '{}' changed in a way that needs manual resolution; left "
                                    "stale (materials untouched).",
                                    path);
            break;
        case Assisi::Geometry::ReconcileOutcome::Failed:
            Assisi::Core::Log::Warn("Asset reimport: could not reconcile '{}'.", path);
            break;
        }
        if (result.changedDisk)
            changed = true;
    }
    return changed;
}

bool SandboxApp::IsAssetStale(std::string_view vpath) const
{
    return _staleMeshes.find(std::string{vpath}) != _staleMeshes.end();
}

// The two resolvers the reconcile/diff/regenerate calls need, backed by the
// database — a texture path→GUID and a material GUID→virtual-path.
namespace
{
Assisi::Core::AssetId ResolveTextureIdWith(const Assisi::Core::AssetDatabase &db, std::string_view vpath)
{
    return db.IdFor(vpath).value_or(Assisi::Core::AssetId{});
}
std::string ResolveMaterialPathWith(const Assisi::Core::AssetDatabase &db, const Assisi::Core::AssetId &id)
{
    return db.PathFor(id).value_or(std::string{});
}
} // namespace

void SandboxApp::OpenStaleResolution(const std::string &vpath)
{
    _staleResolveTarget = vpath;
    _staleResolveDiff   = Assisi::Geometry::DiffGltfMaterials(
        vpath, [this](std::string_view p) { return ResolveTextureIdWith(_assetDatabase, p); },
        [this](const Assisi::Core::AssetId &id) { return ResolveMaterialPathWith(_assetDatabase, id); });
    _staleResolveRequestOpen = true;
}

void SandboxApp::AdvanceStaleQueue()
{
    // Skip any queued mesh that is no longer stale (already resolved this session).
    while (!_staleResolveQueue.empty())
    {
        const std::string next = _staleResolveQueue.front();
        _staleResolveQueue.erase(_staleResolveQueue.begin());
        if (IsAssetStale(next))
        {
            OpenStaleResolution(next);
            return;
        }
    }
    _staleResolveTarget.clear(); // queue drained — modal stays closed
}

void SandboxApp::ApplyStaleResolution(bool regenerate)
{
    const std::string target = _staleResolveTarget;
    if (target.empty())
    {
        return;
    }

    bool applied = false;
    if (regenerate)
    {
        const std::optional<std::size_t> slots = Assisi::Geometry::RegenerateGltfMaterials(
            target, [this](std::string_view p) { return ResolveTextureIdWith(_assetDatabase, p); },
            [this](const Assisi::Core::AssetId &id) { return ResolveMaterialPathWith(_assetDatabase, id); });
        applied = slots.has_value();
        if (applied)
        {
            Assisi::Core::Log::Info("Resolved '{}': regenerated {} material slot(s) from source.", target, *slots);
        }
        else
        {
            Assisi::Core::Log::Warn("Resolve '{}': regeneration failed; left stale.", target);
        }
    }
    else
    {
        applied = Assisi::Geometry::AcceptGltfSource(target);
        if (applied)
        {
            Assisi::Core::Log::Info("Resolved '{}': kept existing materials, accepted new source.", target);
        }
        else
        {
            Assisi::Core::Log::Warn("Resolve '{}': could not accept source; left stale.", target);
        }
    }

    if (applied)
    {
        _staleMeshes.erase(target);

        // Regeneration wrote new `.amat`s and rewrote the manifest, so bring the
        // database back in sync and re-resolve any live entity that draws this
        // mesh. Accepting the source touches only the glTF's hash — nothing the
        // database or a resolved entity depends on — so neither is needed there.
        if (regenerate)
        {
            if (const std::expected<std::size_t, Assisi::Core::AssetError> rescan = _assetDatabase.Rebuild(); rescan)
            {
                Assisi::Core::Log::Info("Asset reimport: {} assets indexed after regenerate.", *rescan);
            }
            if (_scene != nullptr)
            {
                for (auto [entity, mrc] : _scene->Query<Assisi::Runtime::MeshRenderer>())
                {
                    ResolveMeshRendererAssets(mrc);
                }
            }
            _assetBrowserDirty = true;
        }
    }

    AdvanceStaleQueue(); // open the next live-stale mesh, or close the modal
}

void SandboxApp::DrawStaleResolutionModal()
{
    static constexpr const char *kPopupId = "Resolve material conflict";
    if (_staleResolveRequestOpen)
    {
        ImGui::OpenPopup(kPopupId);
        _staleResolveRequestOpen = false;
    }

    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(520.f, 0.f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(kPopupId, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        return;
    }

    ImGui::TextWrapped("The source of '%s' changed in a way that can't be auto-resolved. "
                       "Choose how to reconcile its materials:",
                       _staleResolveTarget.c_str());
    ImGui::Separator();

    if (_staleResolveDiff.valid && !_staleResolveDiff.slots.empty())
    {
        if (ImGui::BeginTable("stale_slots", 3,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed, 40.f);
            ImGui::TableSetupColumn("Material");
            ImGui::TableSetupColumn("Change", ImGuiTableColumnFlags_WidthFixed, 90.f);
            ImGui::TableHeadersRow();
            for (const Assisi::Geometry::SlotDiff &slot : _staleResolveDiff.slots)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%u", slot.slot);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(slot.name.empty() ? "(unnamed)" : slot.name.c_str());
                ImGui::TableNextColumn();
                const char *label = "unchanged";
                ImVec4      color(0.6f, 0.6f, 0.6f, 1.f);
                switch (slot.change)
                {
                case Assisi::Geometry::SlotChange::Unchanged:
                    break;
                case Assisi::Geometry::SlotChange::Changed:
                    label = "changed";
                    color = ImVec4(0.90f, 0.63f, 0.16f, 1.f); // amber — a conflict
                    break;
                case Assisi::Geometry::SlotChange::Added:
                    label = "added";
                    color = ImVec4(0.45f, 0.78f, 0.45f, 1.f); // green — safe
                    break;
                case Assisi::Geometry::SlotChange::Removed:
                    label = "removed";
                    color = ImVec4(0.86f, 0.36f, 0.36f, 1.f); // red — a conflict
                    break;
                }
                ImGui::TextColored(color, "%s", label);
            }
            ImGui::EndTable();
        }
    }
    else
    {
        ImGui::TextColored(ImVec4(0.86f, 0.36f, 0.36f, 1.f), "Could not read the current diff.");
    }

    ImGui::Separator();
    ImGui::TextWrapped("Regenerate from source overwrites the materials with the new source, discarding hand-edits "
                       "to changed slots (each material keeps its GUID). Keep my materials accepts the source and "
                       "stops flagging it, leaving every material untouched.");
    ImGui::Spacing();

    if (ImGui::Button("Regenerate from source"))
    {
        ApplyStaleResolution(/*regenerate=*/true);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Keep my materials"))
    {
        ApplyStaleResolution(/*regenerate=*/false);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Later"))
    {
        AdvanceStaleQueue(); // leave it stale/badged; move to the next live one
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
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
    // Only while simulating: paused/stopped, physics must not stomp the Transforms
    // (an inspector edit or the frozen pose is authoritative then).
    if (IsSimulating())
    {
        _physics.InterpolateTransforms(*_scene, GetInterpolationAlpha());
    }

    // Refresh the editor camera's world matrix from its TRS before the view
    // matrix is derived from it; SceneRenderer propagates the game scene it draws.
    RefreshCameraMatrix();
    // The selected entity gets an always-on-top orange selection outline.
    _sceneRenderer.SetHighlightedEntity(_selectedEntity);
    _sceneRenderer.Render(frame, *_scene, _cameraTransform, _camera);
}

void SandboxApp::OnFixedUpdate(float dt)
{
    // Simulation ticks only while the game is running (Run, not Pause/Stop). When
    // it isn't, physics is frozen so the scene only renders — the editor camera
    // and picking still run from OnUpdate, which is not gated here.
    if (!_scene || !IsSimulating())
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
    DrawGameControlWindow();
    DrawEntityListWindow();
    DrawLevelsWindow();
    DrawInspector();
    DrawHelloImageWindow();
    DrawAssetBrowser();
    DrawStaleResolutionModal();
}
