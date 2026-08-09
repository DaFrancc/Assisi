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
                   uint32_t keep) noexcept
{
    namespace fs = std::filesystem;

    std::error_code ec;
    fs::directory_iterator it(dir, ec);
    if (ec)
    {
        return; // Missing directory is the normal first-launch case.
    }

    std::vector<fs::path> matches;
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
        if (name.starts_with(prefix) && name.ends_with(extension))
        {
            matches.push_back(it->path());
        }
    }

    if (matches.size() <= keep)
    {
        return;
    }

    // Newest first, by name — see the header for why not mtime.
    std::sort(matches.begin(), matches.end(),
              [](const fs::path &a, const fs::path &b) { return a.filename() > b.filename(); });

    for (size_t i = keep; i < matches.size(); ++i)
    {
        std::error_code removeEc;
        if (!fs::remove(matches[i], removeEc) || removeEc)
        {
            Log::Warn("PruneOldFiles: could not delete {}: {}", matches[i].string(),
                      removeEc ? removeEc.message() : "still present");
        }
    }
}

} // namespace Assisi::Core
