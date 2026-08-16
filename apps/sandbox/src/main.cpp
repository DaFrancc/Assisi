/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
/// @file main.cpp
/// @brief Assisi Sandbox entry point — a thin consumer of the editor library.
///
/// The editor itself lives in modules/Editor (Assisi::Editor::EditorApp); this
/// executable just parses arguments, builds an EditorConfig, and runs it. Its
/// only "game" content is DemoSystems.hpp, which exists so the per-world system
/// binding has something observable to run. (The Phase 2 template splits
/// this into Game/GameEditor targets over a shared GameLib; see
/// docs/editor-extraction-plan.md.)

#include "ServerApp.hpp"

#include <Assisi/Editor/EditorApp.hpp>

#include <Assisi/App/PerfCapture.hpp>

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
#include <optional>
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
    "  --verbosity <level>     lowest level to log: trace, debug, info, warn,\n"
    "                          error, fatal (default trace; info in a shipping\n"
    "                          build)\n"
    "  -h, --help              show this help and exit\n"
    "\n"
    " performance capture — run a scene, print medians, exit:\n"
    "  --capture [frames]      measure this many frames and exit (default 600;\n"
    "                          the protocol asks for at least 500). Snaps to the\n"
    "                          level's active Camera, turns pacing off and\n"
    "                          per-pass GPU timers on, and renders undecorated\n"
    "                          so the framebuffer is exactly the size asked for\n"
    "  --capture-warmup <n>    frames to discard first (default 120), covering\n"
    "                          pipeline compilation and first-use uploads\n"
    "  --capture-size <WxH>    resolution to render at, e.g. 2560x1440 or\n"
    "                          1920x1080. Defaults to game.json's window size\n"
    "  --capture-out <path>    write the JSON report here as well as the log\n"
    "  --capture-passes        also time each pass separately. OFF by default:\n"
    "                          per-pass timers force render-pass breaks, so a\n"
    "                          run with them on measures a frame that differs\n"
    "                          from the one that ships. Publish the default;\n"
    "                          use this to find which pass moved\n"
    "\n"
    "  e.g. Assisi-Sandbox -l levels/PerfBlank.alvl --capture 600 \\\n"
    "                      --capture-size 2560x1440 --capture-out blank-1440p.json\n";

// Parses a strictly positive integer argument, reporting the flag by name on
// failure. Zero is rejected along with negatives: a capture of no frames and a
// capture nobody asked for are different intentions, and the flag's presence
// already expressed one of them.
bool ParsePositive(std::string_view text, const char *flag, std::int32_t &out)
{
    std::int32_t value  = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value <= 0)
    {
        std::fprintf(stderr, "%s expects a positive integer, got '%.*s'\n\n%s", flag,
                     static_cast<int>(text.size()), text.data(), kUsage);
        return false;
    }
    out = value;
    return true;
}

// Parses "<width>x<height>". Both halves must be positive; anything else is a
// typo worth refusing rather than silently rendering at some other size and
// publishing the number under the resolution that was asked for.
bool ParseResolution(std::string_view text, std::int32_t &width, std::int32_t &height)
{
    const std::size_t separator = text.find('x');
    if (separator == std::string_view::npos)
    {
        return false;
    }

    const std::string_view left  = text.substr(0, separator);
    const std::string_view right = text.substr(separator + 1);

    std::int32_t parsedWidth  = 0;
    std::int32_t parsedHeight = 0;
    const auto first  = std::from_chars(left.data(), left.data() + left.size(), parsedWidth);
    const auto second = std::from_chars(right.data(), right.data() + right.size(), parsedHeight);
    if (first.ec != std::errc{} || first.ptr != left.data() + left.size() || second.ec != std::errc{} ||
        second.ptr != right.data() + right.size() || parsedWidth <= 0 || parsedHeight <= 0)
    {
        return false;
    }

    width  = parsedWidth;
    height = parsedHeight;
    return true;
}

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

    std::uint32_t parsedPort = 0;
    const auto parsed = std::from_chars(port.data(), port.data() + port.size(), parsedPort);
    if (parsed.ec != std::errc{} || parsed.ptr != port.data() + port.size() || parsedPort == 0 ||
        parsedPort > 65535u)
        return false;
    outPort = static_cast<std::uint16_t>(parsedPort);
    return true;
}

