/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/App/ContentSet.hpp>

#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/ContentHash.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/PakProvider.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>

namespace Assisi::App
{

namespace
{

/// What SetContentPackage named. Set and read on the main thread only: when a
/// game mounts its package, and when a session starts.
const Core::PakProvider *gPackage = nullptr;

/// Whether a virtual path is content the set covers.
///
/// Both extensions, because they are one format and the extension never gates
/// behaviour — a `.alvl` instanced into another `.alvl` is legal, so a level is
/// as load-bearing across the wire as a blueprint is.
bool IsContentFile(std::string_view path)
{
    return path.ends_with(".alvl") || path.ends_with(".abp");
}

/// The package's levels and blueprints, sorted.
std::vector<std::string> PackagedContentPaths(const Core::PakProvider &package)
{
    std::vector<std::string> paths;
    for (const Core::PakEntry &entry : package.Entries())
    {
        if (IsContentFile(entry.path))
        {
            paths.push_back(entry.path);
        }
    }
    std::ranges::sort(paths);
    return paths;
}

/// The hash a file is folded in with: its pak row's when running from a package,
/// its normalized source text otherwise.
std::optional<std::uint64_t> ContentHashOf(const std::string &virtualPath)
{
    if (gPackage != nullptr)
    {
        const Core::PakEntry *entry = gPackage->Find(virtualPath);
        return entry != nullptr ? std::optional<std::uint64_t>{entry->contentHash} : std::nullopt;
    }
    return Core::HashTextFileNormalized(Core::AssetSystem::GetRoot() / virtualPath);
}

} // namespace

void SetContentPackage(const Core::PakProvider *package)
{
    gPackage = package;
}

const Core::PakProvider *ContentPackage()
{
    return gPackage;
}

std::vector<std::string> ScanContentPaths()
{
    if (gPackage != nullptr)
    {
        return PackagedContentPaths(*gPackage);
    }

    std::vector<std::string> paths;

    const std::filesystem::path root = Core::AssetSystem::GetRoot();

    std::error_code ec;
    if (root.empty() || !std::filesystem::is_directory(root, ec))
        return paths;

    for (std::filesystem::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec))
    {
        if (ec)
        {
            Core::Log::Warn("ContentSet: stopped scanning '{}': {}", root.string(), ec.message());
            break;
        }
        if (!it->is_regular_file(ec) || !IsContentFile(it->path().filename().string()))
            continue;

        // Virtual, with forward slashes, so two platforms produce the same string
        // for the same file and the sort below means the same thing on both.
        std::string virtualPath = std::filesystem::relative(it->path(), root, ec).generic_string();
        if (ec || virtualPath.empty())
            continue;
        paths.push_back(std::move(virtualPath));
    }

    // Filesystem enumeration order is not stable across machines; the sort is what
    // makes the combine in BuildContentSet a function of the *set* rather than of
    // the walk.
    std::sort(paths.begin(), paths.end());
    return paths;
}

ContentSet BuildContentSet()
{
    ContentSet set;
    set.paths = ScanContentPaths();

    if (set.paths.empty() && gPackage == nullptr)
    {
        const std::filesystem::path root = Core::AssetSystem::GetRoot();
        std::error_code ec;
        if (root.empty() || !std::filesystem::is_directory(root, ec))
            Core::Log::Warn("ContentSet: no asset root, so the content set is empty.");
    }

    std::uint64_t combined = Core::kFnvOffsetBasis;
    const auto fold     = [&combined](std::uint64_t value)
                          {
                              for (int32_t byte = 0; byte < 8; ++byte)
                              {
                                  combined ^= (value >> (byte * 8)) & 0xFFull;
                                  combined *= Core::kFnvPrime;
                              }
                          };

    for (const std::string &virtualPath : set.paths)
    {
        fold(Core::ContentHash64(
                 std::as_bytes(std::span{virtualPath.data(), virtualPath.size()})));

        // From source, content rather than bytes: a Windows checkout of the same
        // file differs by its line endings alone, and hashing that difference
        // refuses a join between two machines running identical content.
        const std::optional<std::uint64_t> fileHash = ContentHashOf(virtualPath);
        if (!fileHash)
        {
            // Folded in as zero rather than skipped: skipping would let two
            // machines with *different* unreadable files agree with each other.
            Core::Log::Warn("ContentSet: '{}' could not be read; the join will refuse rather than guess.",
                            virtualPath);
        }
        fold(fileHash.value_or(0));
    }

    set.hash = combined;
    return set;
}

} // namespace Assisi::App
