/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
/// @file ChiaraPanel.cpp
/// @brief The capture control panel (design: docs/chiara-design-notes.md §10).
///
/// Lives in App rather than Editor so a game gets it too — a shipped build made
/// with `-c` is exactly the one you want to capture from, and it has no editor.
/// The whole thing compiles to nothing without the capture system; the header's
/// declaration stays unconditional so call sites need no `#ifdef`.

#include <Assisi/App/Application.hpp>

#if defined(ASSISI_CHIARA_ENABLED)

#    include <Assisi/Chiara/Serializer.hpp>
#    include <Assisi/Core/AssetSystem.hpp>
#    include <Assisi/Core/Logger.hpp>

#    include <imgui.h>

#    include <atomic>
#    include <chrono>
#    include <cstdio>
#    include <ctime>
#    include <filesystem>
#    include <memory>
#    include <string>

namespace Assisi::App
{
namespace
{

/// State shared between the UI and the background serialize job. Held by
/// shared_ptr so a dump that outlives the panel (or the window) still has
/// somewhere valid to write its result.
struct DumpState
{
    std::atomic<bool>       running{false};
    std::mutex              resultMutex;
    Chiara::SerializeResult lastResult;
    std::string             lastPath;
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
    std::error_code             ec;
    std::filesystem::create_directories(directory, ec);

    const std::time_t now = std::time(nullptr);
    std::tm           local{};
#    if defined(_WIN32)
    localtime_s(&local, &now);
#    else
    localtime_r(&now, &local);
#    endif

    char stamp[32] = {};
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
    return directory / (std::string(prefix) + "-" + stamp + ".json");
}

[[nodiscard]] std::string FormatBytes(std::uint64_t bytes)
{
    char text[64] = {};
    if (bytes >= (1u << 20))
    {
        std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    }
    else
    {
        std::snprintf(text, sizeof(text), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    }
    return text;
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
    const std::string             path   = Chiara::GetSessionStats().path;
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

void Application::DrawChiaraPanel()
{
    const std::shared_ptr<DumpState> state   = SharedDumpState();
    const bool                       dumping = state->running.load(std::memory_order_acquire);

    const Chiara::CaptureStats stats = Chiara::GetCaptureStats();

    ImGui::TextUnformatted("Chiara — performance capture");
    ImGui::Separator();

    // Disabled while a dump is in flight: the toggle and the serialize job flip
    // the same recording flag, and letting both drive it means a dump can finish
    // by switching capture back on after the user just switched it off.
    ImGui::BeginDisabled(dumping);
    bool recording = Chiara::IsRecording();
    if (ImGui::Checkbox("Recording", &recording))
    {
        Chiara::SetRecording(recording);
    }
    ImGui::EndDisabled();

    ImGui::Text("Threads: %u   Events: %llu", stats.threadCount,
                static_cast<unsigned long long>(stats.totalEventsWritten));
    ImGui::Text("Main ring holds: %.1f s", stats.mainWindowSeconds);

    // Wrapping is not an error, but it is the difference between "the spike is in
    // this capture" and "the spike scrolled out of the buffer before you dumped".
    if (stats.bufferWrapCount > 0)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "Dropped to overwrite: %llu",
                           static_cast<unsigned long long>(stats.bufferWrapCount));
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("The rings are full and the oldest events are being discarded.\n"
                              "That is normal for a long session — it only matters if the\n"
                              "window above is shorter than the thing you are hunting.");
        }
    }

    ImGui::Separator();

    const Chiara::SessionStats session = Chiara::GetSessionStats();

