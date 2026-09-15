/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
/// @file ChiaraCapture.cpp
/// @brief Starting, stopping and writing performance captures.
///
/// The control half only — no UI. A capture is driven from code here, so a
/// headless run or an automated measurement can take one with nothing on screen;
/// the panel that drives it by hand is editor chrome and lives there.
///
/// The whole thing compiles to nothing without the capture system; the header's
/// declarations stay unconditional so call sites need no `#ifdef`.

#include <Assisi/App/Application.hpp>

#if defined(ASSISI_CHIARA_ENABLED)

#    include <Assisi/Chiara/Serializer.hpp>
#    include <Assisi/Core/AssetSystem.hpp>
#    include <Assisi/Core/Logger.hpp>

#    include <atomic>
#    include <chrono>
#    include <cstdio>
#    include <ctime>
#    include <filesystem>
#    include <memory>
#    include <mutex>
#    include <string>

namespace Assisi::App
{
namespace
{

/// State shared between the caller and the background serialize job. Held by
/// shared_ptr so a dump that outlives whatever asked for it still has somewhere
/// valid to write its result.
struct DumpState
{
    std::atomic<bool>       running{false};
    std::mutex resultMutex;
    Chiara::SerializeResult lastResult;
    std::string lastPath;
};

std::shared_ptr<DumpState> &SharedDumpState()
{
    static auto state = std::make_shared<DumpState>();
    return state;
}

/// @brief `captures/<prefix>-YYYYMMDD-HHMMSS.json` under the user root — the
/// same place options.json lives, because a capture is per-user writable state
/// and not asset content.
[[nodiscard]] std::filesystem::path NextCapturePath(const char *prefix = "chiara")
{
    const std::filesystem::path directory = Core::AssetSystem::GetUserRoot() / "captures";
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);

    const std::time_t now = std::time(nullptr);
    std::tm local{};
#    if defined(_WIN32)
    localtime_s(&local, &now);
#    else
    localtime_r(&now, &local);
#    endif

    char stamp[32] = {};
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
    return directory / (std::string(prefix) + "-" + stamp + ".json");
}

} // namespace

void Application::DumpChiaraCapture(double lastSeconds)
{
    const std::shared_ptr<DumpState> state = SharedDumpState();

    // One at a time: two concurrent dumps would each pause and resume recording
    // underneath the other, and the second resume would re-open capture while the
    // first was still reading rings.
    bool expected = false;
    if (!state->running.compare_exchange_strong(expected, true))
    {
        return;
    }

    const std::filesystem::path path = NextCapturePath();

    // On a worker: serializing holds the rings for as long as it takes to walk
    // them, and a frame should never wait on that.
    _jobs.Run(Core::Pool::Worker,
              [state, path, lastSeconds]
        {
            Chiara::SerializeResult result = Chiara::SerializeCapture(path, lastSeconds);
            {
                const std::lock_guard<std::mutex> lock(state->resultMutex);
                state->lastResult = std::move(result);
                state->lastPath   = path.string();
            }
            state->running.store(false, std::memory_order_release);
        });
}

void Application::StartChiaraSession()
{
    const std::filesystem::path path = NextCapturePath("chiara-session");
    if (!Chiara::BeginSession(path))
    {
        Core::Log::Warn("Chiara: could not start a session at {}", path.string());
    }
}

void Application::StopChiaraSession()
{
    // Read the path first: the stats are empty once the session is closed.
    const std::string path   = Chiara::GetSessionStats().path;
    const Chiara::SerializeResult result = Chiara::EndSession();
    if (!result.success)
    {
        return;
    }

    const std::shared_ptr<DumpState>  state = SharedDumpState();
    const std::lock_guard<std::mutex> lock(state->resultMutex);
    state->lastResult = result;
    state->lastPath   = path;
}

Application::ChiaraDumpReport Application::LastChiaraDump() const
{
    const std::shared_ptr<DumpState> state = SharedDumpState();

    ChiaraDumpReport report;
    report.running = state->running.load(std::memory_order_acquire);

    // Copied under the writer's own lock, so a caller sees one dump's result
    // whole rather than the path from this one and the byte count from the last.
    const std::lock_guard<std::mutex> lock(state->resultMutex);
    report.path          = state->lastPath;
    report.error         = state->lastResult.error;
    report.windowSeconds = state->lastResult.windowSeconds;
    report.bytesWritten  = state->lastResult.bytesWritten;
    report.eventsWritten = state->lastResult.eventsWritten;
    report.orphanedArgs  = state->lastResult.orphanedArgs;
    report.success       = state->lastResult.success;
    return report;
}

} // namespace Assisi::App

#else // !ASSISI_CHIARA_ENABLED

namespace Assisi::App
{

void Application::DumpChiaraCapture(double) {}
void Application::StartChiaraSession() {}
void Application::StopChiaraSession() {}
Application::ChiaraDumpReport Application::LastChiaraDump() const { return {}; }

} // namespace Assisi::App

#endif // ASSISI_CHIARA_ENABLED
