/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file EditorNet.cpp
/// @brief The editor's half of a networked play session: the join sequence, the
/// host-time guards, and the panel that shows what is going on.
///
/// **A network session exists only inside a play session.** Hosting starts by
/// entering Play; a client joins by entering Play with a join target; Stop —
/// either side, any reason — tears the session down. Hosting from here is a
/// listen server: the scene the editor is already simulating and rendering is
/// the one being replicated, with no second scene and no self-interpolation for
/// the host player (NetSession.hpp says why that is not a loopback pair).
///
/// That rule is what keeps this file small. A joined client is in Play, so its
/// world is the *play* scene, which the editor already treats as disposable:
/// Save is gated on Editing, the play snapshot preserves the editing scene
/// exactly, and Stop restores it with its undo history intact. What is left here
/// is only what is genuinely new — negotiating which level to load, proving both
/// sides have the same bytes, and stripping the entities the host owns.

#include <Assisi/Editor/EditorApp.hpp>

#include <Assisi/App/BlueprintReplication.hpp>
#include <Assisi/App/ChildProcess.hpp>
#include <Assisi/App/ContentSet.hpp>
#include <Assisi/App/LevelRuntime.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/ContentHash.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/NetSync/NetComponents.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Runtime/AssetResolve.hpp>
#include <Assisi/Runtime/Hierarchy.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>

#include <imgui.h>

#include <cstdint>
#include <filesystem>
#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <vector>

namespace Assisi::Editor
{
namespace
{

/// Green/amber/red for a ping in milliseconds: under 60 is fine, under 150 is
/// playable, above that is not. A negative ping is "unknown" and greys out.
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

constexpr ImVec4 kWarnColor{0.9f, 0.8f, 0.3f, 1.f};
constexpr ImVec4 kErrorColor{0.9f, 0.4f, 0.4f, 1.f};

void LabelledValue(const char *label, const std::string &value)
{
    ImGui::TextUnformatted(label);
    ImGui::SameLine(180.f);
    ImGui::TextUnformatted(value.c_str());
}

/// A counter whose *normal* value is zero: grey at zero, amber above it.
///
/// Each of these names a different problem, so only a row that is actually
/// nonzero should draw the eye. The tooltip carries the diagnosis — a count
/// nobody can interpret is a count nobody acts on.
template <typename T>
void NetCounterRow(const char *label, T value, const char *tooltip)
{
    ImGui::TextUnformatted(label);
    ImGui::SameLine(180.f);
    ImGui::TextColored(value > 0 ? kWarnColor : ImVec4{0.6f, 0.6f, 0.6f, 1.f}, "%llu",
                       static_cast<unsigned long long>(value));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tooltip);
}

/// "levels/Materials.alvl" -> "Materials". The Levels UI works in bare stems.
std::string LevelStem(const std::string &virtualPath)
{
    return std::filesystem::path(virtualPath).stem().string();
}

} // namespace

// ---------------------------------------------------------------------------
// Session lifecycle
// ---------------------------------------------------------------------------

bool EditorApp::IsNetSessionActive() const { return _netSession && _netSession->IsActive(); }

Assisi::NetSync::LevelIdentity EditorApp::HostLevelIdentity() const
{
    Assisi::NetSync::LevelIdentity identity;
    if (_world == nullptr || _world->levelPath.empty())
        return identity; // never saved: addressing stays None, and hosting refuses

    const auto resolved = Assisi::Core::AssetSystem::Resolve(_world->levelPath);
    if (!resolved)
        return identity;

    const std::optional<std::uint64_t> hash = Assisi::Core::HashTextFileNormalized(*resolved);
    if (!hash)
        return identity;

    identity.addressing  = Assisi::NetSync::LevelAddressing::Virtual;
    identity.path        = _world->levelPath;
    identity.contentHash = *hash;
    return identity;
}

void EditorApp::FailJoin(std::string reason)
{
    _netError = reason;
    Assisi::Core::Log::Error("Editor: join failed — {}", reason);
    if (_netSession)
        _netSession->AbortJoin(std::move(reason));
    _joinPhase = JoinPhase::None;
    // Out through the same door as everything else: leaving a bad join is Stop,
    // not a second mechanism.
    _pendingStopPlay = true;
}

