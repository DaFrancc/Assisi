/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/App/ContentSet.hpp>

#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/ContentHash.hpp>
#include <Assisi/Core/Logger.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <system_error>

namespace Assisi::App
{

namespace
{

/// Whether a filename is content the set covers.
///
/// Both extensions, because they are one format and the extension never gates
/// behaviour — a `.alvl` instanced into another `.alvl` is legal, so a level is
/// as load-bearing across the wire as a blueprint is.
bool IsContentFile(const std::filesystem::path &path)
{
    const std::filesystem::path extension = path.extension();
    return extension == ".alvl" || extension == ".abp";
}

} // namespace

std::vector<std::string> ScanContentPaths()
{
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
        if (!it->is_regular_file(ec) || !IsContentFile(it->path()))
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

    const std::filesystem::path root = Core::AssetSystem::GetRoot();
    if (set.paths.empty())
    {
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

        // Content, not bytes. A Windows checkout of the same file differs by its
        // line endings alone, and hashing that difference refuses a join between
        // two machines running identical content.
        const std::optional<std::uint64_t> fileHash = Core::HashTextFileNormalized(root / virtualPath);
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

void ContentSetHashJob::Start(Core::JobSystem &jobs)
{
    if (_running || _task.IsValid())
        return;

    _running = true;
    _task    = jobs.Run(Core::Pool::Worker, []() -> ContentSet { return BuildContentSet(); });
}

bool ContentSetHashJob::Poll(ContentSet &out)
{
    if (!_running || !_task.IsValid() || !_task.IsComplete())
        return false;

    out      = _task.Get();
    _running = false;
    return true;
}

void ContentSetHashJob::Reset()
{
    _task    = {};
    _running = false;
}

} // namespace Assisi::App
