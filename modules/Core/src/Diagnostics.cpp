/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <algorithm>
#include <exception>
#include <format>
#include <system_error>
#include <vector>

#include <Assisi/Core/Diagnostics.hpp>
#include <Assisi/Core/Logger.hpp>

namespace Assisi::Core
{

const std::chrono::time_zone *LocalZone()
{
    static const std::chrono::time_zone *zone = []() -> const std::chrono::time_zone *
    {
        try
        {
            return std::chrono::current_zone();
        }
        catch (const std::exception &)
        {
            return nullptr;
        }
    }();
    return zone;
}

const std::string &LaunchStamp()
{
    static const std::string stamp = []
    {
        using namespace std::chrono;
        const sys_time<seconds> nowUtc = floor<seconds>(system_clock::now());
        if (const time_zone *zone = LocalZone())
        {
            return std::format("{:%Y%m%d-%H%M%S}", zone->to_local(nowUtc));
        }
        return std::format("{:%Y%m%d-%H%M%S}", nowUtc);
    }();
    return stamp;
}

void PruneOldFiles(const std::filesystem::path &dir, std::string_view prefix, std::string_view extension,
                   uint32_t keep, std::string_view protect) noexcept
{
    namespace fs = std::filesystem;

    std::error_code ec;
    fs::directory_iterator it(dir, ec);
    if (ec)
    {
        return; // Missing directory is the normal first-launch case.
    }

    std::vector<fs::path> matches;
    bool                  protectedPresent = false;
    for (const fs::directory_iterator end; it != end; it.increment(ec))
    {
        if (ec)
        {
            Log::Warn("PruneOldFiles: stopped reading {}: {}", dir.string(), ec.message());
            return;
        }
        if (!it->is_regular_file(ec) || ec)
        {
            ec.clear();
            continue;
        }
        const std::string name = it->path().filename().string();
        if (!name.starts_with(prefix) || !name.ends_with(extension))
        {
            continue;
        }
        // Held out of the candidate list entirely — never a deletion target,
        // whatever the clock has done to the sort order.
        if (!protect.empty() && name == protect)
        {
            protectedPresent = true;
            continue;
        }
        matches.push_back(it->path());
    }

    // The protected file occupies one of the `keep` slots when it exists.
    const size_t retain = protectedPresent ? (keep > 0 ? keep - 1 : 0) : keep;
    if (matches.size() <= retain)
    {
        return;
    }

    // Newest first, by name — see the header for why not mtime.
    std::sort(matches.begin(), matches.end(),
              [](const fs::path &a, const fs::path &b) { return a.filename() > b.filename(); });

    for (size_t i = retain; i < matches.size(); ++i)
    {
        std::error_code removeEc;
        if (fs::remove(matches[i], removeEc))
        {
            continue;
        }
        // remove() returning false with no error means it was already gone —
        // another instance pruning the same directory, which is not a problem.
        if (removeEc)
        {
            Log::Warn("PruneOldFiles: could not delete {}: {}", matches[i].string(), removeEc.message());
        }
    }
}

} // namespace Assisi::Core