void EditorApp::StripReplicatedEntities()
{
    if (_scene == nullptr || _physics == nullptr)
        return;

    // Shared with the headless client, which used to carry its own copy of this
    // and had drifted: it ended the entities without taking their bodies out of
    // the physics world, and never dropped the orphaned parent links.
    const Assisi::App::StrippedEntities stripped = Assisi::App::StripReplicatedEntities(*_scene, *_physics);

    if (stripped.entities != 0 || stripped.orphans != 0)
    {
        Assisi::Core::Log::Info("Editor: join stripped {} replicated entit{} from the level ({} orphan link{}).",
                                stripped.entities, stripped.entities == 1 ? "y" : "ies", stripped.orphans,
                                stripped.orphans == 1 ? "" : "s");
    }
}

void EditorApp::BuildJoinedWorld()
{
    if (!_netSession || !_netSession->IsAwaitingLevel() || _scene == nullptr)
        return;

    const Assisi::NetSync::ServerHello  *hello = _netSession->Handshake();
    const Assisi::NetSync::LevelIdentity level = hello->level;

    // --- Which file, and is it the same file? -------------------------------
    std::filesystem::path file;
    switch (level.addressing)
    {
    case Assisi::NetSync::LevelAddressing::None:
        FailJoin("the host is not running a level file, so there is no world to build here.");
        return;
    case Assisi::NetSync::LevelAddressing::Virtual:
    {
        const auto resolved = Assisi::Core::AssetSystem::Resolve(level.path);
        if (!resolved)
        {
            FailJoin("this build has no '" + level.path + "'; get the level from the host and retry.");
            return;
        }
        file = *resolved;
        break;
    }
    case Assisi::NetSync::LevelAddressing::AbsolutePath:
        file = level.path;
        break;
    }

    const std::optional<std::uint64_t> localHash = Assisi::Core::HashTextFileNormalized(file);
    if (!localHash)
    {
        FailJoin("cannot read '" + file.string() + "'.");
        return;
    }
    if (*localHash != level.contentHash)
    {
        // The two numbers only answer "are they different", which the failure
        // already announced — so they go to the log and the message stays
        // something the player can act on.
        Assisi::Core::Log::Error("Editor: level content hash mismatch for '{}' — host {}, local {}.", level.path,
                                 Assisi::Core::ToHex64(level.contentHash), Assisi::Core::ToHex64(*localHash));
        FailJoin("your copy of '" + level.path + "' differs from the host's; sync the file from the host and retry.");
        return;
    }

    // --- Build it -----------------------------------------------------------
    // Straight into the play scene: the pre-play snapshot already holds the
    // editing scene, so there is nothing here to preserve and nothing to confirm.
    Assisi::Runtime::LevelHeader header;
    const auto                   reset = _worlds.Count() > 1 ? Assisi::App::AssetCacheReset::Keep
                                                             : Assisi::App::AssetCacheReset::ClearFirst;
    const bool loaded = level.addressing == Assisi::NetSync::LevelAddressing::AbsolutePath
                            ? Assisi::App::LoadLevelFile(*_scene, file, _assetCache, _assetDatabase, *_physics,
                                                         _sceneRenderer, reset, &header, &_world->instances)
                            : Assisi::App::LoadLevel(*_scene, level.path, _assetCache, _assetDatabase, *_physics,
                                                     _sceneRenderer, reset, &header, &_world->instances);
    if (!loaded)
    {
        FailJoin("'" + level.path + "' failed to load.");
        return;
    }

    // The world is the host's level for the duration; StopPlay puts the edited
    // world's path, systems and instance table back.
    _world->levelPath = level.addressing == Assisi::NetSync::LevelAddressing::Virtual ? level.path : std::string{};
    // Loud rather than fatal: unwinding a half-built join is not something this
    // function can do. A client missing the host's systems mirrors state and runs
    // none of the behaviour, which is worth shouting about even when it cannot be
    // refused outright.
    if (!_worlds.ApplySystems(*_world, header.systems, level.path))
    {
        Assisi::Core::Log::Error("Join: the host's level '{}' names a system this build does not declare. "
                                 "Mirrored state will be correct and its behaviour will not run.",
                                 level.path);
    }

    StripReplicatedEntities();

    // The load rebuilt entity identity from scratch, so every handle kept from the
    // pre-join scene now aliases a live but unrelated entity. Clear them all.
    _selectedEntity     = Assisi::ECS::NullEntity;
    _eyedropperArmed    = false;
    _eyedropperEntity   = Assisi::ECS::NullEntity;
    _eyedropperMeta     = nullptr;
    _assetBrowserOpen   = false;
    _assetBrowserEntity = Assisi::ECS::NullEntity;
    _assetBrowserMeta   = nullptr;

    // Only now, because from here a NetId has somewhere to land.
    _netSession->ConfirmLevelReady();
    _joinPhase = JoinPhase::Live;
    _netError.clear();
    Assisi::Core::Log::Info("Editor: joined — built '{}' and answered the handshake.", level.path);
}

