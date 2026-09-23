/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file GameApp.hpp
/// @brief The application a player runs: boot, step the world, draw it, quit.
///
/// The counterpart of Editor::EditorApp over the same Application base, and
/// deliberately not that class with its panels switched off. A game has one
/// world on screen, no selection, no undo, no play state — it is simulating
/// from the moment it starts, which is the single largest difference and the
/// one that cannot be expressed as an editor with features disabled.
///
/// It boots the scene the shipped config names, and takes no level argument to
/// override it with: the one in AppConfig::startupScene is what a player gets.
/// A scene that is unnamed, unknown or unreadable refuses the launch by name —
/// there is no empty world to fall back to, because a game that opens a black
/// window has failed in a way nobody can act on.
///
/// It lives in App rather than beside a project's own sources because every
/// line of it is engine work — asset resolvers, the scene renderer, the fixed
/// step, the streaming pumps. What a game supplies is systems and content, and
/// neither is registered here: a system reaches the catalog by being declared
/// and linked, and a level names the ones it wants.
///
/// Derive from it to add game-specific frame work; call the base's hook from
/// any override, since each one does the engine half of its own job.

#include <Assisi/App/Application.hpp>
#include <Assisi/App/CameraBenchmark.hpp>
#include <Assisi/App/CookedAssets.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/PakProvider.hpp>
#include <Assisi/Render/AssetCache.hpp>
#include <Assisi/Runtime/Camera.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/SceneRenderer.hpp>
#include <Assisi/Window/ActionMap.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace Assisi::App
{

/// @brief The name of the content package a game reads, beside its executable.
inline constexpr const char *kDefaultPakName = "assets.pak";

/// @brief A benchmark run: the camera flies the level's route, a Chiara session
/// records it, and the game exits when the route ends.
///
/// Renders uncapped with 8x MSAA and FXAA and the Ultra shadow tier, with every
/// other graphics setting at its default, whatever the player's options say. So
/// runs on different machines and days measure the same settings. The options
/// are changed for the run only; a game never saves them.
struct GameBenchmark
{
    /// The level to fly. Empty flies the startup scene.
    std::string level;

    /// How long the route takes. The route is scaled to fit.
    double seconds = kDefaultBenchmarkSeconds;

    /// Time each render pass as well as the frame. Off by default: pass timers
    /// split render passes, so the frame they time is not the frame that ships.
    bool passTiming = false;
};

/// @brief What a game is launched with, as the command line resolved it.
struct GameLaunch
{
    /// Set for a benchmark run. Only a build with Chiara compiled in parses the
    /// flag, since a benchmark names a level and a shipped game takes none.
    std::optional<GameBenchmark> benchmark;

    /// The content package to read. Empty reads kDefaultPakName beside the
    /// executable, which is the only one a shipped build can name.
    std::filesystem::path pak;

    /// Stop after this many fixed ticks; 0 runs until the player quits.
    ///
    /// A test hook before it is anything else: a windowed run ends when someone
    /// closes the window, and nothing in a build machine is going to.
    std::uint64_t tickLimit = 0;
};

/// @brief The game application. See the file comment.
class GameApp : public Application
{
public:
    explicit GameApp(GameLaunch launch);
    ~GameApp() override;

protected:
    /// Opens the content package and installs the readers over it: the asset
    /// source, the level document reader and the config reader. A package that
    /// is missing or unreadable refuses the launch and names its path.
    [[nodiscard]] bool MountContent() override;

    void OnStart() override;
    void OnFixedUpdate(float dt) override;
    void OnUpdate(float dt) override;
    void OnRender(Render::RenderFrame &frame) override;
    void OnResize(int32_t width, int32_t height) override;
    void OnRenderTargetsChanged(const nvrhi::FramebufferInfo &framebufferInfo) override;
    void FlushDeferred() override;
    void InstallQueuedSystems() override;

    /// @brief The world the game plays in. Null only after a refused start, which
    /// closes the app before a frame runs.
    ///
    /// Exposed so a derived game can reach its scene without the manager. Travel
    /// replaces which world holds the role, so hold the reference for a frame,
    /// never across one.
    [[nodiscard]] World &MainWorld() { return *_world; }

    /// @brief Every resident world, for a game that keeps more than one.
    [[nodiscard]] WorldManager &Worlds() { return _worlds; }

private:
    /// Brings up the asset cache and the scene renderer. Windowed runs only —
    /// there is no device in a headless process and nothing to draw with it.
    ///
    /// False when there is nothing to draw with, which refuses the launch rather
    /// than running a game whose window stays empty.
    [[nodiscard]] bool SetupRenderer();

    /// The context the one-shot phases (Begin, Loaded) run under: everything a
    /// per-frame phase gets, with dt and the tick zero because no frame has run.
    [[nodiscard]] SystemContext WorldStartContext(World &world);

    /// Steps every world that is Active and simulating: its FixedUpdate systems,
    /// its physics, then its PostFixedUpdate systems. Worlds step sequentially,
    /// which is what lets them share one Jolt thread pool.
    void StepWorlds(float dt);

    /// Overrides the display options for a benchmark run. See GameBenchmark.
    void ApplyBenchmarkSettings(const GameBenchmark &benchmark);

    /// Moves the benchmark on by one frame, starting the Chiara session when
    /// the run starts and ending it, and the game, when the run ends.
    void AdvanceBenchmark();

    // Largest first, so the object carries no interior padding.

    /// GPU-side meshes, materials and textures, and the bindless table the mesh
    /// pipeline binds. Declared before the renderer, which holds its layout.
    Render::AssetCache _assetCache;

    /// The content package, open from MountContent until the process ends.
    std::optional<Core::PakProvider> _pak;

    /// What the renderer loads through, over the package above. Installed by
    /// MountContent, before the post-process shaders load.
    std::optional<CookedAssetSource> _assetSource;

    Runtime::SceneRenderer _sceneRenderer;

    WorldManager _worlds;

    /// Where the game looks from when the scene nominates no camera. A level
    /// that has not composed one renders from the origin rather than not at all,
    /// which is the difference between a scene that looks wrong and a window
    /// that looks broken.
    Runtime::Transform _fallbackPose;
    Runtime::Camera _fallbackCamera;

    GameLaunch _launch;

    /// Set for a benchmark run once its level has loaded.
    std::optional<CameraBenchmark> _benchmark;

    /// The world being played. Points into the manager, which keeps world
    /// addresses stable for their lifetime.
    World *_world = nullptr;
};

} // namespace Assisi::App
