/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file EditorNet.cpp
/// @brief The editor's half of a networked play session: the join sequence, the
/// host-time guards, and the panel that shows what is going on.
///
/// **A network session exists only inside a play session** (docs/
/// replication-plan-v4.md §3.6). Hosting starts by entering Play; a client joins
/// by entering Play with a join target; Stop — either side, any reason — tears
/// the session down. Hosting from here is the listen server: the scene the
/// editor is already simulating and rendering is the one being replicated, with
/// no second scene and no self-interpolation for the host player (see
/// NetSession.hpp for why the design's loopback-pair framing is not what this
/// does).
///
/// The reason the rule is worth having is what it deletes. A joined client is in
/// Play, so its world is the *play* scene, which the editor already treats as
/// disposable: Save is already gated on Editing, the play snapshot already
/// preserves the editing scene exactly, and Stop already restores it with its
/// undo history intact. What is left here is only what is genuinely new —
/// negotiating which level to load, proving both sides have the same bytes, and
/// stripping the entities the host owns.

#include <Assisi/Editor/EditorApp.hpp>

#include <Assisi/App/ChildProcess.hpp>
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
#include <optional>
#include <string>
#include <vector>

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

constexpr ImVec4 kWarnColor{0.9f, 0.8f, 0.3f, 1.f};
constexpr ImVec4 kErrorColor{0.9f, 0.4f, 0.4f, 1.f};

void LabelledValue(const char *label, const std::string &value)
{
    ImGui::TextUnformatted(label);
    ImGui::SameLine(180.f);
    ImGui::TextUnformatted(value.c_str());
}

/// A counter whose *normal* value is zero, coloured only when it is not.
///
/// The whole point of the messaging counters is that each one names a different
/// problem, so a row that is merely nonzero should draw the eye — and a row that
/// is zero should not. The tooltip carries what the number means, because a
/// count without a diagnosis is a count nobody acts on.
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
    // Out through the same door as everything else. "How do I get out of a bad
    // join" should have exactly one answer, and it is the button that was
    // already there.
    _pendingStopPlay = true;
}

