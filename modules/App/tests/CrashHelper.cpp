/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file CrashHelper.cpp
/// @brief A program that crashes on purpose, for TestCrashReport.cpp.
///
/// It sets up crash handling as Application does, under the user root, and
/// crashes as asked:
///
///   Assisi-CrashHelper fault    writes to an address nothing is mapped at
///   Assisi-CrashHelper abort    calls Core::Abort
///
/// The crash starts this program again as its reporter, headless, which is the
/// real reporter and not a stand-in for it.

#include <Assisi/App/CrashReport.hpp>
#include <Assisi/App/CrashReporter.hpp>
#include <Assisi/Core/Abort.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/Diagnostics.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string_view>

namespace
{

/// The function the frame tests look for by name, kept out of line so it is a
/// frame of its own.
[[gnu::noinline]] void CrashOnPurpose()
{
    // An address read back through volatile, so the compiler cannot see the
    // write is to nowhere and swap it for a trap of its own.
    volatile std::uintptr_t nowhere = 1;
    *reinterpret_cast<volatile std::int32_t *>(nowhere) = 1;
}

} // namespace

int main(int argc, char **argv)
{
    using namespace Assisi;

    if (const std::optional<App::CrashReportLaunch> report =
            App::CrashReportLaunchFrom({argv, static_cast<std::size_t>(argc)}))
    {
        return App::RunCrashReporter(*report);
    }
    if (argc != 2)
    {
        return EXIT_FAILURE;
    }

    const std::filesystem::path &userRoot = Core::AssetSystem::GetUserRoot();
    App::InstallCrashHandlers(userRoot / App::CrashFileStem(Core::LaunchStamp()), userRoot / App::kCrashDumpFolderName);
    App::SetCrashReporter(App::CrashReporterCommand(/*headless=*/ true));

    const std::string_view how = argv[1];
    if (how == "fault")
    {
        CrashOnPurpose();
    }
    else if (how == "abort")
    {
        Core::Abort("the crash helper was asked to abort");
    }
    return EXIT_FAILURE;
}
