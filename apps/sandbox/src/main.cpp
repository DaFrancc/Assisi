/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
/// @file main.cpp
/// @brief Assisi Sandbox entry point — a thin consumer of the editor library.
///
/// The editor itself lives in modules/Editor (Assisi::Editor::EditorApp); this
/// executable just parses arguments, builds an EditorConfig, and runs it. Its
/// only "game" content is the demo systems below, which exist so the per-world
/// system binding has something observable to run. (The Phase 2 template splits
/// this into Game/GameEditor targets over a shared GameLib; see
/// docs/editor-extraction-plan.md.)

#include <Assisi/Editor/EditorApp.hpp>

#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Window/InputContext.hpp>

#include <glm/gtc/quaternion.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace
{
constexpr const char *kUsage =
    "Usage: Assisi-Sandbox [options]\n"
    "  -l, --load-level <lvl>  virtual path of a level to open at startup,\n"
    "                          e.g. levels/Materials.alvl\n"
    "  --no-editor-visuals     don't build the renderer's editor overlay passes\n"
    "                          (selection outline, entity icons, wireframes) —\n"
    "                          runs the render path a Game build gets\n"
    "  -h, --help              show this help and exit\n";

// Parses argv into the editor config inputs. Returns false with a message
// printed when the arguments are malformed; sets shouldExit when --help was
// handled (a clean early exit, not an error).
bool ParseArgs(int argc, char **argv, std::string_view &startupLevel, bool &editorVisuals, bool &shouldExit)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg = argv[i];
        if (arg == "-h" || arg == "--help")
        {
            std::fputs(kUsage, stdout);
            shouldExit = true;
            return true;
        }
        if (arg == "-l" || arg == "--load-level")
        {
            if (i + 1 >= argc)
            {
                std::fprintf(stderr, "%.*s requires a level path\n\n%s", static_cast<int>(arg.size()), arg.data(),
                             kUsage);
                return false;
            }
            startupLevel = argv[++i];
        }
        else if (arg == "--no-editor-visuals")
        {
            editorVisuals = false;
        }
        else
        {
            std::fprintf(stderr, "Unknown argument '%.*s'\n\n%s", static_cast<int>(arg.size()), arg.data(), kUsage);
            return false;
        }
    }
    return true;
}

// --- Demo game systems -----------------------------------------------------
//
// A minimum of real game logic, so that per-world system binding is observable
// in the editor rather than only in the headless tests: Play should visibly do
// something, Pause should visibly stop it, and a second resident world should
// spin on its own.
//
// Both are *stateless* — everything they touch lives in components — which is
// the shape a system installed into several worlds must have (see
// docs/world-system-binding-design-notes.md §1). SpinDemo's own accumulator
// deliberately lives in the World, not in a capture, for the same reason.

/// Spins every non-physics entity about Y. Physics-driven entities are excluded
/// (Without<RigidBodyDescriptor>) so this never fights Jolt for the same pose.
void SpinDemoSystem(Assisi::App::SystemContext &ctx)
{
    constexpr float kRadiansPerSecond = 1.0f;
    const glm::quat step =
        glm::angleAxis(kRadiansPerSecond * ctx.dt, glm::vec3(0.f, 1.f, 0.f));

    Assisi::ECS::Scene &scene = ctx.world.scene;
    for (auto [entity, transform] :
         scene.Query<Assisi::ECS::Transform>(Assisi::ECS::Without<Assisi::Physics::RigidBodyDescriptor>{}))
    {
        (void)transform;
        // Transform is ACOMP(tracked), and the query hands out an unstamped
        // reference: write through GetMut so PropagateTransforms actually sees
        // the change, or the world matrix keeps the old pose.
        if (Assisi::ECS::Transform *mutable_ = scene.GetMut<Assisi::ECS::Transform>(entity))
            mutable_->rotation = step * mutable_->rotation;
    }
}

/// Reports the space bar. Registered ActiveWorldOnly, so with two worlds
/// simulating only the active one reacts — the "one InputContext, N worlds"
/// rule made visible. Also the demo of SystemContext's nullable input.
void InputDemoSystem(Assisi::App::SystemContext &ctx)
{
    if (ctx.input == nullptr) // headless host: no devices to read
        return;
    if (ctx.input->IsKeyPressed(Assisi::Window::Key::Space))
    {
        Assisi::Core::Log::Info("InputDemo: space in world '{}' (active={}).", ctx.world.name,
                                ctx.isActiveWorld);
    }
}

void RegisterDemoSystems(Assisi::App::SystemRegistry &systems)
{
    systems.Register(Assisi::App::SystemPhase::Update, "SpinDemo", &SpinDemoSystem);
    systems.Register(Assisi::App::SystemPhase::Update, "InputDemo", &InputDemoSystem)
        .After("SpinDemo")
        .ActiveWorldOnly();
}
} // namespace

int main(int argc, char **argv)
{
    std::string_view startupLevel;
    bool             editorVisuals = true;
    bool             shouldExit    = false;
    if (!ParseArgs(argc, argv, startupLevel, editorVisuals, shouldExit))
    {
        return EXIT_FAILURE;
    }
    if (shouldExit)
    {
        return EXIT_SUCCESS;
    }

    Assisi::Editor::EditorApp app({.registerGameSystems = &RegisterDemoSystems,
                                   .startupLevel        = std::string(startupLevel),
                                   .enableEditorVisuals = editorVisuals});
    if (!app.Initialize())
    {
        return EXIT_FAILURE;
    }
    app.Run();
    return EXIT_SUCCESS;
}
