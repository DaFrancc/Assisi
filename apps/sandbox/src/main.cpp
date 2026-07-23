/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
/// @file main.cpp
/// @brief Assisi Sandbox entry point — a thin consumer of the editor library.
///
/// The editor itself lives in modules/Editor (Assisi::Editor::EditorApp); this
/// executable just parses arguments, builds an EditorConfig, and runs it. The
/// sandbox registers no game systems — it is pure level editing. (The Phase 2
/// template splits this into Game/GameEditor targets over a shared GameLib;
/// see docs/editor-extraction-plan.md.)

#include <Assisi/Editor/EditorApp.hpp>

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
    "  -h, --help              show this help and exit\n";

// Parses argv into a startup-level virtual path (empty if none). Returns false
// with a message printed when the arguments are malformed; sets shouldExit when
// --help was handled (a clean early exit, not an error).
bool ParseArgs(int argc, char **argv, std::string_view &startupLevel, bool &shouldExit)
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
    bool             shouldExit = false;
    if (!ParseArgs(argc, argv, startupLevel, shouldExit))
    {
        return EXIT_FAILURE;
    }
    if (shouldExit)
    {
        return EXIT_SUCCESS;
    }

    Assisi::Editor::EditorApp app({.registerGameSystems = nullptr, .startupLevel = std::string(startupLevel)});
    if (!app.Initialize())
    {
        return EXIT_FAILURE;
    }
    app.Run();
    return EXIT_SUCCESS;
}