// ---------------------------------------------------------------------------
// Play in editor
// ---------------------------------------------------------------------------

bool EditorApp::WritePieTempLevel(Assisi::NetSync::LevelIdentity &outLevel)
{
    if (_scene == nullptr)
        return false;

    // Under the user root rather than the asset root: a transient of one play
    // session, not content, so it must not appear in the asset browser or pick up
    // a GUID sidecar. Clients address it absolutely, so where it lives is nobody
    // else's business.
    const auto resolved = Assisi::Core::AssetSystem::ResolveUser("pie-host-level.alvl");
    if (!resolved)
    {
        Assisi::Core::Log::Error("PIE: cannot resolve a temp level path under the user root.");
        return false;
    }

    const Assisi::Runtime::LevelHeader header{
        .instances = {}, .systems = _world != nullptr ? _world->systemNames : std::vector<std::string>{}};
    if (!Assisi::Runtime::SceneSerializer::SaveToFile(*_scene, *resolved, header,
                                                      _world != nullptr ? &_world->instances : nullptr))
    {
        Assisi::Core::Log::Error("PIE: could not write the temp level '{}'.", resolved->string());
        return false;
    }

    const std::optional<std::uint64_t> hash = Assisi::Core::HashTextFileNormalized(*resolved);
    if (!hash)
    {
        Assisi::Core::Log::Error("PIE: could not read back the temp level '{}'.", resolved->string());
        return false;
    }

    _pieTempLevel        = *resolved;
    outLevel.addressing  = Assisi::NetSync::LevelAddressing::AbsolutePath;
    outLevel.path        = resolved->string();
    outLevel.contentHash = *hash;
    return true;
}

void EditorApp::SpawnPieClients(std::int32_t count)
{
    if (count <= 0)
        return;

    const std::optional<std::filesystem::path> exe = Assisi::Core::AssetSystem::ExecutablePath();
    if (!exe)
    {
        Assisi::Core::Log::Error("PIE: cannot find this executable's path; no clients launched.");
        return;
    }

    const std::string endpoint = std::string(_netAddress.data()) + ":" + std::to_string(_netPort);

    for (std::int32_t i = 0; i < count; ++i)
    {
        // One user root per child, so options.json, the log and any capture land
        // in the child's own directory instead of over the parent's. The asset
        // root stays shared, opened read-only on the child's side.
        std::filesystem::path userRoot = std::filesystem::temp_directory_path() /
                                         ("assisi-pie-client-" + std::to_string(i));
        std::error_code ec;
        std::filesystem::create_directories(userRoot, ec);

        std::vector<std::string> args{"--pie-client", "--connect", endpoint};
        std::vector<std::string> env{"ASSISI_USER_ROOT=" + userRoot.string()};

        Assisi::App::ChildProcess child;
        if (!child.Spawn(*exe, args, env, exe->parent_path()))
        {
            Assisi::Core::Log::Error("PIE: failed to launch client {} of {}.", i + 1, count);
            break;
        }
        _pieClients.push_back(std::move(child));
    }

    Assisi::Core::Log::Info("PIE: {} client(s) launched against {}.", _pieClients.size(), endpoint);
}

void EditorApp::ShutdownPieClients()
{
    for (Assisi::App::ChildProcess &child : _pieClients)
        child.Terminate();
    _pieClients.clear();

    if (!_pieTempLevel.empty())
    {
        std::error_code ec;
        std::filesystem::remove(_pieTempLevel, ec);
        _pieTempLevel.clear();
    }
}

// Joining leaves the camera exactly where the author pointed it, deliberately.
//
// Nothing has to preserve it: the fly camera is plain EditorApp state
// (_cameraTransform), not an entity, so a level load cannot disturb it. What must
// not come back is auto-framing. Focusing "the first mirrored entity" frames
// whichever one happened to arrive first — in a level of falling crates, a crate
// mid-fall — so the camera landed somewhere that depended on how long the join
// took, further underground the later you joined. Double-clicking an entity in
// the list is the deliberate version of the same thing.

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
    Assisi::Core::Log::Info("Editor: network session closed.");
}

