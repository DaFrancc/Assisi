/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
/// @file EditorMain.cpp
/// @brief The GameEditor entry point — a thin consumer of the editor library.
///
/// The editor itself lives in modules/Editor (Assisi::Editor::EditorApp); this
/// executable parses arguments, builds an EditorConfig, and runs it. It shares
/// GameLib — the game's systems, the headless ServerApp, the argument parsers —
/// with the Game target, and adds the editor on top.
///
/// Every authoring and development verb lives here rather than in the game: a
/// level path, the capture harness, the networking roles. The editor is a
/// library rather than its own executable because reflection registers per final
/// binary, so an editor that did not link the game's code could not inspect the
/// components that code declares.

#include "LaunchArgs.hpp"
#include "ServerApp.hpp"

#include <Assisi/Editor/EditorApp.hpp>

#include <Assisi/App/PerfCapture.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

namespace
{
constexpr const char *kUsage =
    "Usage: Assisi-GameEditor [options]\n"
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
    "  --gpu-cull              start with the GPU-driven cull path on (off by\n"
    "                          default; the CPU path is the reference)\n"
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
    "                          1920x1080. Defaults to the configured window size\n"
    "  --capture-out <path>    write the JSON report here as well as the log\n"
    "  --capture-image <path>  write a PNG of the frame after the last measured\n"
    "                          one, without the debug UI. Alone, it measures one\n"
    "                          frame after the warm-up\n"
    "  --capture-camera <ex,ey,ez,tx,ty,tz>\n"
    "                          stand the camera at e looking at t, instead of the\n"
    "                          level's active Camera\n"
    "  --capture-options <path> run with this options file instead of the\n"
    "                          user's options.json, which is left untouched\n"
    "  --capture-passes        also time each pass separately. OFF by default:\n"
    "                          per-pass timers force render-pass breaks, so a\n"
    "                          run with them on measures a frame that differs\n"
    "                          from the one that ships. Publish the default;\n"
    "                          use this to find which pass moved\n"
    "\n"
    "  e.g. Assisi-GameEditor -l levels/PerfBlank.alvl --capture 600 \\\n"
    "                         --capture-size 2560x1440 --capture-out blank-1440p.json\n";

/// The default frame count for --capture, which is the measurement protocol's
/// own figure rather than a number picked here.
constexpr int32_t kDefaultCaptureFrames = 600;

/// The port --host binds when the flag names none.
constexpr std::uint16_t kDefaultHostPort = 27015;

/// What argv resolved to, plus whether main should stop before starting.
///
/// Ordered widest member first, so the aggregate carries no interior padding.
struct EditorArgs
{
    Assisi::App::PerfCaptureConfig capture;
    Game::ServerOptions serverOptions;
    std::string startupLevel;
    bool editorVisuals = true;
    bool server        = false;
    bool pieClient     = false;
    bool gpuCulling    = false;
    bool shouldExit    = false;
};

/// Reads the value that follows @p flag, reporting a missing one by name.
bool TakeValue(int32_t argc, char **argv, int32_t &i, const char *flag, std::string_view &out)
{
    if (i + 1 >= argc)
    {
        std::fprintf(stderr, "%s requires a value\n\n%s", flag, kUsage);
        return false;
    }
    out = argv[++i];
    return true;
}

/// Reports a value the parser refused, naming the flag it belonged to.
void ReportBadValue(const char *flag, std::string_view value, const char *expected)
{
    std::fprintf(stderr, "%s expects %s, got '%.*s'\n\n%s", flag, expected, static_cast<int>(value.size()),
                 value.data(), kUsage);
}

/// The capture flags. Split from the rest so neither half is a wall: this one is
/// the measurement harness, and everything in it is inert in an ordinary run.
///
/// @return true when @p arg was a capture flag and was handled; @p ok carries
/// whether it parsed.
bool ParseCaptureArg(std::string_view arg, int32_t argc, char **argv, int32_t &i,
                     Assisi::App::PerfCaptureConfig &capture, bool &ok)
{
    ok = true;
    std::string_view value;

    if (arg == "--capture")
    {
        // The frame count is optional, like --host's port, so the common case is
        // just "--capture".
        capture.frames = kDefaultCaptureFrames;
        if (i + 1 < argc && argv[i + 1][0] != '-')
        {
            value = argv[++i];
            if (!Game::ParsePositive(value, capture.frames))
            {
                ReportBadValue("--capture", value, "a positive integer");
                ok = false;
            }
        }
        return true;
    }
    if (arg == "--capture-warmup")
    {
        ok = TakeValue(argc, argv, i, "--capture-warmup", value) &&
             Game::ParsePositive(value, capture.warmupFrames);
        if (!ok && !value.empty())
        {
            ReportBadValue("--capture-warmup", value, "a positive integer");
        }
        return true;
    }
    if (arg == "--capture-size")
    {
        ok = TakeValue(argc, argv, i, "--capture-size", value) &&
             Game::ParseResolution(value, capture.width, capture.height);
        if (!ok && !value.empty())
        {
            ReportBadValue("--capture-size", value, "<width>x<height>, e.g. 2560x1440");
        }
        return true;
    }
    if (arg == "--capture-camera")
    {
        ok = TakeValue(argc, argv, i, "--capture-camera", value) &&
             Game::ParseCameraPose(value, capture.cameraEye, capture.cameraTarget);
        if (!ok && !value.empty())
        {
            ReportBadValue("--capture-camera", value, "ex,ey,ez,tx,ty,tz");
        }
        capture.hasCameraPose = ok;
        return true;
    }
    if (arg == "--capture-passes")
    {
        capture.perPassTiming = true;
        return true;
    }
    if (arg == "--capture-out")
    {
        ok = TakeValue(argc, argv, i, "--capture-out", value);
        if (ok)
        {
            capture.outputPath = std::string(value);
        }
        return true;
    }
    if (arg == "--capture-image")
    {
        ok = TakeValue(argc, argv, i, "--capture-image", value);
        if (ok)
        {
            capture.imagePath = std::string(value);
        }
        return true;
    }
    if (arg == "--capture-options")
    {
        ok = TakeValue(argc, argv, i, "--capture-options", value);
        if (ok)
        {
            capture.optionsPath = std::string(value);
        }
        return true;
    }

    return false;
}

/// The networking and headless roles, for the same reason the captures are
/// separate: they are one subject, and a reader after the host port does not
/// want to walk past the capture harness to reach it.
bool ParseSessionArg(std::string_view arg, int32_t argc, char **argv, int32_t &i, EditorArgs &out, bool &ok)
{
    ok = true;
    std::string_view value;

    if (arg == "--server")
    {
        out.server = true;
        return true;
    }
    if (arg == "--pie-client")
    {
        out.pieClient = true;
        return true;
    }
    if (arg == "--host")
    {
        out.server             = true;
        out.serverOptions.role = Game::ServerRole::Host;
        // The port is optional, so only consume the next argument when it does
        // not look like another flag.
        if (i + 1 < argc && argv[i + 1][0] != '-')
        {
            value = argv[++i];
            std::string address;
            std::uint16_t port = kDefaultHostPort;
            if (!Game::ParseAddress(std::string(":").append(value), address, port))
            {
                ReportBadValue("--host", value, "a port in 1-65535");
                ok = false;
                return true;
            }
            out.serverOptions.port = port;
        }
        return true;
    }
    if (arg == "--connect")
    {
        out.serverOptions.role = Game::ServerRole::Client;
        ok = TakeValue(argc, argv, i, "--connect", value) &&
             Game::ParseAddress(value, out.serverOptions.address, out.serverOptions.port);
        if (!ok && !value.empty())
        {
            ReportBadValue("--connect", value, "addr, addr:port or :port");
        }
        return true;
    }
    if (arg == "--spawn")
    {
        int32_t count = 0;
        ok = TakeValue(argc, argv, i, "--spawn", value) && Game::ParsePositive(value, count);
        if (!ok && !value.empty())
        {
            ReportBadValue("--spawn", value, "a positive integer");
        }
        out.serverOptions.spawnCount = static_cast<std::uint32_t>(count);
        return true;
    }
    if (arg == "--ticks")
    {
        int32_t ticks = 0;
        ok = TakeValue(argc, argv, i, "--ticks", value) && Game::ParsePositive(value, ticks);
        if (!ok && !value.empty())
        {
            ReportBadValue("--ticks", value, "a positive integer");
        }
        out.serverOptions.tickLimit = static_cast<std::uint64_t>(ticks);
        return true;
    }

    return false;
}

/// Parses argv into @p out. Returns false with a message printed when the
/// arguments are malformed; sets shouldExit when --help was handled, which is a
/// clean early exit rather than an error.
bool ParseArgs(int32_t argc, char **argv, EditorArgs &out)
{
    for (int32_t i = 1; i < argc; ++i)
    {
        const std::string_view arg = argv[i];
        std::string_view value;
        bool ok = true;

        if (arg == "-h" || arg == "--help")
        {
            std::fputs(kUsage, stdout);
            out.shouldExit = true;
            return true;
        }

        if (ParseCaptureArg(arg, argc, argv, i, out.capture, ok) ||
            ParseSessionArg(arg, argc, argv, i, out, ok))
        {
            if (!ok)
            {
                return false;
            }
            continue;
        }

        if (arg == "-l" || arg == "--load-level")
        {
            if (!TakeValue(argc, argv, i, "--load-level", value))
            {
                return false;
            }
            out.startupLevel = std::string(value);
        }
        else if (arg == "--no-editor-visuals")
        {
            out.editorVisuals = false;
        }
        else if (arg == "--gpu-cull")
        {
            out.gpuCulling = true;
        }
        else if (arg == "--verbosity")
        {
            if (!TakeValue(argc, argv, i, "--verbosity", value))
            {
                return false;
            }
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
        }
        else
        {
            std::fprintf(stderr, "Unknown argument '%.*s'\n\n%s", static_cast<int>(arg.size()), arg.data(),
                         kUsage);
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    // The editor works on the source tree, so levels and blueprints are read as
    // the JSON an author saves. Installed before anything could load one.
    (void)Assisi::Runtime::SceneSerializer::SetDocumentReader(&Assisi::Runtime::SceneSerializer::ReadTextDocument);

    EditorArgs args;
    args.capture.frames = 0; // 0 means "not a capture run"; --capture sets it

    if (!ParseArgs(static_cast<int32_t>(argc), argv, args))
    {
        return EXIT_FAILURE;
    }
    if (args.shouldExit)
    {
        return EXIT_SUCCESS;
    }

    // A picture on its own needs a run to take it from; one measured frame is the
    // shortest there is.
    if (!args.capture.imagePath.empty() && args.capture.frames == 0)
    {
        args.capture.frames = 1;
    }

    // --connect on its own means the headless test client; --connect with
    // --pie-client means a windowed editor that joins. The flag rather than a
    // separate verb, because everything else about a PIE client is an ordinary
    // editor, and giving it its own entry point would make it a different
    // program that only resembles the one it is meant to exercise.
    std::string autoJoinEndpoint;
    if (args.pieClient)
    {
        if (args.serverOptions.role != Game::ServerRole::Client)
        {
            std::fprintf(stderr, "--pie-client requires --connect <addr[:port]>\n\n%s", kUsage);
            return EXIT_FAILURE;
        }
        autoJoinEndpoint = args.serverOptions.address + ":" + std::to_string(args.serverOptions.port);
    }
    else if (args.serverOptions.role == Game::ServerRole::Client)
    {
        args.server = true;
    }

    // The dedicated server is a different program, not the editor with its
    // window hidden: it brings up only the simulation half of Application and
    // never constructs an editor, a renderer, or a window.
    if (args.server)
    {
        args.serverOptions.level = args.startupLevel;
        Game::ServerApp serverApp(args.serverOptions);
        if (!serverApp.Initialize())
        {
            return EXIT_FAILURE;
        }
        serverApp.Run();
        // A server that refused to start must say so in its exit code, or a
        // supervisor reads the clean shutdown as a normal one.
        return serverApp.StartupFailed() ? EXIT_FAILURE : EXIT_SUCCESS;
    }

    args.capture.levelPath = args.startupLevel;
    Assisi::Editor::EditorApp app({.startupLevel        = args.startupLevel,
                                   .autoJoinEndpoint    = autoJoinEndpoint,
                                   .restrictedViewer    = args.pieClient,
                                   .enableEditorVisuals = args.editorVisuals,
                                   .perfCapture         = args.capture,
                                   .gpuCulling          = args.gpuCulling});
    if (!app.Initialize())
    {
        return EXIT_FAILURE;
    }
    app.Run();
    return EXIT_SUCCESS;
}