void EditorApp::StripReplicatedEntities()
{
    if (_scene == nullptr)
        return;

    // The host owns these; they arrive as mirrors. The level file's copies are
    // the host's authored originals, so keeping both would double every
    // replicated object in the world.
    std::vector<Assisi::ECS::Entity> doomed;
    _scene->ForEachEntity(
        [&](Assisi::ECS::Entity entity)
        {
            if (_scene->Has<Assisi::NetSync::Replicated>(entity))
                doomed.push_back(entity);
        });

    for (const Assisi::ECS::Entity entity : doomed)
    {
        if (const auto *body = _scene->Get<Assisi::Physics::RigidBody>(entity))
        {
            _physics->RemoveBody(*body);
            _scene->Remove<Assisi::Physics::RigidBody>(entity);
        }
        _scene->Destroy(entity);
    }
    _scene->FlushDestroyed();

    // Anything that was parented to a stripped entity now points at a dead
    // handle. Left alone it dangles into the transform propagation, which reads
    // the child as a root and draws it at its *local* pose — a decoration
    // detached from the thing it decorated, in a world that otherwise looks
    // right. Dropping the link says the same thing honestly.
    std::vector<Assisi::ECS::Entity> orphans;
    _scene->ForEachEntity(
        [&](Assisi::ECS::Entity entity)
        {
            const auto *parent = _scene->Get<Assisi::Runtime::Parent>(entity);
            if (parent != nullptr && !_scene->IsAlive(parent->parent))
                orphans.push_back(entity);
        });
    for (const Assisi::ECS::Entity entity : orphans)
        _scene->Remove<Assisi::Runtime::Parent>(entity);

    if (!doomed.empty() || !orphans.empty())
    {
        Assisi::Core::Log::Info("Editor: join stripped {} replicated entit{} from the level ({} orphan link{}).",
                                doomed.size(), doomed.size() == 1 ? "y" : "ies", orphans.size(),
                                orphans.size() == 1 ? "" : "s");
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
        // already announced — so they go to the log, and the message says the
        // thing the player can act on.
        Assisi::Core::Log::Error("Editor: level content hash mismatch for '{}' — host {}, local {}.", level.path,
                                 Assisi::Core::ToHex64(level.contentHash), Assisi::Core::ToHex64(*localHash));
        FailJoin("your copy of '" + level.path + "' differs from the host's; sync the file from the host and retry.");
        return;
    }

    // --- Build it -----------------------------------------------------------
    // Straight into the play scene. The pre-play snapshot already holds the
    // editing scene, so there is nothing here to preserve and no confirmation to
    // ask for — the thing v3.5 spent a rule on is simply not a hazard once the
    // join happens inside Play.
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
    // world's identity and profile back.
    _world->levelPath = level.addressing == Assisi::NetSync::LevelAddressing::Virtual ? level.path : std::string{};
    _worlds.ApplyProfile(*_world, header.profile);

    StripReplicatedEntities();

    // A load rebuilds entity identity from scratch, so anything holding a handle
    // from the pre-join scene now aliases a live but unrelated entity.
    _selectedEntity     = Assisi::ECS::NullEntity;
    _eyedropperArmed    = false;
    _eyedropperEntity   = Assisi::ECS::NullEntity;
    _eyedropperMeta     = nullptr;
    _assetBrowserOpen   = false;
    _assetBrowserEntity = Assisi::ECS::NullEntity;
    _assetBrowserMeta   = nullptr;

    // Only now: from here a NetId has somewhere to land.
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

    // Under the user root rather than the asset root: this is a transient of one
    // play session, not content, and it must not appear in the asset browser or
    // pick up a GUID sidecar. The clients address it absolutely, so where it
    // lives is nobody else's business.
    const auto resolved = Assisi::Core::AssetSystem::ResolveUser("pie-host-level.alvl");
    if (!resolved)
    {
        Assisi::Core::Log::Error("PIE: cannot resolve a temp level path under the user root.");
        return false;
    }

    const Assisi::Runtime::LevelHeader header{.instances = {},
                                              .profile = _world != nullptr ? _world->profile : std::string{}};
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
        // Each child gets its own user root. That is the whole of the
        // "no shared-file writes" rule for per-user state: options.json, the
        // log, and any capture land in the child's own directory instead of
        // over the parent's. The asset root stays shared and is opened
        // read-only on the child's side.
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

// Joining deliberately leaves the camera exactly where the author left it.
//
// Nothing here has to preserve it: the fly camera is plain EditorApp state
// (_cameraTransform), not an entity, so the level load below rebuilds the scene
// without the camera noticing. What had to go was code that moved it on purpose.
//
// That code focused the first mirrored entity, on the theory that a viewer window
// opening onto nothing undermines the one-click demo. But "first mirrored entity"
// means whichever one happened to arrive first, and in a level of falling crates
// that is a crate, mid-fall — so the camera was flung somewhere that depended on
// how long the join took, and the later you joined the further underground you
// ended up. Framing on join is a guess about what the author wants to look at,
// and the author already told us by pointing the camera before they joined.
// Someone who does want to go somewhere specific can double-click an entity in
// the list, which is the deliberate version of the same thing.

void EditorApp::OnShutdown()
{
    // Closing the window is a way of ending a play session, and the two things
    // that outlive this process if nobody says otherwise are a socket and a
    // fleet of viewer windows. Deliberately *not* a full StopPlay: the scene
    // restore it runs re-resolves assets against a renderer that is on its way
    // down, and nothing is going to look at the result.
    ShutdownNetSession();
    ShutdownPieClients();
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
    Assisi::Core::Log::Info("Editor: network session closed.");
}

void EditorApp::PollNetSession(float dt)
{
    if (!_netSession)
        return;

    // Before Poll, so a hello that has been waiting on the scan goes out on the
    // same frame it landed.
    if (std::uint64_t hash = 0; _contentSetHash.Poll(hash))
    {
        Assisi::Core::Log::Info("Editor: content set hashed ({}).", Assisi::Core::ToHex64(hash));
        _netSession->SetContentSetHash(hash);
    }

    _netSession->Poll();

    if (_netIntent == NetIntent::Standalone)
        return;

    // A host that went away, a rejected handshake, a transport fault: Poll turns
    // all of them into an Offline session. The play session goes with it — the
    // client's world *was* the host's world, and there is nothing left to look
    // at once the stream stops.
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

        // Phase-aware: a first scan of a large asset tree is not a dead host, and
        // timing out on it would read as one. The clock only runs once both sides
        // have everything they need to answer.
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
    // Called from OnRender, immediately after the physics writeback: it decays a
    // bodied mirror's visual offset onto the pose the writeback just wrote, so
    // running before the writeback would mean writing an offset the writeback
    // then erases. (It used to run in OnUpdate for exactly that reason —
    // interpolated mirrors have no writeback to lose to.)
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

    // Both of these are about a mismatch between what the author marked and what
    // the clients will actually see, and both are otherwise discovered by
    // watching a second window and wondering.
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
        // The drift is the obvious half and the contacts are the half that
        // actually bites: a cosmetic crate rolling against a *sleeping mirror*
        // fights the client's sleep enforcement, and can jitter or come to rest
        // at a pose the host never saw.
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("These simulate locally as cosmetic physics — they will settle differently on "
                              "every client, and their collisions with replicated bodies happen only on the "
                              "machine they are on. Mark them Replicated to synchronize.");
        }
    }
}

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

    // Re-read the session's liveness at every point that depends on it, never
    // once at the top. The buttons below can destroy it *during this frame*,
    // and a cached "is it active" flag then describes a session that no longer
    // exists — which is precisely how the Disconnect button used to segfault.
    const auto active = [this] { return IsNetSessionActive(); };

    // ---- status line -------------------------------------------------------
    ImGui::TextUnformatted(_netSession ? _netSession->StatusText().c_str() : "Offline");
    ImGui::Separator();

    // ---- controls ----------------------------------------------------------
    // Host and Join enter Play. They are the same gesture as pressing Run, with
    // a role attached — which is the whole of §3.6 in two buttons. (R3 moves
    // both into the Play control's dropdown so one surface answers "where do I
    // host?" and "where do I join?"; this panel then keeps the detail view.)
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
    // that goes wrong with a join goes wrong here first, and "which level do
    // they think we are on" is otherwise unanswerable from inside the editor.
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
    // it at this address" are different claims, and only the second one is what
    // someone on the other machine needs typed into their Join field.
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
        // actually work" is one click rather than a level built for the purpose.
        // Host-side by construction: there is no client→server state channel in
        // this design, and this is the server acting on its own world.
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
        // Default-send polarity means a future engine module marking a new type
        // replicable would quietly add it to every marked entity; a capability
        // surface you can read is the cheap fence against noticing that months
        // later on a bandwidth graph.
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
        // budget is binding, and correction *frequency* is degrading — which the
        // priority accumulator makes fair, not free.
        ImGui::TextUnformatted("Dirty backlog");
        ImGui::SameLine(180.f);
        ImGui::TextColored(stats.dirtyBacklog > 0 ? kWarnColor : ImVec4{0.6f, 0.6f, 0.6f, 1.f}, "%u entit%s",
                           stats.dirtyBacklog, stats.dirtyBacklog == 1 ? "y" : "ies");
        LabelledValue("Keyframe sweeps", std::format("{}", stats.keyframeSweeps));

        // --- relevancy ------------------------------------------------------
        // Worth its own group because boundary thrash is invisible otherwise:
        // it looks exactly like ordinary bandwidth, and it is the one failure
        // mode hysteresis exists to prevent. Enters climbing in lockstep with
        // exits is the shape to watch for.
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
        // Split by *why* rather than by count. "Intents dropped" is not a
        // diagnosis: a rate-limited client is misbehaving, a stale one has a
        // clock problem, a rejected one is lying or mismatched, and an
        // unhandled one means somebody forgot to write a handler.
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
        // Rates, not totals: a total that keeps climbing tells you the session is
        // still running, which you could already see. Sampled over a second so a
        // frame-rate stutter does not read as a bandwidth spike.
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

        // Divergence is the measurement the correction cadence has to be
        // justified by. There is no determinism argument available to justify it
        // instead: the client starts from state that crossed a wire, applies
        // corrections the server never applies, and adds bodies on a different
        // schedule, so "same binary" buys nothing here.
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
        // mirror, one to heal it. A resync button with nothing to heal proves
        // nothing about whether it works.
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

        // A nonzero rejection count is never normal: it means either corruption
        // the transport did not catch or a protocol bug. Make it loud.
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
            // Explicit widening: %f consumes a double through varargs, so the
            // float would promote anyway — saying so silences -Wdouble-promotion
            // without changing a byte of behaviour.
            ImGui::TextColored(kWarnColor, "Joining — waiting for the host (%.0f s)...",
                               static_cast<double>(_joinElapsed));
        else if (!stats.worldComplete)
            ImGui::TextColored(kWarnColor, "Still receiving the initial world...");
    }

    ImGui::End();
}

} // namespace Assisi::Editor
