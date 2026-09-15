/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/App/GameApp.hpp>

#include <Assisi/App/InputSetup.hpp>
#include <Assisi/App/LevelRuntime.hpp>
#include <Assisi/App/SceneCamera.hpp>
#include <Assisi/App/SystemCatalog.hpp>
#include <Assisi/Chiara/Profile.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Render/RenderSystem.hpp>
#include <Assisi/Render/Vulkan/VulkanContext.hpp>
#include <Assisi/Runtime/Camera.hpp>
#include <Assisi/Runtime/Hierarchy.hpp>
#include <Assisi/Window/Key.hpp>

#include <optional>
#include <vector>

namespace Assisi::App
{
namespace
{
// Per-frame ceiling on draining decoded assets to the GPU. A publish does real
// GPU work — createTexture, writeTexture, a blocking submit — so a whole burst
// landing in one frame hitches it. The game has no background preload to protect,
// so it takes the generous tier the editor reserves for a foreground load.
constexpr double kAssetPublishBudgetMs = 10.0;
constexpr std::uint64_t kAssetPublishBudgetBytes = 128ull << 20;
} // namespace

GameApp::GameApp(GameLaunch launch) : _launch(launch)
{
}

GameApp::~GameApp() = default;

void GameApp::OnStart()
{
    // The shipped bindings, then whatever the player rebound over the top.
    // Windowed only: a headless process has no devices, so an action map would
    // be answering questions nobody asks.
    if (HasPresentation())
    {
        LoadActionMap(_actions, GetOptions().bindings);
    }

    // The GUID→path index every mesh and material reference resolves through.
    // ReadOnly, never Reconcile: a shipped game indexes what it was given and
    // writes nothing beside it. Minting a sidecar into a player's install
    // directory is a write a game has no business making, and on a read-only
    // install it is one that fails.
    if (const auto indexed = _assetDatabase.Rebuild(Core::RebuildMode::ReadOnly))
    {
        Core::Log::Info("Game: {} assets indexed.", *indexed);
    }
    else
    {
        // Not fatal here: the world still loads, and every asset reference in it
        // simply fails to resolve. The level load is where that becomes an error
        // a player can be told about.
        Core::Log::Warn("Game: the asset root is unavailable; nothing will resolve.");
    }
    InstallAssetResolvers(_assetCache, _assetDatabase);

    // What a travel needs to turn a level file into a running world. Captured by
    // pointer; every one of these outlives the manager. The renderer is null in a
    // headless run, and WorldManager takes the render-free path when it is.
    _worlds.SetServices({.cache    = &_assetCache,
                         .database = &_assetDatabase,
                         .renderer = HasPresentation() ? &_sceneRenderer : nullptr,
                         .jobs     = &Jobs()});

    // One world, active and simulating from the first tick. The editor starts
    // its world stopped because an author is composing it; nobody is composing
    // this one, and a game that waited for a Play button would never start.
    _world = &_worlds.Create("Main");
    _worlds.SetActive(*_world);
    _world->state    = WorldState::Active;
    _world->simulate = true;

    // An empty list installs nothing, which is what a world built in memory
    // holds. A level loaded later applies whatever systems it names.
    (void)_worlds.ApplySystems(*_world, {}, "(startup)");

    if (HasPresentation())
    {
        SetupRenderer();
    }
}

void GameApp::SetupRenderer()
{
    Render::Vulkan::VulkanContext *vulkanContext = Render::RenderSystem::GetVulkanContext();
    if (vulkanContext == nullptr)
    {
        return;
    }
    nvrhi::IDevice *device = vulkanContext->GetDevice();
    const auto fbSize = GetWindow().GetFramebufferSize();

    // Before the scene renderer: the asset cache owns the bindless
    // material-texture table the mesh pipeline binds.
    _assetCache.Initialize(device, &Jobs());

    // enableEditorVisuals stays false and has no way to become true. The overlay
    // passes cost three pipelines and load assets/editor/**, and a game has
    // neither a selection to outline nor those files to load.
    if (!_sceneRenderer.Initialize({.device = device,
                                    .framebufferInfo = GetSceneFramebufferInfo(),
                                    .overlayFramebufferInfo = GetOverlayFramebufferInfo(),
                                    .width = fbSize.Width,
                                    .height = fbSize.Height,
                                    .camera = _fallbackCamera,
                                    .bindlessLayout = _assetCache.BindlessLayout(),
                                    .bindlessTable = _assetCache.BindlessTable(),
                                    .materialTable = _assetCache.MaterialTableBuffer(),
                                    .enableEditorVisuals = false}))
    {
        Core::Log::Error("Game: the scene render path failed to build; there is nothing to draw with.");
        RequestClose();
        return;
    }

    // The player's saved knobs. Nothing is allocated until content needs it — no
    // shadow map until a sun loads, no probe until a sky does.
    _sceneRenderer.SetShadowSettings(GetOptions().shadows);
    _sceneRenderer.SetEnvironmentSettings(GetOptions().environment);
    _sceneRenderer.SetSsaoSettings(GetOptions().ambientOcclusion);
}

void GameApp::StepWorlds(float dt)
{
    _worlds.ForEach(
        [this, dt](World &world)
        {
            if (world.state != WorldState::Active || !world.simulate)
            {
                return;
            }

            // Apply forces this tick, then simulate them, then react to what the
            // step actually did. The phase decides which side of the step a
            // system lands on; ordering within a phase cannot substitute for it.
            world.systems.Run(SystemPhase::FixedUpdate,
                              {world, dt, GetSimTick(), HasPresentation() ? &GetInput() : nullptr, &_actions,
                               GetEvents(), /*isActiveWorld=*/ &world == _worlds.Active(), &_worlds});

            {
                ASSISI_PROFILE_SCOPE("physics-step");
                world.physics.Update(dt);
            }
            {
                // Snapshot the new poses for OnRender to blend between. Linear in
                // the body count and separable from the solve, so a big scene can
                // say which of the two grew.
                ASSISI_PROFILE_SCOPE("physics-capture");
                world.physics.CaptureState();
            }

            world.systems.Run(SystemPhase::PostFixedUpdate,
                              {world, dt, GetSimTick(), HasPresentation() ? &GetInput() : nullptr, &_actions,
                               GetEvents(), /*isActiveWorld=*/ &world == _worlds.Active(), &_worlds});
        });
}

void GameApp::OnFixedUpdate(float dt)
{
    if (_world == nullptr)
    {
        return;
    }

    StepWorlds(dt);

    // A run with a tick budget ends when it is spent. Checked after the step, so
    // `--ticks 1` means one tick actually ran rather than none.
    if (_launch.tickLimit > 0 && GetSimTick() >= _launch.tickLimit)
    {
        RequestClose();
    }
}

void GameApp::OnUpdate(float dt)
{
    if (_world == nullptr)
    {
        return;
    }

    if (HasPresentation())
    {
        // Escape quits. In the editor the same key ends a play session and hands
        // the cursor back; here there is no session to return to, and a player
        // pressing it means the game.
        if (GetInput().IsKeyPressed(Window::Key::Escape))
        {
            RequestClose();
        }
    }

    // --- The frame's safe point ---------------------------------------------
    // Travel frees GPU assets that draws already recorded still reference, and the
    // system that asked for it runs inside the walk over the worlds. So game code
    // requests and this applies, outside the walk and before this frame's draws.
    if (_worlds.HasTravelRequest())
    {
        if (World *const arrived = _worlds.ProcessTravelRequest())
        {
            _world = arrived;
            _worlds.SweepAssetCache();
        }
    }

    // Advance a background load: once its worker has deserialized, this resolves
    // and streams the new world's assets while it stays hidden.
    _worlds.PumpPendingLoad();

    if (HasPresentation())
    {
        // Drain decoded-and-waiting uploads under a per-frame budget, batched into
        // one submit, before this frame's draws so they are visible to it.
        _assetCache.PumpPublishes(kAssetPublishBudgetMs, kAssetPublishBudgetBytes);

        // Swap billboard placeholders for the real mesh and material as each
        // finishes streaming. Every resident world, or one nobody is looking at
        // keeps its placeholders forever.
        _worlds.ForEach(
            [this](World &world)
            {
                if (world.state == WorldState::Loading)
                {
                    return;
                }
                UpgradeStreamingAssets(world.scene, _assetCache, _assetDatabase, world.streamingPending);
            });
    }

    // Worlds that simulate but are not drawn get neither the pose write-back nor
    // the transform propagation the render path performs for the world it draws.
    _worlds.ForEach(
        [this](World &world)
        {
            if (world.simulate && world.state == WorldState::Active && &world != _world)
            {
                SyncUnrenderedWorld(world);
            }
        });

    // Game logic, out of each world's own registry. Systems that consume input
    // opt out of the non-active worlds by declaring ActiveWorldOnly(): one
    // InputContext, N worlds.
    _worlds.ForEach(
        [this, dt](World &world)
        {
            if (world.state != WorldState::Active || !world.simulate)
            {
                return;
            }

            const SystemContext ctx{world,        dt,
                                    GetSimTick(), HasPresentation() ? &GetInput() : nullptr,
                                    &_actions,    GetEvents(),
                                    /*isActiveWorld=*/ &world == _worlds.Active(),
                                    &_worlds};
            world.systems.Run(SystemPhase::PreUpdate,  ctx);
            world.systems.Run(SystemPhase::Update,     ctx);
            world.systems.Run(SystemPhase::PostUpdate, ctx);
        });
}

void GameApp::OnRender(Render::RenderFrame &frame)
{
    if (!_sceneRenderer.IsValid() || _world == nullptr)
    {
        return;
    }

    {
        // Blend physics-driven Transforms between their last two fixed-step poses,
        // so bodies move at the display's refresh rate rather than the physics
        // rate. The resolver is required: a parented body's pose comes back in
        // world space while its Transform is an offset from its parent, so without
        // it every parented body drifts by its parent's transform once per frame.
        ASSISI_PROFILE_SCOPE("physics-interpolate");
        _world->physics.InterpolateTransforms(_world->scene, GetInterpolationAlpha(),
                                              ParentWorldResolver(_world->scene));
    }

    // Before the camera is chosen, not only inside Render(): a camera is placed
    // from its *world* matrix, and one parented to a character that just moved
    // would otherwise sit where the previous frame computed — permanently a frame
    // behind whatever it is attached to.
    _world->propagationTick = Runtime::PropagateTransforms(_world->scene, _world->propagationTick);

    const std::optional<SceneView> view = ActiveSceneCamera(_world->scene);
    const Runtime::Transform &pose   = view ? view->pose : _fallbackPose;
    const Runtime::Camera    &camera = view ? view->camera : _fallbackCamera;

    // The game's own render systems, through the world's registry. After
    // propagation and before the scene draw, so the matrices they are handed are
    // the ones the frame is actually drawn with.
    if (_world->systems.HasRenderSystems())
    {
        const float aspectRatio =
            frame.height > 0 ? static_cast<float>(frame.width) / static_cast<float>(frame.height) : 1.f;
        RenderContext renderCtx{_world->scene, GetInterpolationAlpha(), Runtime::ViewMatrix(pose),
                                Runtime::ProjectionMatrix(camera, aspectRatio)};
        _world->systems.RunRender(renderCtx);
    }

    _sceneRenderer.Render(frame, _world->scene, pose, camera, _world->propagationTick);
}

void GameApp::OnResize(int32_t width, int32_t height)
{
    _sceneRenderer.Resize(width, height, _fallbackCamera);
}

void GameApp::OnRenderTargetsChanged(const nvrhi::FramebufferInfo &framebufferInfo)
{
    if (!_sceneRenderer.OnRenderTargetsChanged(framebufferInfo, GetOverlayFramebufferInfo()))
    {
        Core::Log::Error("Game: failed to rebuild the mesh pass pipeline after a render-target change.");
    }
}

void GameApp::FlushDeferred()
{
    // Apply the entities Scene::Destroy() queued this frame. Runs after the frame
    // is recorded, so a destroyed entity lives out its final frame before its
    // pools are touched — which keeps structural changes out of any mid-frame
    // Query. A world still Loading is skipped: a worker owns its scene until
    // promotion.
    _worlds.ForEach(
        [](World &world)
        {
            if (world.state != WorldState::Loading)
            {
                world.scene.FlushDestroyed();
            }
        });
}

void GameApp::InstallQueuedSystems()
{
    // Every resident world: a blueprint can be spawned into a background one, and
    // a queue nobody drains leaves an entity holding its components and running
    // none of the code.
    _worlds.ForEach([](World &world) { DrainSystemInstalls(world); });
}

} // namespace Assisi::App
