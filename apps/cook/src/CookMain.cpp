/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file CookMain.cpp
/// @brief The cooker's entry point: a source tree in, a cooked tree out, and a
///        non-zero exit for anything that did not cook.
///
/// The exit code is the contract. A cook failure has to *fail the build*, and a
/// build system reads `$?` — a run that logs an error and exits zero is
/// indistinguishable from one that had nothing to do.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <Assisi/Cook/CookTree.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Image/Compress.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>

namespace
{

constexpr int kExitOk     = 0;
constexpr int kExitFailed = 1;
constexpr int kExitUsage  = 2;

void PrintUsage()
{
    std::fputs("usage: assisi-cook --source <asset-root> --out <cooked-root> [--texture-tier fast|best]\n", stderr);
}

/// The texture tier a flag value names. Best is the default, because a cooked
/// tree is a packaging candidate unless whoever cooked it said otherwise.
std::optional<Assisi::Image::CompressQuality> ParseTextureTier(std::string_view value)
{
    if (value == "fast")
    {
        return Assisi::Image::CompressQuality::Fast;
    }
    if (value == "best")
    {
        return Assisi::Image::CompressQuality::Best;
    }
    return std::nullopt;
}

} // namespace

int main(int argc, char **argv)
{
    // A level cooks by being loaded, and the cook reads the source tree.
    (void)Assisi::Runtime::SceneSerializer::SetDocumentReader(&Assisi::Runtime::SceneSerializer::ReadTextDocument);

    std::filesystem::path sourceRoot;
    std::filesystem::path cookedRoot;
    Assisi::Image::CompressQuality textureQuality = Assisi::Image::CompressQuality::Best;

    for (int i = 1; i < argc; ++i)
    {
        const std::string_view argument{argv[i]};
        const bool hasValue = i + 1 < argc;

        if (argument == "--source" && hasValue)
        {
            sourceRoot = argv[++i];
        }
        else if (argument == "--out" && hasValue)
        {
            cookedRoot = argv[++i];
        }
        else if (argument == "--texture-tier" && hasValue)
        {
            const std::optional<Assisi::Image::CompressQuality> tier = ParseTextureTier(argv[++i]);
            if (!tier)
            {
                PrintUsage();
                return kExitUsage;
            }
            textureQuality = *tier;
        }
        else
        {
            PrintUsage();
            return kExitUsage;
        }
    }

    if (sourceRoot.empty() || cookedRoot.empty())
    {
        PrintUsage();
        return kExitUsage;
    }

    const std::expected<Assisi::Cook::CookReport, Assisi::Cook::CookError> report =
        Assisi::Cook::CookTree(sourceRoot, cookedRoot, textureQuality);
    if (!report)
    {
        // The path first, because that is what a person needs to open. Printed to
        // stderr rather than logged so it survives whatever the build system does
        // with the log sink.
        std::fprintf(stderr, "cook: %s: %s\n", report.error().vpath.c_str(), report.error().reason.c_str());
        return kExitFailed;
    }

    std::fprintf(stdout, "cook: %zu cooked, %zu unchanged, %zu source-only\n", report->cooked, report->skipped,
                 report->sourceOnly);
    return kExitOk;
}
