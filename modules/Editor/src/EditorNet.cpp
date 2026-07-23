/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file EditorNet.cpp
/// @brief The editor's network panel: Host / Join / Disconnect, and the live
/// numbers you need to tell "it works" from "it looks like it works".
///
/// Hosting from here *is* the listen server — the scene the editor is already
/// simulating and rendering is the one being replicated, with no second scene
/// and no self-interpolation for the host player (see NetSession.hpp for why
/// the design's loopback-pair framing is not what this does).

#include <Assisi/Editor/EditorApp.hpp>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/NetSync/NetComponents.hpp>

#include <imgui.h>

#include <cstdint>

namespace Assisi::Editor
{
namespace
{

/// Colour a rate green/amber/red against thresholds a human can act on. Ping in
/// milliseconds: under 60 is fine, under 150 is playable, above that is not.
ImVec4 PingColor(std::int32_t pingMs)
{
    if (pingMs < 0)
        return ImVec4{0.6f, 0.6f, 0.6f, 1.f};
    if (pingMs < 60)
        return ImVec4{0.4f, 0.9f, 0.4f, 1.f};
    if (pingMs < 150)
        return ImVec4{0.9f, 0.8f, 0.3f, 1.f};
    return ImVec4{0.9f, 0.4f, 0.4f, 1.f};
}

void LabelledValue(const char *label, const std::string &value)
{
    ImGui::TextUnformatted(label);
    ImGui::SameLine(180.f);
    ImGui::TextUnformatted(value.c_str());
}

} // namespace

void EditorApp::DrawNetworkWindow()
{
    if (!ImGui::Begin("Network"))
    {
        ImGui::End();
        return;
    }

    const bool active = _netSession && _netSession->IsActive();

    // ---- status line -------------------------------------------------------
    if (_netSession)
    {
        const std::string status = _netSession->StatusText();
        ImGui::TextUnformatted(status.c_str());
    }
    else
    {
        ImGui::TextUnformatted("Offline");
    }
    ImGui::Separator();

    // ---- controls ----------------------------------------------------------
    ImGui::SetNextItemWidth(140.f);
    ImGui::InputText("Address", _netAddress.data(), _netAddress.size(),
                     active ? ImGuiInputTextFlags_ReadOnly : ImGuiInputTextFlags_None);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.f);
    ImGui::InputInt("Port", &_netPort, 0, 0, active ? ImGuiInputTextFlags_ReadOnly : ImGuiInputTextFlags_None);
    _netPort = std::clamp(_netPort, 1, 65535);

    ImGui::BeginDisabled(active || !_scene);
    if (ImGui::Button("Host"))
    {
        // Bound to the scene that exists right now. A level load replaces the
        // scene wholesale, which is why the session is torn down there rather
        // than left holding a dangling reference.
        _netSession = std::make_unique<Assisi::NetSync::NetSession>(*_scene);
        if (!_netSession->Host(static_cast<std::uint16_t>(_netPort)))
            _netSession.reset();
    }
    ImGui::SameLine();
    if (ImGui::Button("Join"))
    {
        _netSession = std::make_unique<Assisi::NetSync::NetSession>(*_scene);
        if (!_netSession->Join(_netAddress.data(), static_cast<std::uint16_t>(_netPort)))
            _netSession.reset();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!active);
    if (ImGui::Button("Disconnect"))
        ShutdownNetSession();
    ImGui::EndDisabled();

    if (_netSession && !_netSession->LastError().empty() && !active)
    {
        ImGui::TextColored(ImVec4{0.9f, 0.4f, 0.4f, 1.f}, "%s", _netSession->LastError().c_str());
    }

    if (!active)
    {
        ImGui::Separator();
        ImGui::TextDisabled("Hosting replicates this scene; entities need a Replicated component to travel.");
        ImGui::End();
        return;
    }

    // ---- live stats --------------------------------------------------------
    const Assisi::NetSync::SessionStats stats = _netSession->Stats();

    ImGui::Separator();
    ImGui::TextUnformatted("Ping");
    ImGui::SameLine(180.f);
    ImGui::TextColored(PingColor(stats.pingMs), "%d ms", stats.pingMs);

    LabelledValue("Bandwidth in", std::format("{:.1f} kB/s", stats.inBytesPerSec / 1024.f));
    LabelledValue("Bandwidth out", std::format("{:.1f} kB/s", stats.outBytesPerSec / 1024.f));

    if (_netSession->IsHost())
    {
        LabelledValue("Clients", std::format("{}", stats.clientCount));
        LabelledValue("Replicated entities", std::format("{}", stats.replicatedEntities));
        LabelledValue("Snapshots sent", std::format("{}", stats.snapshotsSent));
        LabelledValue("Bytes sent", std::format("{}", stats.bytesSent));
        if (stats.snapshotsSent > 0)
            LabelledValue("Avg snapshot", std::format("{} B", stats.bytesSent / stats.snapshotsSent));
    }
    else
    {
        LabelledValue("Server tick", std::format("{}", stats.serverTick));
        LabelledValue("Mirrored entities", std::format("{}", stats.replicatedEntities));
        LabelledValue("Snapshots applied", std::format("{}", stats.snapshotsApplied));

        // A nonzero rejection count is never normal: it means either corruption
        // the transport did not catch or a protocol bug. Make it loud.
        ImGui::TextUnformatted("Snapshots rejected");
        ImGui::SameLine(180.f);
        ImGui::TextColored(stats.snapshotsRejected > 0 ? ImVec4{0.9f, 0.4f, 0.4f, 1.f}
                                                       : ImVec4{0.6f, 0.6f, 0.6f, 1.f},
                           "%llu", static_cast<unsigned long long>(stats.snapshotsRejected));

        // Buffer depth is the honest health signal for the clock: zero means the
        // server ran out of our input, which the player feels as dropped input.
        ImGui::TextUnformatted("Input buffer");
        ImGui::SameLine(180.f);
        ImGui::TextColored(stats.inputBufferDepth == 0 ? ImVec4{0.9f, 0.8f, 0.3f, 1.f}
                                                       : ImVec4{0.4f, 0.9f, 0.4f, 1.f},
                           "%u command%s", stats.inputBufferDepth, stats.inputBufferDepth == 1 ? "" : "s");

        LabelledValue("Clock lead", std::format("{} ticks", stats.clockLead));
        LabelledValue("Clock corrections", std::format("{}", stats.clockCorrections));

        if (!stats.worldComplete)
            ImGui::TextColored(ImVec4{0.9f, 0.8f, 0.3f, 1.f}, "Still receiving the initial world...");
    }

    ImGui::End();
}

void EditorApp::ShutdownNetSession()
{
    if (!_netSession)
        return;

    // Disconnect() drops a client's mirrored entities; flush so they are gone
    // before anything iterates the scene again.
    _netSession->Disconnect();
    if (_scene)
        _scene->FlushDestroyed();
    _netSession.reset();
    Core::Log::Info("Editor: network session closed.");
}

void EditorApp::PollNetSession()
{
    if (_netSession)
        _netSession->Poll();
}

void EditorApp::TickNetSession()
{
    if (_netSession)
        _netSession->Tick(GetSimTick());
}

void EditorApp::InterpolateNetSession()
{
    if (_netSession)
        _netSession->Interpolate();
}

} // namespace Assisi::Editor