    // Snapshot dumps: reach back into the ring for what already happened. Bounded
    // by the ring, which is the point — you press these *after* seeing a spike.
    ImGui::TextDisabled("Snapshot — the recent past, from memory");
    ImGui::BeginDisabled(dumping || session.active);
    if (ImGui::Button("Dump 5 s"))
    {
        DumpChiaraCapture(5.0);
    }
    ImGui::SameLine();
    if (ImGui::Button("Dump 20 s"))
    {
        DumpChiaraCapture(20.0);
    }
    ImGui::SameLine();
    if (ImGui::Button("Dump 60 s"))
    {
        DumpChiaraCapture(60.0);
    }
    ImGui::SameLine();
    if (ImGui::Button("Dump all"))
    {
        DumpChiaraCapture(0.0);
    }
    ImGui::EndDisabled();

    if (!session.active && stats.mainWindowSeconds > 0.0)
    {
        ImGui::TextDisabled("A dump longer than the %.0f s held above just gets everything.",
                            stats.mainWindowSeconds);
    }

    ImGui::Separator();

    // Session recording: streams to disk as it goes, so its length is bounded by
    // free space rather than by the ring. Use it when you know in advance what
    // you want to capture and it is longer than the buffer holds.
    ImGui::TextDisabled("Session — record forwards, straight to disk");
    ImGui::BeginDisabled(dumping);
    if (!session.active)
    {
        if (ImGui::Button("Start session"))
        {
            StartChiaraSession();
        }
    }
    else
    {
        if (ImGui::Button("Stop session"))
        {
            StopChiaraSession();
        }
    }
    ImGui::EndDisabled();

    if (session.active)
    {
        ImGui::Text("Recording %.1f s — %s, %llu events, %llu flushes", session.elapsedSeconds,
                    FormatBytes(session.bytesWritten).c_str(),
                    static_cast<unsigned long long>(session.eventsWritten),
                    static_cast<unsigned long long>(session.drains));

        // The number that decides whether the trace can be trusted. Draining
        // pauses capture, so it happens on a schedule; if a ring wrapped between
        // two of them the trace has a hole, and a hole nobody mentions is worse
        // than one that announces itself.
        if (session.eventsLost > 0)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Lost %llu events — drains are not keeping up",
                               static_cast<unsigned long long>(session.eventsLost));
        }
        ImGui::TextDisabled("%s", session.path.c_str());
        ImGui::TextDisabled("The file is readable even if the process dies mid-session.");
    }

    if (dumping)
    {
        // Not a spinner widget: ImGui has none, and a rotating character is
        // enough to say "still going" without pretending to know progress.
        static constexpr char kSpin[] = {'|', '/', '-', '\\'};
        const std::size_t     tick    = static_cast<std::size_t>(ImGui::GetTime() * 8.0) % sizeof(kSpin);
        ImGui::Text("Writing %c", kSpin[tick]);
    }
    else
    {
        const std::lock_guard<std::mutex> lock(state->resultMutex);
        if (!state->lastPath.empty())
        {
            if (state->lastResult.success)
            {
                ImGui::TextWrapped("Wrote %s (%s, %.1f s, %llu events)", state->lastPath.c_str(),
                                   FormatBytes(state->lastResult.bytesWritten).c_str(),
                                   state->lastResult.windowSeconds,
                                   static_cast<unsigned long long>(state->lastResult.eventsWritten));
                if (state->lastResult.orphanedArgs > 0)
                {
                    ImGui::TextDisabled("%llu args had no enclosing scope and were dropped",
                                        static_cast<unsigned long long>(state->lastResult.orphanedArgs));
                }
            }
            else
            {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Dump failed: %s",
                                   state->lastResult.error.c_str());
            }
        }
        else
        {
            ImGui::TextDisabled("Drop a capture into ui.perfetto.dev to read it.");
        }
    }
}

} // namespace Assisi::App

#else // !ASSISI_CHIARA_ENABLED

namespace Assisi::App
{

// Inline-equivalent stubs so a default build links without the caller knowing.
void Application::DrawChiaraPanel() {}
void Application::DumpChiaraCapture(double) {}
void Application::StartChiaraSession() {}
void Application::StopChiaraSession() {}

} // namespace Assisi::App

#endif // ASSISI_CHIARA_ENABLED
