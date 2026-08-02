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

#include "ServerApp.hpp"

#include <Assisi/Editor/EditorApp.hpp>

#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/App/PhysicsSystems.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Window/InputContext.hpp>

#include <glm/gtc/quaternion.hpp>

#include <charconv>
#include <cstdint>

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
    "  --server                run headless: no window, renderer, input or debug\n"
    "                          UI — just the fixed-step simulation (see ServerApp)\n"
    "  --host [port]           --server + replicate to clients (default port 27015)\n"
    "  --connect <addr[:port]> join a host and mirror its world. Headless by\n"
    "                          default; with --pie-client it is a windowed editor\n"
    "  --pie-client            play-in-editor client: a windowed editor that joins\n"
    "                          --connect at startup and writes nothing the editor\n"
    "                          that spawned it also owns. Launched by \"Host + N\"\n"
    "  --spawn <n>             --host only: spawn n moving replicated entities\n"
    "  --ticks <n>             --server only: stop after n fixed ticks (0 = run\n"
    "                          until interrupted, the default)\n"
    "  -h, --help              show this help and exit\n";

// Parses argv into the editor config inputs. Returns false with a message
// printed when the arguments are malformed; sets shouldExit when --help was
// handled (a clean early exit, not an error).
// Parses "addr", "addr:port", or ":port" into its parts, leaving whichever it
// does not find untouched. IPv6 literals are not handled here — --connect takes
// the plain form, and anything more elaborate belongs in a server browser, not
// in argv parsing.
bool ParseAddress(std::string_view text, std::string &outAddress, std::uint16_t &outPort)
{
    const std::size_t colon = text.rfind(':');
    if (colon == std::string_view::npos)
    {
        outAddress = std::string(text);
        return !outAddress.empty();
    }

    const std::string_view host = text.substr(0, colon);
    const std::string_view port = text.substr(colon + 1);
    if (!host.empty())
        outAddress = std::string(host);

    std::uint32_t  parsedPort = 0;
    const auto parsed = std::from_chars(port.data(), port.data() + port.size(), parsedPort);
    if (parsed.ec != std::errc{} || parsed.ptr != port.data() + port.size() || parsedPort == 0 ||
        parsedPort > 65535u)
        return false;
    outPort = static_cast<std::uint16_t>(parsedPort);
    return true;
}

bool ParseArgs(int argc, char **argv, std::string_view &startupLevel, bool &editorVisuals, bool &server,
               Sandbox::ServerOptions &serverOptions, bool &pieClient, bool &shouldExit)
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
        else if (arg == "--server")
        {
            server = true;
        }
        else if (arg == "--host")
        {
            server             = true;
            serverOptions.role = Sandbox::ServerRole::Host;
            // The port is optional, so only consume the next argument when it
            // does not look like another flag.
            if (i + 1 < argc && argv[i + 1][0] != '-')
            {
                const std::string_view value = argv[++i];
                std::uint32_t          port  = 0;
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), port);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || port == 0 ||
                    port > 65535u)
                {
                    std::fprintf(stderr, "--host expects a port in 1-65535, got '%.*s'\n\n%s",
                                 static_cast<int>(value.size()), value.data(), kUsage);
                    return false;
                }
                serverOptions.port = static_cast<std::uint16_t>(port);
            }
        }
        else if (arg == "--pie-client")
        {
            pieClient = true;
        }
        else if (arg == "--connect")
        {
            if (i + 1 >= argc)
            {
                std::fprintf(stderr, "--connect requires an address\n\n%s", kUsage);
                return false;
            }
            serverOptions.role = Sandbox::ServerRole::Client;
            if (!ParseAddress(argv[++i], serverOptions.address, serverOptions.port))
            {
                std::fprintf(stderr, "--connect could not parse the address\n\n%s", kUsage);
                return false;
            }
        }
        else if (arg == "--spawn")
        {
            if (i + 1 >= argc)
            {
                std::fprintf(stderr, "--spawn requires a count\n\n%s", kUsage);
                return false;
            }
            const std::string_view value = argv[++i];
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), serverOptions.spawnCount);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
            {
                std::fprintf(stderr, "--spawn expects a non-negative integer, got '%.*s'\n\n%s",
                             static_cast<int>(value.size()), value.data(), kUsage);
                return false;
            }
        }
        else if (arg == "--ticks")
        {
            if (i + 1 >= argc)
            {
                std::fprintf(stderr, "%.*s requires a tick count\n\n%s", static_cast<int>(arg.size()), arg.data(),
                             kUsage);
                return false;
            }
            const std::string_view value = argv[++i];
            const auto             parsed =
                std::from_chars(value.data(), value.data() + value.size(), serverOptions.tickLimit);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
            {
                std::fprintf(stderr, "--ticks expects a non-negative integer, got '%.*s'\n\n%s",
                             static_cast<int>(value.size()), value.data(), kUsage);
                return false;
            }
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
    systems.Register(Assisi::App::SystemPhase::Update, "SpinDemo", &SpinDemoSystem)
        // Nothing to spin without Transforms — so in a world that has none (an
        // empty one, or a streamed-out region later), this costs one array load
        // per frame instead of a call and a query.
        .RequireAny<Assisi::ECS::Transform>();
    systems.Register(Assisi::App::SystemPhase::Update, "InputDemo", &InputDemoSystem)
        .After("SpinDemo")
        .ActiveWorldOnly();
}

