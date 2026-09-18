/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file PackMain.cpp
/// @brief The packer's entry point: a cooked tree in, one pak out, and a
///        non-zero exit for anything that did not pack.
///
/// Compression is a flag rather than a setting in a file, so each build target
/// passes its own: a debug pak favours pack time, a shipping one size.
///
/// --previous names the pak the last release shipped, from wherever it was kept
/// or downloaded. The new pak keeps each unchanged asset where that one had it,
/// so a storefront's delta update carries only what changed.

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <Assisi/Cook/CookTree.hpp>
#include <Assisi/Cook/PakWriter.hpp>
#include <Assisi/Core/Errors.hpp>
#include <Assisi/Core/PakCodec.hpp>
#include <Assisi/Core/PakProvider.hpp>

namespace
{

constexpr int kExitOk     = 0;
constexpr int kExitFailed = 1;
constexpr int kExitUsage  = 2;

void PrintUsage()
{
    std::fputs("usage: assisi-pack --cooked <cooked-root> --out <file.pak> [--compress none|lz4|zstd]\n"
               "                   [--previous <shipped.pak>]\n",
               stderr);
}

std::optional<Assisi::Core::PakCodec> ParseCodec(std::string_view value)
{
    for (const Assisi::Core::PakCodec codec :
         {Assisi::Core::PakCodec::None, Assisi::Core::PakCodec::Lz4, Assisi::Core::PakCodec::Zstd})
    {
        if (value == Assisi::Core::ToString(codec))
        {
            return codec;
        }
    }
    return std::nullopt;
}

} // namespace

int main(int argc, char **argv)
{
    std::filesystem::path cookedRoot;
    std::filesystem::path outPath;
    std::filesystem::path previousPath;
    Assisi::Core::PakCodec codec = Assisi::Core::PakCodec::None;

    for (int i = 1; i < argc; ++i)
    {
        const std::string_view argument{argv[i]};
        const bool hasValue = i + 1 < argc;

        if (argument == "--cooked" && hasValue)
        {
            cookedRoot = argv[++i];
        }
        else if (argument == "--out" && hasValue)
        {
            outPath = argv[++i];
        }
        else if (argument == "--previous" && hasValue)
        {
            previousPath = argv[++i];
        }
        else if (argument == "--compress" && hasValue)
        {
            const std::optional<Assisi::Core::PakCodec> parsed = ParseCodec(argv[++i]);
            if (!parsed)
            {
                PrintUsage();
                return kExitUsage;
            }
            codec = *parsed;
        }
        else
        {
            PrintUsage();
            return kExitUsage;
        }
    }

    if (cookedRoot.empty() || outPath.empty())
    {
        PrintUsage();
        return kExitUsage;
    }

    // The manifest, not the directory: see PakWriter.hpp for why.
    std::ifstream manifest(cookedRoot / Assisi::Cook::kManifestFileName, std::ios::binary);
    if (!manifest)
    {
        std::fprintf(stderr, "pack: %s: no manifest; cook first\n", cookedRoot.generic_string().c_str());
        return kExitFailed;
    }
    const std::string text{std::istreambuf_iterator<char>(manifest), std::istreambuf_iterator<char>()};
    const std::vector<Assisi::Cook::ManifestEntry> entries = Assisi::Cook::DeserializeManifest(text);

    // Mounted rather than parsed here, so a previous pak is held to the same checks
    // a game applies before trusting its index.
    std::optional<Assisi::Core::PakProvider> previous;
    if (!previousPath.empty())
    {
        std::expected<Assisi::Core::PakProvider, Assisi::Core::AssetError> mounted =
            Assisi::Core::PakProvider::Mount(previousPath);
        if (!mounted)
        {
            std::fprintf(stderr, "pack: %s: previous pak does not mount: %s\n", previousPath.generic_string().c_str(),
                         std::string{Assisi::Core::ToString(mounted.error())}.c_str());
            return kExitFailed;
        }
        previous.emplace(std::move(*mounted));
    }

    const std::expected<Assisi::Cook::PakReport, Assisi::Cook::CookError> report = Assisi::Cook::WritePak(
        cookedRoot, entries, outPath, codec,
        previous ? previous->Entries() : std::span<const Assisi::Core::PakEntry>{});
    if (!report)
    {
        std::fprintf(stderr, "pack: %s: %s\n", report.error().vpath.c_str(), report.error().reason.c_str());
        return kExitFailed;
    }

    std::fprintf(stdout,
                 "pack: %zu assets, %" PRIu64 " bytes stored (%" PRIu64 " uncompressed, %" PRIu64 " in gaps), %s\n",
                 report->slices, report->storedBytes, report->uncompressedBytes, report->gapBytes,
                 std::string{Assisi::Core::ToString(codec)}.c_str());
    std::fprintf(stdout, "pack: %s, %zu of %zu assets kept in place\n",
                 std::string{Assisi::Cook::ToString(report->layout)}.c_str(), report->keptSlices, report->slices);
    return kExitOk;
}