void EditorApp::PollNetSession(float dt)
{
    if (!_netSession)
        return;

    // Before Poll, so a hello that has been waiting on the scan goes out on the
    // same frame it landed.
    if (Assisi::App::ContentSet content; _contentSetHash.Poll(content))
    {
        Assisi::Core::Log::Info("Editor: content set hashed ({}, {} files).", Assisi::Core::ToHex64(content.hash),
                                content.paths.size());
        // Here rather than at Host/Join, because this is where the list the
        // handshake hashed exists. The same call the headless path makes.
        if (_world != nullptr)
            Assisi::App::ApplyContentSet(*_netSession, *_world, std::move(content));
    }

    _netSession->Poll();

    if (_netIntent == NetIntent::Standalone)
        return;

    // A host that went away, a rejected handshake, a transport fault: Poll turns
    // all of them into an Offline session, and the play session goes with it. The
    // client's world *was* the host's world — there is nothing left to look at
    // once the stream stops.
    if (!_netSession->IsActive())
    {
        if (_netError.empty())
            _netError = _netSession->LastError().empty() ? "the session ended" : _netSession->LastError();
        _pendingStopPlay = true;
        return;
    }

    if (_joinPhase == JoinPhase::Connecting)
    {
        if (_netSession->IsAwaitingLevel())
        {
            // Marshalled, not done here: building the world frees and re-resolves
            // GPU assets this frame's draws may already reference.
            _joinPhase        = JoinPhase::Building;
            _pendingJoinBuild = true;
            return;
        }

        // The timeout clock only runs once both sides can answer: a first scan of
        // a large asset tree is not a dead host, but timing out on it reads as one.
        if (!_netSession->HasContentSetHash())
            return;

        _joinElapsed += dt;
        if (_joinElapsed >= kJoinTimeoutSeconds)
            FailJoin("no answer from the host — check the address and that it is hosting.");
    }
}

void EditorApp::TickNetSession()
{
    if (_netSession)
        _netSession->Tick(GetSimTick());
}

void EditorApp::SmoothNetView()
{
    // Must run from OnRender, after the physics writeback: it decays a bodied
    // mirror's visual offset onto the pose the writeback just wrote, so running
    // first means writing an offset the writeback then erases. (It used to live in
    // OnUpdate, which is safe only for interpolated mirrors — they have no
    // writeback to lose to.)
    if (_netSession)
        _netSession->SmoothView(ImGui::GetIO().DeltaTime);
}

// ---------------------------------------------------------------------------
// Panels
// ---------------------------------------------------------------------------

void EditorApp::DrawHostAuthoringWarnings()
{
    if (_scene == nullptr)
        return;

    // Both counts are a mismatch between what the author marked and what clients
    // will actually see — otherwise found by watching a second window and
    // wondering.
    std::size_t marked          = 0;
    std::size_t unmarkedDynamic = 0;
    _scene->ForEachEntity(
        [&](Assisi::ECS::Entity entity)
        {
            const bool replicated = _scene->Has<Assisi::NetSync::Replicated>(entity);
            if (replicated)
                ++marked;

            const auto *descriptor = _scene->Get<Assisi::Physics::RigidBodyDescriptor>(entity);
            if (!replicated && descriptor != nullptr && !descriptor->isStatic)
                ++unmarkedDynamic;
        });

    if (marked == 0)
    {
        ImGui::TextColored(kWarnColor, "Nothing in this level is marked Replicated — clients will connect to a "
                                       "world that never changes.");
    }

    if (unmarkedDynamic > 0)
    {
        ImGui::TextColored(kWarnColor, "%zu unmarked dynamic bod%s", unmarkedDynamic,
                           unmarkedDynamic == 1 ? "y" : "ies");
        // Drift is the obvious half; contacts are the half that bites. A cosmetic
        // crate rolling against a *sleeping mirror* fights the client's sleep
        // enforcement, and can jitter or settle at a pose the host never saw.
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("These simulate locally as cosmetic physics — they will settle differently on "
                              "every client, and their collisions with replicated bodies happen only on the "
                              "machine they are on. Mark them Replicated to synchronize.");
        }
    }
}