// Parses argv into the editor config inputs. Returns false with a message
// printed when the arguments are malformed; sets shouldExit when --help was
// handled (a clean early exit, not an error).
bool ParseArgs(int argc, char **argv, std::string_view &startupLevel, bool &editorVisuals, bool &server,
               Sandbox::ServerOptions &serverOptions, bool &pieClient, bool &shouldExit,
               Assisi::App::PerfCaptureConfig &capture)
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
        if (arg == "--verbosity")
        {
            if (i + 1 >= argc)
            {
                std::fprintf(stderr, "--verbosity requires a level name\n\n%s", kUsage);
                return false;
            }
            const std::string_view value = argv[++i];
            const std::optional<Assisi::Core::LogLevel> level = Assisi::Core::ParseLogLevel(value);
            if (!level)
            {
                std::fprintf(stderr, "--verbosity: '%.*s' is not a level name. Valid names are:",
                             static_cast<int>(value.size()), value.data());
                for (const std::string_view name : Assisi::Core::LogLevelNames())
                {
                    std::fprintf(stderr, " %.*s", static_cast<int>(name.size()), name.data());
                }
                std::fprintf(stderr, "\n\n%s", kUsage);
                return false;
            }
            // Applied here rather than stored: this runs before the Application
            // exists, so it takes effect for every line the engine emits,
            // including the ones from bring-up.
            Assisi::Core::GetLogger().SetMinLevel(*level);
            continue;
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
                std::uint32_t port  = 0;
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
            const auto parsed =
                std::from_chars(value.data(), value.data() + value.size(), serverOptions.tickLimit);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
            {
                std::fprintf(stderr, "--ticks expects a non-negative integer, got '%.*s'\n\n%s",
                             static_cast<int>(value.size()), value.data(), kUsage);
                return false;
            }
        }
        else if (arg == "--capture")
        {
            // Frame count is optional, like --host's port: the default is the
            // protocol's own figure, so the common case is just "--capture".
            capture.frames = 600;
            if (i + 1 < argc && argv[i + 1][0] != '-')
            {
                if (!ParsePositive(argv[++i], "--capture", capture.frames))
                {
                    return false;
                }
            }
        }
        else if (arg == "--capture-warmup")
        {
            if (i + 1 >= argc)
            {
                std::fprintf(stderr, "--capture-warmup requires a frame count\n\n%s", kUsage);
                return false;
            }
            if (!ParsePositive(argv[++i], "--capture-warmup", capture.warmupFrames))
            {
                return false;
            }
        }
        else if (arg == "--capture-size")
        {
            if (i + 1 >= argc)
            {
                std::fprintf(stderr, "--capture-size requires a resolution like 2560x1440\n\n%s", kUsage);
                return false;
            }
            if (!ParseResolution(argv[++i], capture.width, capture.height))
            {
                std::fprintf(stderr, "--capture-size expects <width>x<height>, e.g. 2560x1440\n\n%s", kUsage);
                return false;
            }
        }
        else if (arg == "--capture-passes")
        {
            capture.perPassTiming = true;
        }
        else if (arg == "--capture-out")
        {
            if (i + 1 >= argc)
            {
                std::fprintf(stderr, "--capture-out requires a path\n\n%s", kUsage);
                return false;
            }
            capture.outputPath = argv[++i];
        }
        else
        {
            std::fprintf(stderr, "Unknown argument '%.*s'\n\n%s", static_cast<int>(arg.size()), arg.data(), kUsage);
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    std::string_view startupLevel;
    bool editorVisuals = true;
    bool server        = false;
    bool pieClient     = false;
    bool shouldExit    = false;
    Sandbox::ServerOptions serverOptions;
    Assisi::App::PerfCaptureConfig capture;
    capture.frames = 0; // 0 means "not a capture run"; --capture sets it
    if (!ParseArgs(argc, argv, startupLevel, editorVisuals, server, serverOptions, pieClient, shouldExit, capture))
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
        // A server that refused to start must say so in its exit code, or a
        // supervisor reads the clean shutdown as a normal one.
        return serverApp.StartupFailed() ? EXIT_FAILURE : EXIT_SUCCESS;
    }

    capture.levelPath = std::string(startupLevel);
    Assisi::Editor::EditorApp app({.startupLevel        = std::string(startupLevel),
                                   .autoJoinEndpoint    = autoJoinEndpoint,
                                   .restrictedViewer    = pieClient,
                                   .enableEditorVisuals = editorVisuals,
                                   .perfCapture         = capture});
    if (!app.Initialize())
    {
        return EXIT_FAILURE;
    }
    app.Run();
    return EXIT_SUCCESS;
}