/// A second profile, so that "which systems run" is visibly a per-level choice
/// rather than a global one. `assets/levels/Test.alvl` selects it by name, and a
/// world built from it keeps the input probe but does no spinning — load it
/// alongside a default-profile level and only one of the two animates.
///
/// It is also where the bounce is switched on, and that is the more interesting
/// half: a profile installer receives the whole World, not just its registry, so
/// it can set up the *engine* state its systems need as well as the systems
/// themselves. App::BounceSystem does nothing without contact reporting, and
/// contact reporting is off by default, so the two are enabled together in one
/// place rather than left for a level author to remember separately.
void RegisterDemoProfiles(Assisi::App::WorldManager &worlds)
{
    worlds.RegisterProfile(
        "Static",
        [](Assisi::App::World &world)
        {
            world.physics.SetContactReporting(true);

            world.systems
                .Register(Assisi::App::SystemPhase::FixedUpdate, "Bounce", &Assisi::App::BounceSystem)
                // A world with no Bounce components never calls it — and this is
                // the level's own gate, so a second resident level without
                // bouncers pays nothing for this one having them.
                .RequireAny<Assisi::Physics::Bounce>();

            world.systems.Register(Assisi::App::SystemPhase::Update, "InputDemo", &InputDemoSystem)
                .ActiveWorldOnly();
        });
}
} // namespace

int main(int argc, char **argv)
{
    std::string_view      startupLevel;
    bool                  editorVisuals = true;
    bool                  server        = false;
    bool                  pieClient     = false;
    bool                  shouldExit    = false;
    Sandbox::ServerOptions serverOptions;
    if (!ParseArgs(argc, argv, startupLevel, editorVisuals, server, serverOptions, pieClient, shouldExit))
    {
        return EXIT_FAILURE;
    }
    if (shouldExit)
    {
        return EXIT_SUCCESS;
    }

    // --connect on its own means the headless test client; --connect with
    // --pie-client means a windowed editor that joins. The flag rather than a
    // separate verb, because everything else about a PIE client is an ordinary
    // editor, and giving it its own entry point would make it a different
    // program that only resembles the one it is meant to exercise.
    std::string autoJoinEndpoint;
    if (pieClient)
    {
        if (serverOptions.role != Sandbox::ServerRole::Client)
        {
            std::fprintf(stderr, "--pie-client requires --connect <addr[:port]>\n\n%s", kUsage);
            return EXIT_FAILURE;
        }
        autoJoinEndpoint = serverOptions.address + ":" + std::to_string(serverOptions.port);
    }
    else if (serverOptions.role == Sandbox::ServerRole::Client)
    {
        server = true;
    }

    // The dedicated server is a different program, not the editor with its
    // window hidden: it brings up only the simulation half of Application and
    // never constructs an editor, a renderer, or a window.
    if (server)
    {
        serverOptions.level = std::string(startupLevel);
        Sandbox::ServerApp serverApp(serverOptions);
        if (!serverApp.Initialize())
        {
            return EXIT_FAILURE;
        }
        serverApp.Run();
        return EXIT_SUCCESS;
    }

    Assisi::Editor::EditorApp app({.registerGameSystems = &RegisterDemoSystems,
                                   .registerProfiles    = &RegisterDemoProfiles,
                                   .startupLevel        = std::string(startupLevel),
                                   .autoJoinEndpoint    = autoJoinEndpoint,
                                   .restrictedViewer    = pieClient,
                                   .enableEditorVisuals = editorVisuals});
    if (!app.Initialize())
    {
        return EXIT_FAILURE;
    }
    app.Run();
    return EXIT_SUCCESS;
}
