/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
/// @file EditorChiaraPanel.cpp
/// @brief The capture control panel — recording toggle, ring coverage, dump
///        buttons.
///
/// Editor chrome. Capture itself is driven from App, so a headless run or an
/// automated measurement can take one with nothing on screen; this is only the
/// hand-driven face of it, and a game has no use for it.
///
/// The whole thing compiles to nothing without the capture system; the
/// declaration stays unconditional so the call site needs no `#ifdef`.

#include <Assisi/Editor/EditorChiaraPanel.hpp>

#if defined(ASSISI_CHIARA_ENABLED)

#    include <cstdio>
#    include <span>
#    include <string>

#    include <imgui.h>

#    include <Assisi/Chiara/Chiara.hpp>
#    include <Assisi/Render/RenderSystem.hpp>
#    include <Assisi/Render/Vulkan/VulkanContext.hpp>

namespace Assisi::Editor
{
namespace
{

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

void DrawChiaraPanel(App::Application &app)
{
    const App::Application::ChiaraDumpReport dump = app.LastChiaraDump();
    const Chiara::CaptureStats stats = Chiara::GetCaptureStats();

    ImGui::TextUnformatted("Chiara — performance capture");
    ImGui::Separator();

    // Disabled while a dump is in flight: the toggle and the serialize job flip
    // the same recording flag, and letting both drive it means a dump can finish
    // by switching capture back on after the user just switched it off.
    ImGui::BeginDisabled(dump.running);
    bool recording = Chiara::IsRecording();
    if (ImGui::Checkbox("Recording", &recording))
    {
        Chiara::SetRecording(recording);
    }
    ImGui::EndDisabled();

    // The device's half of the frame. Off by default and not tied to the
    // recording toggle: a timer splits every timed pass into its own render
    // pass, so a frame measured this way is not the frame that ships, and
    // leaving it on quietly changes the thing being measured.
    if (Render::Vulkan::VulkanContext *context = Render::RenderSystem::GetVulkanContext())
    {
        bool passTiming = context->IsPassTimingEnabled();
        if (ImGui::Checkbox("GPU per-pass timing", &passTiming))
        {
            context->SetPassTimingEnabled(passTiming);
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Times each render pass on the GPU and writes them into the\n"
                              "capture as gpu/<pass> counters.\n\n"
                              "Costs a render-pass break per timed pass, so total frame time\n"
                              "rises while this is on. Use it to find which pass moved, then\n"
                              "turn it off before quoting a frame time.");
        }

        // Two frames behind the one being recorded, because a query cannot be
        // read until its frame has finished on the device.
        const std::span<const Render::Vulkan::VulkanContext::PassTiming> passes = context->GetPassTimings();
        if (passTiming && passes.empty())
        {
            ImGui::TextDisabled("  waiting for the first timed frame…");
        }
        for (const Render::Vulkan::VulkanContext::PassTiming &pass : passes)
        {
            ImGui::Text("  %-22s %6.3f ms", pass.name, static_cast<double>(pass.milliseconds));
        }
    }

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
    ImGui::BeginDisabled(dump.running || session.active);
    if (ImGui::Button("Dump 5 s"))
    {
        app.DumpChiaraCapture(5.0);
    }
    ImGui::SameLine();
    if (ImGui::Button("Dump 20 s"))
    {
        app.DumpChiaraCapture(20.0);
    }
    ImGui::SameLine();
    if (ImGui::Button("Dump 60 s"))
    {
        app.DumpChiaraCapture(60.0);
    }
    ImGui::SameLine();
    if (ImGui::Button("Dump all"))
    {
        app.DumpChiaraCapture(0.0);
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
    ImGui::BeginDisabled(dump.running);
    if (!session.active)
    {
        if (ImGui::Button("Start session"))
        {
            app.StartChiaraSession();
        }
    }
    else
    {
        if (ImGui::Button("Stop session"))
        {
            app.StopChiaraSession();
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

    if (dump.running)
    {
        // Not a spinner widget: ImGui has none, and a rotating character is
        // enough to say "still going" without pretending to know progress.
        static constexpr char kSpin[] = {'|', '/', '-', '\\'};
        const std::size_t tick = static_cast<std::size_t>(ImGui::GetTime() * 8.0) % sizeof(kSpin);
        ImGui::Text("Writing %c", kSpin[tick]);
    }
    else if (!dump.path.empty())
    {
        if (dump.success)
        {
            ImGui::TextWrapped("Wrote %s (%s, %.1f s, %llu events)", dump.path.c_str(),
                               FormatBytes(dump.bytesWritten).c_str(), dump.windowSeconds,
                               static_cast<unsigned long long>(dump.eventsWritten));
            if (dump.orphanedArgs > 0)
            {
                ImGui::TextDisabled("%llu args had no enclosing scope and were dropped",
                                    static_cast<unsigned long long>(dump.orphanedArgs));
            }
        }
        else
        {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Dump failed: %s", dump.error.c_str());
        }
    }
    else
    {
        ImGui::TextDisabled("Drop a capture into ui.perfetto.dev to read it.");
    }
}

} // namespace Assisi::Editor

#else // !ASSISI_CHIARA_ENABLED

namespace Assisi::Editor
{
void DrawChiaraPanel(App::Application &) {}
} // namespace Assisi::Editor

#endif // ASSISI_CHIARA_ENABLED