/// The prompt StartPlay raises instead of hosting a level with unsaved edits.
///
/// Clients load the level from disk, so they see the last *saved* version. Host
/// past that and replicated bodies are corrected against geometry no client has —
/// which surfaces much later, and far from here, as objects bouncing off nothing.
/// A modal rather than a warning label because the failure is remote and delayed:
/// nobody traces it back to an amber line they glanced past at host time.
void EditorApp::DrawHostUnsavedModal()
{
    static constexpr const char *kTitle = "Host with unsaved edits?";

    if (_hostPromptOpen && !ImGui::IsPopupOpen(kTitle))
        ImGui::OpenPopup(kTitle);
    if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    ImGui::TextWrapped("Clients load this level from disk, so they will see the last SAVED version — not the "
                       "edits you have made since. Replicated bodies then get corrected against geometry the "
                       "clients cannot see, which shows up much later as objects bouncing off nothing.");
    ImGui::Separator();

    const auto close = [this]
    {
        _hostPromptOpen = false;
        ImGui::CloseCurrentPopup();
    };

    if (ImGui::Button("Save and host"))
    {
        close();
        if (_world != nullptr && !_world->levelPath.empty())
            SaveLevel(LevelStem(_world->levelPath));
        StartPlay(NetIntent::Host);
    }
    ImGui::SetItemDefaultFocus();
    ImGui::SameLine();
    if (ImGui::Button("Host last-saved"))
    {
        close();
        _hostIgnoreDirty = true; // one attempt only
        StartPlay(NetIntent::Host);
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
        close();

    ImGui::EndPopup();
}

void EditorApp::DrawNetworkWindow()
{
    if (!ImGui::Begin("Network"))
    {
        ImGui::End();
        return;
    }

    // Re-read liveness at every point that depends on it, never once at the top:
    // the buttons below can destroy the session *during this frame*, and a cached
    // flag then describes a session that no longer exists. That is how the
    // Disconnect button used to segfault.
    const auto active = [this] { return IsNetSessionActive(); };

    // ---- status line -------------------------------------------------------
    ImGui::TextUnformatted(_netSession ? _netSession->StatusText().c_str() : "Offline");
    ImGui::Separator();

    // ---- controls ----------------------------------------------------------
    // Host and Join enter Play: the same gesture as pressing Run, with a role
    // attached. The Play control's dropdown (DrawGameControlWindow) starts
    // sessions too and is the primary surface; this panel is the detail view.
    ImGui::SetNextItemWidth(140.f);
    ImGui::InputText("Address", _netAddress.data(), _netAddress.size(),
                     active() ? ImGuiInputTextFlags_ReadOnly : ImGuiInputTextFlags_None);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.f);
    ImGui::InputInt("Port", &_netPort, 0, 0, active() ? ImGuiInputTextFlags_ReadOnly : ImGuiInputTextFlags_None);
    _netPort = std::clamp(_netPort, 1, 65535);

    const bool editing = _playState == PlayState::Editing;
    ImGui::BeginDisabled(!editing || _scene == nullptr);
    if (ImGui::Button("Host"))
        StartPlay(NetIntent::Host);
    ImGui::SameLine();
    if (ImGui::Button("Join"))
        StartPlay(NetIntent::Join);
    ImGui::EndDisabled();
    if (!editing && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Stop first — a session runs inside a play session, so hosting or joining "
                          "*is* pressing Play.");

    ImGui::SameLine();
    ImGui::BeginDisabled(!active());
    if (ImGui::Button("Disconnect"))
        StopPlay();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Ends the play session too — same as Stop.");

    if (!_netError.empty())
        ImGui::TextColored(kErrorColor, "%s", _netError.c_str());

    // Re-checked, not the value from the top of the function: Disconnect above
    // may have just torn the session down.
    if (!active())
    {
        ImGui::Separator();
        if (editing && HostLevelIdentity().addressing == Assisi::NetSync::LevelAddressing::None)
            ImGui::TextColored(kWarnColor, "Save the level before hosting — clients load it from disk.");
        ImGui::TextDisabled("Hosting replicates this scene; entities need a Replicated component to travel.");
        ImGui::End();
        return;
    }

    // ---- live stats --------------------------------------------------------
    const Assisi::NetSync::SessionStats stats = _netSession->Stats();

    ImGui::Separator();

    // Which level the two ends agreed on. Worth a line of its own: everything
    // that goes wrong with a join goes wrong here first, and it is otherwise
    // unanswerable from inside the editor.
    if (const Assisi::NetSync::ServerHello *hello = _netSession->Handshake();
        hello != nullptr && _netSession->IsClient())
    {
        LabelledValue("Level", hello->level.path.empty() ? "<none>" : hello->level.path);
    }
    else if (_netSession->IsHost())
    {
        LabelledValue("Level", _world != nullptr && !_world->levelPath.empty() ? _world->levelPath : "<unsaved>");
    }

    // Where this session actually is. "It says Hosting" and "a client can reach
    // it at this address" are different claims, and only the second is what
    // someone on the other machine types into their Join field.
    LabelledValue("Endpoint", _netSession->IsHost()
                                  ? std::format("listening on :{}", _netPort)
                                  : std::format("{}:{}", _netAddress.data(), _netPort));

    if (_netSession->IsHost())
        DrawHostAuthoringWarnings();

    ImGui::TextUnformatted("Ping");
    ImGui::SameLine(180.f);
    ImGui::TextColored(PingColor(stats.pingMs), "%d ms", stats.pingMs);

    LabelledValue("Bandwidth in", std::format("{:.1f} kB/s", stats.inBytesPerSec / 1024.f));
    LabelledValue("Bandwidth out", std::format("{:.1f} kB/s", stats.outBytesPerSec / 1024.f));

    if (_netSession->IsHost())
    {
        // A minimal authoritative disturbance, so "does the correction stream
        // work" is one click rather than a level built for the purpose. Host-side
        // by construction: there is no client→server state channel, and this is
        // the server acting on its own world.
        const bool canNudge = _selectedEntity != Assisi::ECS::NullEntity && _scene->IsAlive(_selectedEntity) &&
                              _scene->Get<Assisi::Physics::RigidBody>(_selectedEntity) != nullptr &&
                              _scene->Has<Assisi::NetSync::Replicated>(_selectedEntity);
        ImGui::BeginDisabled(!canNudge);
        if (ImGui::Button("Nudge selected body") && canNudge)
        {
            constexpr glm::vec3 kNudge{2.f, 6.f, 0.f};
            _physics->SetBodyLinearVelocity(*_scene->Get<Assisi::Physics::RigidBody>(_selectedEntity), kNudge);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip(canNudge ? "Throw the selected replicated body. Every client should follow within a "
                                         "correction interval."
                                       : "Select a replicated entity with a rigid body.");
        }

        LabelledValue("Clients", std::format("{}", stats.clientCount));
        LabelledValue("Replicated entities", std::format("{}", stats.replicatedEntities));

        // What this session is *capable* of sending, shown rather than inferred.
        // Sending is opt-out, so a future engine module marking a new type
        // replicable would quietly add it to every marked entity; a readable
        // capability list is the cheap fence against finding that months later on
        // a bandwidth graph.
        if (ImGui::TreeNode("Replicable components"))
        {
            for (const Assisi::Core::Reflect::ComponentMeta *meta :
                 Assisi::Core::Reflect::ComponentRegistry::Instance().ReplicableComponents())
            {
                const bool vetoed = std::find(_netVetoedComponentNames.begin(), _netVetoedComponentNames.end(),
                                              meta->name) != _netVetoedComponentNames.end();
                if (vetoed)
                    ImGui::TextDisabled("%s — vetoed by game.json", meta->name.c_str());
                else
                    ImGui::BulletText("%s", meta->name.c_str());
            }
            ImGui::TextDisabled("Per-entity: untick components in the inspector's Sends list.");
            ImGui::TreePop();
        }
        LabelledValue("Snapshots sent", std::format("{}", stats.snapshotsSent));
        LabelledValue("Bytes sent", std::format("{}", stats.bytesSent));
        if (stats.snapshotsSent > 0)
            LabelledValue("Avg snapshot", std::format("{} B", stats.bytesSent / stats.snapshotsSent));

        // Zero is the normal state. A number that stays high means the byte
        // budget is binding and correction *frequency* is degrading — which the
        // priority accumulator makes fair, not free.
        ImGui::TextUnformatted("Dirty backlog");
        ImGui::SameLine(180.f);
        ImGui::TextColored(stats.dirtyBacklog > 0 ? kWarnColor : ImVec4{0.6f, 0.6f, 0.6f, 1.f}, "%u entit%s",
                           stats.dirtyBacklog, stats.dirtyBacklog == 1 ? "y" : "ies");
        LabelledValue("Keyframe sweeps", std::format("{}", stats.keyframeSweeps));

        // --- relevancy ------------------------------------------------------
        // Its own group because boundary thrash is invisible otherwise: it looks
        // exactly like ordinary bandwidth, and it is the failure mode hysteresis
        // exists to prevent. Watch for enters climbing in lockstep with exits.
        if (ImGui::TreeNodeEx("Relevancy", ImGuiTreeNodeFlags_DefaultOpen))
        {
            LabelledValue("Largest set", std::format("{} of {}", stats.relevantEntities,
                                                     stats.replicatedEntities));
            LabelledValue("Entered", std::format("{}", stats.relevancyEnters));

            const bool thrashing = stats.relevancyExits > 0 && stats.relevancyEnters > stats.relevancyExits * 2;
            ImGui::TextUnformatted("Left");
            ImGui::SameLine(180.f);
            ImGui::TextColored(thrashing ? kWarnColor : ImVec4{0.6f, 0.6f, 0.6f, 1.f}, "%llu",
                               static_cast<unsigned long long>(stats.relevancyExits));
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Entities crossing in and out of a client's set. Enters and exits climbing "
                                  "together means entities are oscillating on the boundary — widen the exit "
                                  "radius or the dwell.");
            }
            ImGui::TreePop();
        }

        // --- messages -------------------------------------------------------
        // Split by *why*, not by count: "intents dropped" is not a diagnosis. A
        // rate-limited client is misbehaving, a stale one has a clock problem, a
        // rejected one is lying or mismatched, an unhandled one means a handler
        // was never written.
        if (ImGui::TreeNodeEx("Messages", ImGuiTreeNodeFlags_DefaultOpen))
        {
            LabelledValue("Intents accepted", std::format("{}", stats.intentsAccepted));
            NetCounterRow("Intents rejected", stats.intentsRejected,
                          "A field outside its declared range, an event sent as an intent, or an entity the "
                          "sender does not control. Rejected, never clamped.");
            NetCounterRow("Intents rate-limited", stats.intentsRateLimited,
                          "A client exceeding the per-type ceiling. Dropped before the payload is decoded.");
            NetCounterRow("Intents stale", stats.intentsStale,
                          "A client tick outside the accepted window — too old to act on, or too far ahead "
                          "to have happened.");
            NetCounterRow("Intents unhandled", stats.intentsUnhandled,
                          "A registered message type with no AMSG_HANDLER. Normal if deliberate.");

            LabelledValue("Events sent", std::format("{}", stats.eventsSent));
            LabelledValue("Announcements", std::format("{}", stats.announcementsSent));
            NetCounterRow("Events held", stats.eventsHeld,
                          "Waiting for the entity they are about to reach that client. A number that stays "
                          "high means events about entities the client will never be told about.");
            NetCounterRow("Events evicted", stats.eventsEvicted,
                          "Held events whose subject despawned before it ever arrived.");
            ImGui::TreePop();
        }
    }
    else
    {
        LabelledValue("Server tick", std::format("{}", stats.serverTick));
        LabelledValue("Mirrored entities", std::format("{}", stats.replicatedEntities));
        LabelledValue("Snapshots applied", std::format("{}", stats.snapshotsApplied));

        LabelledValue("Events received", std::format("{}", stats.eventsDispatched));
        NetCounterRow("Events unhandled", stats.eventsUnhandled,
                      "The host sent a message type nothing here handles. Normal if deliberate.");
        NetCounterRow("Announcements waiting", stats.eventsHeld,
                      "Reliable events that arrived ahead of the world they describe, held until the applied "
                      "tick catches up.");

        // --- the correction stream ------------------------------------------
        // Rates, not totals: a climbing total only says the session is running,
        // which you could already see. Sampled over a second so a frame-rate
        // stutter does not read as a bandwidth spike.
        _netSampleSeconds += ImGui::GetIO().DeltaTime;
        if (_netSampleSeconds >= 1.f)
        {
            _correctionBytesPerSecond =
                static_cast<float>(stats.correctionBytes - _lastCorrectionBytes) / _netSampleSeconds;
            _correctionsPerSecond =
                static_cast<float>(stats.correctionsApplied - _lastCorrectionsApplied) / _netSampleSeconds;
            _lastCorrectionBytes      = stats.correctionBytes;
            _lastCorrectionsApplied   = stats.correctionsApplied;
            _netSampleSeconds         = 0.f;
        }

        ImGui::SeparatorText("Corrections");
        LabelledValue("Corrections/s", std::format("{:.1f}", _correctionsPerSecond));
        LabelledValue("Correction bytes/s", std::format("{:.0f} B/s", _correctionBytesPerSecond));

        // The measurement that has to justify the correction cadence, since no
        // determinism argument can: the client starts from state that crossed a
        // wire, applies corrections the server never applies, and adds bodies on a
        // different schedule, so "same binary" buys nothing here.
        LabelledValue("Divergence (mean)", std::format("{:.1f} mm", stats.divergenceMean * 1000.f));
        ImGui::TextUnformatted("Divergence (max)");
        ImGui::SameLine(180.f);
        ImGui::TextColored(stats.divergenceMax > 0.5f ? kWarnColor : ImVec4{0.6f, 0.6f, 0.6f, 1.f}, "%.1f mm",
                           static_cast<double>(stats.divergenceMax) * 1000.0);

        // Silent churn otherwise: a client-side cleanup system running over
        // mirrors would destroy and re-receive them forever with no signal.
        if (stats.mirrorsResurrected > 0)
        {
            ImGui::TextColored(kWarnColor, "Mirrors resurrected: %llu",
                               static_cast<unsigned long long>(stats.mirrorsResurrected));
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("A local system destroyed a mirrored entity. Clients may push mirrors, but "
                                  "destroying one only lasts until the host next mentions it.");
            }
        }

        // Two debug affordances that only make sense together: one to break the
        // mirror, one to heal it. A resync button with nothing broken to heal
        // proves nothing about whether it works.
        if (ImGui::Button("Force full resync"))
            _netSession->RequestKeyframe();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Ask the host to re-send everything from scratch, without waiting for the "
                              "periodic sweep.");

        ImGui::SameLine();
        const bool canScramble = _selectedEntity != Assisi::ECS::NullEntity && _scene->IsAlive(_selectedEntity) &&
                                 _scene->Has<Assisi::NetSync::Mirrored>(_selectedEntity) &&
                                 _scene->Get<Assisi::Physics::RigidBody>(_selectedEntity) != nullptr;
        ImGui::BeginDisabled(!canScramble);
        if (ImGui::Button("Corrupt selected mirror") && canScramble)
        {
            // Client-side damage the host has no way to know about — the exact
            // failure class the keyframe sweep exists for.
            const Assisi::Physics::RigidBody *body = _scene->Get<Assisi::Physics::RigidBody>(_selectedEntity);
            const auto [position, rotation]        = _physics->GetBodyTransform(*body);
            _physics->ApplyBodyState(*body, position + glm::vec3{0.f, 3.f, 1.5f}, rotation, glm::vec3{0.f},
                                     glm::vec3{0.f}, /*activate=*/false);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip(canScramble ? "Shove this mirror somewhere the host never put it. Nothing in the "
                                            "delta path can notice; only a resync (or the sweep) heals it."
                                          : "Select a mirrored entity with a body.");
        }
        ImGui::SeparatorText("Clock");

        // Nonzero is never normal: either corruption the transport did not catch,
        // or a protocol bug. Red, not amber.
        ImGui::TextUnformatted("Snapshots rejected");
        ImGui::SameLine(180.f);
        ImGui::TextColored(stats.snapshotsRejected > 0 ? kErrorColor : ImVec4{0.6f, 0.6f, 0.6f, 1.f}, "%llu",
                           static_cast<unsigned long long>(stats.snapshotsRejected));

        // Buffer depth is the honest health signal for the clock: zero means the
        // server ran out of our input, which the player feels as dropped input.
        ImGui::TextUnformatted("Input buffer");
        ImGui::SameLine(180.f);
        ImGui::TextColored(stats.inputBufferDepth == 0 ? kWarnColor : ImVec4{0.4f, 0.9f, 0.4f, 1.f}, "%u command%s",
                           stats.inputBufferDepth, stats.inputBufferDepth == 1 ? "" : "s");

        LabelledValue("Clock lead", std::format("{} ticks", stats.clockLead));
        LabelledValue("Clock corrections", std::format("{}", stats.clockCorrections));

        if (_joinPhase == JoinPhase::Connecting)
            // %f takes a double through varargs, so the float promotes either
            // way; saying so silences -Wdouble-promotion.
            ImGui::TextColored(kWarnColor, "Joining — waiting for the host (%.0f s)...",
                               static_cast<double>(_joinElapsed));
        else if (!stats.worldComplete)
            ImGui::TextColored(kWarnColor, "Still receiving the initial world...");
    }

    ImGui::End();
}

} // namespace Assisi::Editor
