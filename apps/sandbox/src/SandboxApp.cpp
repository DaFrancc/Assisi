/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include "SandboxApp.hpp"
#include "SandboxImGui.hpp"

#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Render/RenderSystem.hpp>
#include <Assisi/Render/Vulkan/VulkanContext.hpp>
#include <Assisi/Runtime/Camera.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/Hierarchy.hpp>
#include <Assisi/Window/Key.hpp>

#include <imgui.h>

#include <nlohmann/json.hpp>

#include <expected>
#include <fstream>

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
