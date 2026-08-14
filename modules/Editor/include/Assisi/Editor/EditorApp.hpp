/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file EditorApp.hpp
/// @brief The Assisi editor application, built on the Application layer.
///
/// The one public class of the editor library: an editor executable constructs
/// an EditorApp with an EditorConfig carrying the game's hooks and calls
/// Initialize()/Run(). The implementation is split across translation units by
/// concern (all private to the library):
///   - EditorApp.cpp           lifecycle/setup, diagnostics, OnImGui dispatch,
///                             undo/redo plumbing, stale-asset prompt
///   - EditorAssetBrowser.cpp  asset browser + thumbnails
///   - EditorBlueprintMode.cpp blueprint editing mode + re-expansion on save
///   - EditorCamera.cpp        fly camera, entity picking, eyedropper
///   - EditorColliders.cpp     collider wireframes and silhouette outlines
///   - EditorGizmo.cpp         transform and instance gizmos
///   - EditorInspector.cpp     reflected component field editing
///   - EditorLevels.cpp        level/blueprint scan, load, save, instancing
///   - EditorNet.cpp           net session, join, play-in-editor clients
///   - EditorPlay.cpp          play control, selection, entity create/delete
///   - EditorWorlds.cpp        resident worlds, travel, migration
/// (EditorOptions.cpp implements EditorOptionsPanel, not this class.)

#include <Assisi/App/Application.hpp>
#include <Assisi/App/ChildProcess.hpp>
#include <Assisi/App/ContentSet.hpp>
#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Window/ActionMap.hpp>

#include <Assisi/Core/AssetDatabase.hpp>
#include <Assisi/Core/Reflect/Annotations.hpp>
#include <Assisi/Core/Reflect/ComponentMeta.hpp>
#include <Assisi/Geometry/AssetImport.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#if defined(ASSISI_NETWORKING)
#    include <Assisi/NetSync/NetSession.hpp>
#endif
#include <Assisi/Physics/PhysicsWorld.hpp>
#include <Assisi/Render/AssetCache.hpp>
#include <Assisi/Render/GeometryArena.hpp>
#include <Assisi/Render/MeshBuffer.hpp>
#include <Assisi/Render/RenderFrame.hpp>
#include <Assisi/Render/Texture.hpp>
#include <Assisi/Runtime/SceneRenderer.hpp>

#include <Assisi/Editor/EditHistory.hpp>
#include <Assisi/Editor/GizmoDrag.hpp>
#include <Assisi/Editor/InstanceGesture.hpp>
#include <Assisi/Editor/PrePlayState.hpp>
#include <Assisi/Editor/ScenePick.hpp>

#include <nvrhi/nvrhi.h>

#include <array>
#include <memory>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Assisi::Editor
{

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

/// @brief Emitted when the user clicks to select (or deselect) an entity.
/// NullEntity means the click landed on empty space.
AEVENT()
struct EntitySelectionChangedEvent
{
    Assisi::ECS::Entity entity;
    /// Ctrl or Shift was held: fold the entity into the selection instead of
    /// replacing it. Both modifiers do the same thing in the viewport — a range
    /// needs an order to walk through, and the 3D view has none.
    bool additive = false;
};

// ---------------------------------------------------------------------------
// EditorApp
// ---------------------------------------------------------------------------

/// @brief The game side's hooks into the editor, supplied at construction.
///
/// Seam contract:
///  - Game systems tick ONLY while the editor is Playing (F5) — never while
///    Editing or Paused. The editor runs their PreUpdate/Update/PostUpdate
///    phases (in that order, after its own systems) from OnUpdate, and their
///    FixedUpdate phase from OnFixedUpdate just before the physics step.
///  - Render systems do not run in-editor — the editor owns rendering. OnStart
///    warns once if any are declared.
///  - After()/Before() ordering resolves within one registry only: game systems
///    cannot order against editor systems, by design.
///  - There is no play-start/stop notification, so a game system holding entity
///    handles across a play session has no reset hook.
struct EditorConfig
{
    // Systems are declared with ASYSTEM, reach the catalog by being linked, and
    // a level names the ones it wants — there is deliberately nothing to
    // register here. The hooks this replaced had to be remembered, and a host
    // that forgot got a world that physics-stepped and ran no game logic.

    /// Virtual path (under the asset root) of a level to open once at
    /// startup, e.g. "levels/Materials.alvl". Empty = none. Resolved through
    /// Core::AssetSystem; a missing/typo'd path warns and starts empty.
    std::string startupLevel;

    /// Endpoint ("address" or "address:port") to join automatically once the
    /// editor has started. Empty = none.
    ///
    /// This is the whole of what makes a play-in-editor client a client: a full
    /// editor, launched by another one, that enters Play as a joiner as soon as
    /// it is up. Deliberately no special code path — one that differed would
    /// stop being a test of the thing it is meant to test.
    std::string autoJoinEndpoint;

    /// Run as a restricted viewer: no writes to anything the editor that
    /// spawned this process also owns (see Application::SetRestrictedViewer and
    /// Core::RebuildMode::ReadOnly). Set for a play-in-editor client.
    bool restrictedViewer = false;

    /// Build the SceneRenderer's editor overlay passes (selection outline,
    /// entity icons, collider wireframes). On by default; `--no-editor-visuals`
    /// clears it, so the renderer's off-path — what a Game build runs: passes
    /// never built, no assets/editor/** loads — can be exercised from a stock
    /// editor binary. Distinct from the F11 "Editor overlays" checkbox, which
    /// only hides the overlays per frame.
    bool enableEditorVisuals = true;
};

class EditorOptionsPanel;

class EditorApp : public Assisi::App::Application
{
public:
    explicit EditorApp(EditorConfig config = {});

    /// Out of line: `_options` is a unique_ptr to a type only forward-declared
    /// here, so the deleter cannot be instantiated in this header.
    ~EditorApp();

    /// @brief Transform-gizmo handle set. Public so the free helpers in
    /// EditorGizmo.cpp can map it to ImGuizmo's operation enum.
    enum class GizmoOp : std::uint8_t
    {
        Translate,
        Rotate,
        Scale
    };

    /// @brief How a click folds into the current selection.
    ///
    /// Named after the gesture, not the modifier key: the two do not map
    /// one-to-one. The viewport binds *both* Ctrl and Shift to Toggle (there is
    /// no row order out there to draw a range through); the entity list binds
    /// Shift to Range.
    enum class SelectMode : std::uint8_t
    {
        Replace, ///< Plain click: this entity becomes the whole selection.
        Toggle,  ///< Ctrl-click: add it, or drop it if it was already in.
        Range    ///< Shift-click: everything between the anchor and here.
    };

    void OnStart() override;
    void OnFixedUpdate(float dt) override;
    void OnUpdate(float dt) override;
    void OnRender(Assisi::Render::RenderFrame &frame) override;
    void OnImGui() override;
    void OnResize(int32_t width, int32_t height) override;
    void OnRenderTargetsChanged(const nvrhi::FramebufferInfo &framebufferInfo) override;
    void FlushDeferred() override;
    void InstallQueuedSystems() override;
    void OnShutdown() override;

private:
    // --- Setup ---
    void SetupCamera();
    void SetupScene();

    // --- Camera and picking (per frame) ---
    void HandleEntityPicking();
    void UpdateCamera(float dt);
    /// @brief Recomputes _cameraTransform.worldMatrix from its TRS. The camera is
    /// parentless, so world == local; call before reading the view matrix.
    void RefreshCameraMatrix();
    /// @brief Reseeds _yaw/_pitch from the camera's current rotation so the fly
    /// controller resumes from the new orientation without snapping. Called when
    /// a focus animation ends, or is cancelled by manual look input.
    void SyncYawPitchFromRotation();

    // --- ImGui panels ---
    void DrawOptionsWindow(); // hands the frame to EditorOptionsPanel and applies its result
    void DrawDiagnosticsWindow();
    void DrawChiaraWindow();  // performance capture (F9); empty in builds without profiling
    void DrawLevelsWindow();
    void DrawBlueprintsWindow();
    void DrawInspector();
    void DrawHelloImageWindow(); // ImGui-texture-display smoke test
    void DrawAssetBrowser();
    void DrawGameControlWindow(); // Run/Pause/Stop the simulation (F5/F6/F7)
    void DrawEntityListWindow();  // scene entity list: click selects, double-click focuses
    void DrawHistoryWindow();     // undo/redo stack view; click a row to jump
    void DrawTransformGizmo();    // ImGuizmo manipulator over the selected entity
    /// @brief Draws the manipulator and applies whatever it produced this frame.
    /// Returns whether the handles are held.
    ///
    /// Split out so the drag's release edge sits in the caller: this function early-
    /// returns on four conditions a drag can end through — no scene, a dead or
    /// non-editable entity, a missing Transform — and a release read at the bottom of
    /// it was reachable from none of them (ENG-127). Every such return is "not held",
    /// which is all the caller needs to close the drag.
    [[nodiscard]] bool DrawTransformGizmoHandles();
    void DrawInstanceGizmo();     // …and over a selected blueprint instance, which moves as one
    /// @brief Writes @p world onto @p entity as a local TRS against @p parentWorld,
    /// and syncs any physics body to it. The tail of a gizmo drag, factored out
    /// because a multi-selection runs it once per entity.
    void ApplyGizmoWorldMatrix(Assisi::ECS::Entity entity, const glm::mat4 &parentWorld,
                               const glm::mat4 &world);
    // --- Networking ---------------------------------------------------------
    // Compiled out entirely without networking. The editor is fully usable
    // without it — what disappears is the multiplayer panel, hosting, joining,
    // and play-in-editor *clients*. Plain play-in-editor is not networking and
    // stays.
#if defined(ASSISI_NETWORKING)
    void DrawNetworkWindow();     // negotiated level + live net stats
    void DrawHostUnsavedModal();  // "save and host / host last-saved / cancel"
    /// @brief The two host-side authoring warnings: a level with nothing marked
    /// Replicated, and dynamic bodies that will run as cosmetic local physics.
    /// Both describe a gap between what was marked and what clients will see.
    void DrawHostAuthoringWarnings();
    void ShutdownNetSession();    // tear down and forget; safe to call with no session
    void PollNetSession(float dt); // top of the fixed step: connection events, messages, join progress
    void TickNetSession();        // end of the fixed step: snapshots (host) or input (client)
    void SmoothNetView();         // once per frame, AFTER the physics writeback: interpolation + correction smoothing
#endif // ASSISI_NETWORKING

    // --- Networked play -----------------------------------------------------
    // The rule the rest falls out of: **a network session exists only inside a
    // play session.** Hosting starts by entering Play; a client joins by
    // entering Play with a join target; Stop — either side, any reason — tears
    // the session down. That is what lets a join build its world inside the
    // *play* scene, which the editor already treats as disposable, so nearly
    // every guard an "editing while joined" mode would need is machinery the
    // play snapshot/restore already provides.

    // NetIntent and JoinPhase stay outside the networking guard on purpose:
    // `Standalone` is what *ordinary* Play is, so StartPlay takes one either
    // way and the whole play path would need two spellings without them.
    // Without networking the other enumerators are never reached.

    /// @brief What the current (or next) play session does on the network.
    enum class NetIntent : std::uint8_t
    {
        Standalone, ///< Ordinary Play. No transport is created at all.
        Host,       ///< Play-and-listen: this scene is the replicated one.
        Join,       ///< Play as a client of someone else's world.
    };

    /// @brief How far a joining client has got. Distinct from the session's own
    /// state because "connected" and "has a world to put the snapshots in" are
    /// two different things, and only the editor knows the second.
    enum class JoinPhase : std::uint8_t
    {
        None,       ///< Not joining.
        Connecting, ///< Transport up, waiting for the host's ServerHello.
        Building,   ///< Hello received; loading and stripping the host's level.
        Live,       ///< Handshake answered; snapshots are being applied.
    };

#if defined(ASSISI_NETWORKING)
    /// @brief True while a session exists and is not Offline.
    [[nodiscard]] bool IsNetSessionActive() const;

    /// @brief What this editor would advertise as its level: the edited world's
    /// saved path plus a content hash of the file as it currently sits on disk.
    /// Addressing is `None` when the level has never been saved, which is what
    /// makes "save the level to host" a check rather than a suggestion.
    [[nodiscard]] Assisi::NetSync::LevelIdentity HostLevelIdentity() const;

    /// @brief Build the joined world from the host's handshake: resolve the
    /// level, verify its content hash, load it into the play scene, strip the
    /// entities the host owns, then answer the handshake. Marshalled to the
    /// frame's safe point — it frees and re-resolves GPU assets.
    void BuildJoinedWorld();

    /// @brief Destroy every entity carrying `Replicated` and clear the dangling
    /// `Parent` of anything that was under one. Those entities are the host's;
    /// they arrive as mirrors, and keeping the file's copies too would double
    /// the world.
    void StripReplicatedEntities();

    /// @brief Abort a join in progress with a reason a human can act on: the
    /// panel shows it, the log keeps it, and the session ends through the same
    /// Stop as everything else.
    void FailJoin(std::string reason);

    // --- Play in editor -----------------------------------------------------
    // "Host + N clients" launches N more copies of this executable, each of
    // which joins the listen server this one just started — so that two worlds
    // agreeing can be checked by looking, without choreographing two windows by
    // hand every time.

    /// @brief Snapshot the live scene to a temp level file and describe it as
    /// an absolute-path LevelIdentity.
    ///
    /// This is what makes PIE immune to the unsaved-edits problem that hosting
    /// across machines has to prompt about: the clients load the scene as it is
    /// *right now*, not as it was last saved. Returns false (and logs) if the
    /// file could not be written.
    bool WritePieTempLevel(Assisi::NetSync::LevelIdentity &outLevel);

    /// @brief Launch @p count play-in-editor clients against the local host.
    void SpawnPieClients(std::int32_t count);

    /// @brief Terminate and reap every play-in-editor client, then delete the
    /// temp level. Safe to call when there are none.
    void ShutdownPieClients();
#endif // ASSISI_NETWORKING

    // --- Diagnostics ---
    /// @brief Runs at the end of OnImGui: warns, with full ImGui internal state,
    /// when a widget holds ActiveId for seconds with no mouse button down and no
    /// text edit — the "UI stops responding until a new window opens" wedge.
    void LogImGuiWedgeDiagnostics();

    // --- Collider visualisation ---
    // Builds collider wireframes AND silhouette outlines (collider volume + entity
    // mesh) for every RigidBodyDescriptor and hands them to the renderer (green
    // depth-tested, orange x-ray for the selection), plus the list of collider
    // entities so their editor billboards are suppressed. Editor-only: a no-op
    // while the game is playing.
    void SubmitColliderWireframes();

    // Submits the collider volume's silhouette outline for one body: a box/sphere/
    // cylinder unit mesh scaled to the descriptor, a capsule as a cylinder + two
    // end spheres whose union is the capsule.
    void SubmitColliderOutline(const glm::mat4 &bodyModel, const Assisi::Physics::RigidBodyDescriptor &desc,
                               const glm::vec3 &color);

    // --- Gizmo state ---
    /// @brief True while the transform gizmo is hovered or being dragged. Entity
    /// picking checks this so a click on the gizmo doesn't reselect what's behind it.
    [[nodiscard]] bool IsUsingGizmo() const;

    // --- Asset browser helpers ---
    /// @brief Arms the browser to write into @p meta's field at @p fieldOffset on
    /// the selected entity, and opens the window.
    void OpenAssetBrowserFor(const Assisi::Core::Reflect::ComponentMeta &meta, std::size_t fieldOffset);
    /// @brief Arms the browser to write into element @p slot of an AssetPathVector
    /// field (a MeshRenderer material slot), listing only materials, and opens it.
    void OpenAssetBrowserForSlot(const Assisi::Core::Reflect::ComponentMeta &meta, std::size_t fieldOffset,
                                 int32_t slot);
    /// @brief Writes @p vpath into the pinned browser target field and closes.
    void SelectAsset(std::string_view vpath);
    /// @brief Re-resolves a MeshRenderer entity's mesh/texture from its current
    /// paths, so an inspector/browser edit takes effect without a level reload.
    /// Wraps Runtime::ResolveMeshRendererAssets with the alive/has-component
    /// checks over the selected entity.
    void ReresolveEntityAssets(Assisi::ECS::Entity entity);
    /// @brief Reads _assetBrowserDir into the cached dirs/images lists. Called
    /// only when the listing may have changed, not every frame.
    void RescanAssetBrowser();

    // --- Inspector helpers ---
    bool EditComponentFields(void *mut, const Assisi::Core::Reflect::ComponentMeta &meta);
    // The inspector's replication surfaces. Every one reads or writes a
    // NetSync::Replicated marker, so without networking there is no component
    // for them to be about and the inspector has no replication block.
#if defined(ASSISI_NETWORKING)
    /// @brief The inspector's replication block, drawn under the entity id: the
    /// Replicated checkbox, the session-scoped NetId, which of the two client
    /// timelines a mirror is on, and the warnings that catch an entity that will
    /// not replicate the way its author expects.
    void DrawReplicationSection(bool mirrored);

    /// Per-component send policy for the selected authoring entity: one checkbox
    /// per capable component it *carries*, writing `Replicated::excluded`.
    /// Authoring entities only — a mirror's marker is client-fabricated, so
    /// showing its mask would display data the host never sent.
    ///
    /// Rebuilt from the entity's live component set every frame, so nothing here
    /// can go stale when a component is added or removed.
    void DrawReplicationPolicy();

    /// The Relevance dropdown, drawn only for entities this machine authors.
    ///
    /// Split out for a different honesty rule from the rest of the policy block:
    /// a mirror's Replicated marker is client-fabricated, so showing its
    /// relevance as authorable would dress a default up as the host's decision.
    void DrawRelevancePolicy();

    /// Does this entity currently send @p meta to clients?
    ///
    /// The single *read* behind both policy surfaces — the glyph button on each
    /// component header and the Sends checklist. Neither keeps state of its own,
    /// so they cannot disagree: they are two renderings of one mask.
    [[nodiscard]] bool SelectedEntitySends(const Assisi::Core::Reflect::ComponentMeta &meta) const;

    /// ...and the single *write*, undo-recorded. Same reason.
    void SetSelectedEntitySends(const Assisi::Core::Reflect::ComponentMeta &meta, bool sends);
#endif // ASSISI_NETWORKING

    /// Whether the game config forbids @p meta outright, in which case a
    /// per-entity control for it would be a switch that cannot matter.
    [[nodiscard]] bool IsComponentGameVetoed(const Assisi::Core::Reflect::ComponentMeta &meta) const;

    /// Component names the game config vetoes, cached at session start so a
    /// forbidden component renders as a disabled switch with a reason rather
    /// than a live one that silently does nothing.
    std::vector<std::string> _netVetoedComponentNames;
    /// @brief Draws one editable row per material slot of @p mrc's resolved mesh
    /// (labelled by the imported material name), each a `.amat` path + browse
    /// button writing into `materialOverrides[slot]`. @p fieldOffset is the offset
    /// of the materialOverrides vector within the MeshRenderer. Returns true if a
    /// row was edited (the caller re-resolves).
    bool EditMaterialSlots(Assisi::Runtime::MeshRenderer &mrc,
                           const Assisi::Core::Reflect::ComponentMeta &meta, std::size_t fieldOffset);
    /// @brief Draws the path input for an AssetId field (@p inputId is the ImGui
    /// id): display = the id's resolved virtual path, typing a path re-resolves
    /// the id via the database. Returns true and writes @p id when edited. The
    /// caller lays out the browse button + label to the right.
    bool AssetIdPathField(const char *inputId, Assisi::Core::AssetId &id);
    void HandlePhysicsEditing(bool anyFieldEdited);

    /// @brief Holds the selected entity's rigid body still for the duration of an
    /// edit gesture, and keeps the hold alive.
    ///
    /// **Call every frame** a Transform-editing gesture is held — a gizmo drag,
    /// an Inspector field. The body is made Static, out of the solver's hands
    /// entirely: while being placed it will not be pushed out of what it
    /// overlaps, rotate free of a contact, or stutter against a surface. It still
    /// pushes everything it overlaps, which is the half you do want.
    ///
    /// Idempotent while the same entity stays selected. If the selection moves
    /// mid-gesture the previous body is released first, so a hold can never be
    /// left behind on an entity nothing is editing any more.
    ///
    /// The matching release is NOT here: it happens once per frame at the end of
    /// OnImGui, on the first frame nothing calls this. See ThawEditedBody.
    void RequestPhysicsFreeze();

    /// @brief Returns the body frozen by HandlePhysicsEditing to its authored
    /// motion type and clears the freeze.
    ///
    /// Safe when nothing is frozen, when the world it belonged to is gone, and
    /// when the entity or its body has since been destroyed — all reachable, and
    /// all of which must still clear the state rather than leave a freeze
    /// recorded against something that no longer exists.
    void ThawEditedBody();

    /// @brief Writes an eyedropper-picked entity into the armed EntityRef field.
    void ApplyEyedropperPick(Assisi::ECS::Entity picked);

    // --- Undo/redo (editor-only; see EditHistory.hpp) ---
    /// @brief Ctrl-Z / Ctrl-Y / Ctrl-Shift-Z, handled at the top of OnUpdate — a
    /// safe point: after the prior frame's FlushDestroyed, before systems run.
    /// Gated on nothing being typed into and a history being active.
    void HandleUndoRedoHotkeys();
    /// @brief The EditHistory rebind hook: rebuilds the transient state that
    /// serialization excludes after a component is restored/removed by an apply —
    /// physics body (RigidBodyDescriptor), body pose (Transform), resolved asset
    /// pointers (MeshRenderer). Routed through the same helpers the live edits use.
    void ApplyEditRebind(Assisi::ECS::Entity entity, Assisi::Core::Reflect::ComponentId id, bool present);
    /// @brief Builds the rebind hook bound to this app (shared by both histories).
    Assisi::Editor::EditHistory::RebindHook MakeEditRebindHook();
    /// @brief The history that captures and applies edits *right now*, or nullptr
    /// when editing must not be captured. Editing -> the persistent main history;
    /// Paused -> a scratch history discarded when play resumes or stops; Playing ->
    /// nullptr (the simulation owns the scene, edits are neither captured nor
    /// undoable). This is the single switch every capture/undo site routes through.
    [[nodiscard]] Assisi::Editor::EditHistory *ActiveHistory();
    /// @brief Every history that exists right now, active or not.
    ///
    /// **Ask what an edit *costs* here, not @ref ActiveHistory.** Those are
    /// different questions: a blueprint save destroys members in the *level*
    /// worlds while the active history is the blueprint world's, and EditHistory
    /// refuses any scene it is not bound to — so the cost came back 0, the "this
    /// will drop N undo steps" prompt never opened, and the level's stack kept
    /// transactions naming entities that no longer existed.
    ///
    /// No world→history map is needed: `CountForgettable` and `ForgetEntities`
    /// both take the scene and return 0 for one they do not own, so asking all of
    /// them and summing is correct however many there come to be.
    [[nodiscard]] std::vector<Assisi::Editor::EditHistory *> AllHistories();
    /// @brief True when the scene has edits not yet written to disk — the active
    /// history's position differs from the one recorded at the last successful
    /// SaveLevel. Drives the window-title `*` marker.
    [[nodiscard]] bool IsSceneDirty();
    /// @brief The entity's Name (if it has a non-empty one), else "". Used to tag
    /// undo labels with the affected entity.
    [[nodiscard]] std::string EntityDisplayName(Assisi::ECS::Entity entity) const;
    /// @brief Builds an undo transaction label: the action, plus " - <name>" when
    /// the entity is named (e.g. "Edit Transform - Player"), else its id.
    [[nodiscard]] std::string EditLabel(std::string_view action, Assisi::ECS::Entity entity) const;

    // --- Worlds ---
    /// @brief Switches which world the editor renders and inspects, repointing
    /// `_scene`/`_physics` at it. One world is rendered per viewport, so "shown"
    /// and "active" are the same thing: selecting another world moves the view
    /// there, and the panels follow.
    void SetActiveWorld(Assisi::App::World &world);
    /// @brief Loads @p virtualPath into a NEW resident world and shows it, leaving
    /// the current world alive and simulating. The asset cache is **not** cleared —
    /// the other world's resolved pointers reference it. Returns false (nothing
    /// created) if the level didn't load. Reached from the Game panel's debug
    /// control; a game reaches the same capability through WorldManager.
    bool LoadLevelAsNewWorld(const std::string &virtualPath);
    /// @brief Travels to @p virtualPath: the running game changes level without
    /// leaving Play. The edited world goes dormant (Stop still restores it); any
    /// other outgoing world is destroyed. A failed travel keeps play running
    /// where it is. Reached from the Game panel's debug control; a game calls
    /// WorldManager::LoadLevel directly.
    bool TravelToLevel(const std::string &virtualPath);
    /// @brief Migrates the selected entity (and its subtree) into the named
    /// resident world, then clears the selection. Debug stand-in for the
    /// game marking entities as travelling.
    void MigrateSelectionTo(const std::string &targetWorld);
    /// @brief Starts a background preload of @p virtualPath — the running world
    /// keeps simulating. Poll via _worlds.PendingLoadReady(); swap with
    /// PromotePreloadedWorld(). The async form of TravelToLevel.
    void BeginPreload(const std::string &virtualPath);
    /// @brief Swaps the finished preload in as active (instant) and shows it,
    /// then sweeps the asset cache. No-op if nothing is preloaded.
    void PromotePreloadedWorld();
    /// @brief Destroys every world the play session created and shows the edited
    /// one again. Called by StopPlay before it restores the snapshot.
    void DestroyPlayWorlds();
    /// @brief True when edits may be captured and saved — i.e. the world being
    /// shown is the edited world. Other residents are inspect-only, so the panels
    /// disable their editing controls.
    [[nodiscard]] bool IsEditable() const;

    /// @brief True when @p entity may be edited: the world allows it *and* the
    /// entity is not a mirror of somebody else's.
    ///
    /// **One predicate**, gating the inspector, the gizmo, the Delete key and the
    /// hierarchy actions together, because a guard applied to three of the four
    /// is a guard nobody can reason about. It now protects only a disposable play
    /// scene, and still earns its keep: a gizmo fighting the correction stream is
    /// a confusing artifact even in a world about to be thrown away, and the
    /// server only resends what *changes* — so an edit to a static replicated
    /// field would sit visibly wrong until the next keyframe sweep.
    [[nodiscard]] bool IsEditable(Assisi::ECS::Entity entity) const;

    // --- Entity identity ---
    /// @brief A human label for an entity in a picker or a header.
    ///
    /// `car_3 › wheel_fl` for a blueprint member, its Name if it has one, else
    /// `[index:generation]`. Previewing everything as `Entity [41:0]` is already
    /// hard to pick from and unusable once a level holds forty cars.
    [[nodiscard]] std::string DescribeEntity(Assisi::ECS::Entity entity) const;

    /// @brief Whether @p entity is a mirror the server owns.
    [[nodiscard]] bool IsMirrored(Assisi::ECS::Entity entity) const;

    // --- Worlds panel ---
    /// @brief The resident-world dropdown drawn at the top of the Entities panel.
    void DrawWorldSelector();

    // --- Level and blueprint scanning ---
    void ScanLevels();

    /// @brief Refreshes @ref _blueprintFiles from the asset root.
    void ScanBlueprints();

    // --- Blueprint editing mode --------------------------------------------
    //
    // A blueprint is an ordinary level file, so editing one is opening it as a
    // level. What makes it a *mode* rather than another Open Level is that the
    // level you came from stays resident and untouched behind it: saving the
    // blueprint has to bring that level's copies of it up to date, and it cannot
    // do that to a world it just unloaded.
    //
    // The blueprint world takes over the **edited** role while it is open, with
    // its own undo history and dirty marker, and hands the role back on close.
    // Two live histories rather than one saved and restored: the level's must
    // survive the round trip exactly, and EditHistory binds a Scene by reference.

    /// @brief True while the world being shown is the open blueprint.
    [[nodiscard]] bool InBlueprintMode() const;

    /// @brief Opens @p source (a `.abp`, or any level file) in its own world with
    /// the editor's lighting rig, and switches the editor into blueprint mode.
    ///
    /// **Never call this from a panel**: it loads assets and touches GPU state,
    /// so like any other level load it must run at the frame's main-thread drain.
    /// Reach it through @ref _pendingBlueprintOpen.
    void OpenBlueprintForEditing(const std::string &source);

    /// @brief Leaves blueprint mode: hands the edited role back, destroys the
    /// blueprint world, and shows the level again. No-op if none is open.
    void CloseBlueprintEditor();

    /// @brief Stands up the lighting the blueprint editor works by — a sun and a
    /// raised ambient — so a model is visible without the author having to light
    /// one. Every entity it creates carries Runtime::EditorOnly, which is what
    /// keeps the sun out of the saved file.
    void AddBlueprintEditorRig(Assisi::App::World &world);

    /// @brief The blueprint-mode panel: what is being edited, save/close, and the
    /// live lighting controls.
    void DrawBlueprintEditorWindow();

    /// @brief The blueprint world's sun, or NullEntity if the rig is gone (the
    /// author is free to delete it — it is an ordinary entity).
    [[nodiscard]] Assisi::ECS::Entity BlueprintSunEntity() const;

    // --- Re-expansion on save -----------------------------------------------

    /// @brief One live instance a save would bring up to date, and what it takes.
    struct PendingReexpand
    {
        Assisi::App::World *world = nullptr;
        Assisi::ECS::InstanceId instanceId;
        /// The member names the live tags were written against. Captured before the
        /// definition cache is dropped, because nothing can reconstruct them after.
        std::vector<std::string> previousMemberNames;
        /// The entity behind each of those names, in the same order, captured in the
        /// same breath and for a sharper version of the same reason.
        ///
        /// A `BlueprintMember` tag carries an *index*, and only the definition those
        /// tags were written against can say which name that index means. Once the
        /// cache is dropped, `Runtime::FindMember` resolves through the **new** file
        /// — which no longer declares precisely the members this diff is looking for,
        /// so it returns NullEntity for every one. Resolving the doomed set by name
        /// rather than from here finds nothing, and the save destroys members without
        /// ever asking.
        std::vector<Assisi::ECS::Entity> previousMemberEntities;
    };

    /// @brief Brings every live copy of @p source up to date, across every resident
    /// world that is not simulating.
    ///
    /// Called by a successful save. Instances are matched by **closure**, not by
    /// source path: a parking lot's flattened member list contains the car's members,
    /// so editing the car changes the lot too.
    ///
    /// If the edit deletes members and that would cost undo history, this stops and
    /// asks (@ref _pendingReexpand) rather than doing it — see DrawSaveConfirmModal.
    /// The save is not finished until that is answered: Cancel puts the file back
    /// (@ref CancelPendingSave), so the write is not a fait accompli by the time the
    /// author is told what it costs.
    ///
    /// **The cache is dropped unconditionally**, before any of that: it is a statement
    /// about the file, not about the copies, and a save that skipped it would leave
    /// `GetBlueprintDefinition` handing out the contents from before the write. A save
    /// arriving while an earlier prompt is still up cannot ask a second question, so it
    /// takes the declining answer (@ref MarkInstancesStale) rather than returning
    /// quietly.
    ///
    /// @param collected must come from @ref CollectReexpandTargets, called **before**
    ///        the file was written. It cannot be gathered here: by this point the new
    ///        contents are on disk, and a cold cache would parse them as the old ones.
    void ReexpandInstancesOf(const std::string &source, std::vector<PendingReexpand> collected);

    /// @brief Which live instances a save of @p source would have to bring up to date,
    /// and what each of them is made of right now.
    ///
    /// **Call before writing the file.** Every field here describes the state the save
    /// is about to replace, and two of them — the member names and the entities behind
    /// them — exist only until it does. `GetBlueprintDefinition` falls back to parsing
    /// from disk when the cache is cold, so gathering after the write is correct only
    /// by luck of the cache being warm; a cancelled save is exactly the case that
    /// leaves it cold, and the retry then saw no change at all.
    [[nodiscard]] std::vector<PendingReexpand> CollectReexpandTargets(const std::string &source);

    /// @brief Records @p source as a file whose live copies are behind it, and says so.
    ///
    /// Idempotent. The single place @ref _staleInstanceSources is added to, so the two
    /// ways of declining a catch-up — the prompt's "Leave them" and a save that could
    /// not be asked about — leave the editor in the same state and log the same
    /// sentence.
    void MarkInstancesStale(const std::string &source);

    /// @brief Performs the work @ref ReexpandInstancesOf collected. Truncates the
    /// history of any world that lost a member, and rebuilds physics and assets for
    /// every member that survived or arrived.
    void ApplyPendingReexpand();

    /// @brief The "this save costs you N undo steps — save anyway?" gate. No-op when
    /// no save is waiting on an answer.
    void DrawSaveConfirmModal();

    /// @brief Puts back everything the un-answered save changed, and says so.
    ///
    /// The live copies need no undoing — @ref ApplyPendingReexpand is the only thing
    /// that destroys a member and it has not run — so this is the file, the world's
    /// level path, and the saved-state token that drives the dirty marker. The cache
    /// is dropped again afterwards, because it was dropped against contents that are
    /// no longer on disk.
    void CancelPendingSave();

    /// @brief A written file whose save has not been agreed to yet, and everything
    /// needed to put the world back exactly as it was.
    ///
    /// The write happens first and is undone on Cancel, rather than the cost being
    /// predicted beforehand: predicting it would mean re-deriving member names from
    /// the scene, a second copy of the rule `Runtime/Naming.hpp` owns, and the diff
    /// has to come from the file anyway — the file is what live copies re-expand from.
    struct PendingSaveConfirm
    {
        /// The virtual path written, which is also what the definition cache keys on.
        std::string virtualPath;
        /// Where it landed, resolved once at save time — Cancel must not depend on
        /// the asset roots still resolving the same way.
        std::filesystem::path resolved;
        /// The file's previous contents, or nullopt when the save created it (Save As
        /// to a new name) and Cancel therefore has to remove it again.
        std::optional<std::string> previousBytes;
        /// The world the save was for. Compared before anything is restored onto it:
        /// a handle to a world that is no longer the edited one must not have its
        /// level path rewritten from under whoever holds it now.
        Assisi::App::World *world = nullptr;
        /// What `_world->levelPath` was before the save retargeted it (Save As).
        std::string previousLevelPath;
        /// The dirty-marker token before the save cleared it, and which of the two
        /// it belongs to.
        std::uint64_t previousSavedToken = 0;
        bool savedTokenIsBlueprint = false;
    };
    std::optional<PendingSaveConfirm> _pendingSaveConfirm;

    // --- Moving an instance -------------------------------------------------
    //
    // An instance has no root entity to grab — the root evaporates at expansion —
    // so "the instance's transform" is a field on a table row, and moving it means
    // moving every member the placement reaches. Two callers do that: the gizmo
    // and the inspector. They share these three so a typed number and a dragged
    // handle produce the same edit, the same undo entry, and the same rounding.

    /// @brief Opens a placement gesture on @p instanceId against the viewed world
    /// and its history: snapshots the record and every member's pose, because the
    /// undo entry needs both and neither is reconstructible afterwards. Idempotent
    /// while the same gesture is open, so a site may call it every frame of a drag.
    ///
    /// A site that calls this must also call `_instanceGesture.Hold()` for as long
    /// as it still has the placement. It must *not* close the gesture itself — see
    /// SweepInstanceGesture.
    void BeginInstanceGesture(Assisi::ECS::InstanceId instanceId);

    /// @brief Moves @p instanceId to @p placement, carrying its members by the
    /// delta.
    ///
    /// **By delta, never by re-expansion**: re-expanding would destroy and recreate
    /// handles behind undo's back, and this gesture has to stay cheap. A non-uniform
    /// scale is averaged to one number here — the first half of "an instance may only
    /// scale uniformly", which keeps one from ever being authored; the load hard-fails
    /// on one.
    void ApplyInstancePlacement(Assisi::ECS::InstanceId instanceId, const Assisi::Runtime::Transform &placement);

    /// @brief Closes the open placement gesture into one transaction, carrying the
    /// record and every pose that actually moved, unless an edit site held it this
    /// frame. No-op if nothing moved — a click without a drag is not an edit.
    ///
    /// Called once from OnImGui after every panel has drawn, and from nowhere else.
    /// **Neither edit site may close the gesture on its own**: the gizmo draws first
    /// and cannot see that the Inspector is mid-scrub, which is how one dragged field
    /// once recorded a transaction per frame.
    void SweepInstanceGesture();

    /// @brief The Inspector, when what is selected is an *instance* rather than an
    /// entity — its identity and its placement, typed rather than only dragged.
    void DrawInstanceInspector();

    /// @brief Floor for a typed instance scale. Zero divides every member's scale
    /// away with no record of what it was; see ApplyInstancePlacement.
    static constexpr float kMinTypedInstanceScale = 1e-4f;

    // --- Finding and selecting an instance ----------------------------------

    /// @brief Draws a billboard at every live instance's placement, and outlines the
    /// selected one.
    ///
    /// An instance's root evaporates at expansion, so nothing in the scene marks
    /// where a copy was put — leaving the group's origin invisible and, worse,
    /// unclickable. A billboard gives it the same handle every placement-only entity
    /// already has.
    void SubmitInstanceIcons();

    /// @brief The instance whose root billboard @p mousePos is over, or 0.
    ///
    /// @param tOut distance along the pick ray, so the caller can decide between
    ///        this and an entity hit by which is actually in front.
    [[nodiscard]] Assisi::ECS::InstanceId PickInstance(glm::vec2 mousePos, float &tOut);

    // --- Creating instances and blueprints ----------------------------------

    /// @brief Places an instance of @p source in front of the camera, as one
    /// undoable transaction: the record and every member it created.
    ///
    /// The instance is *authored* — level content, written back when the level
    /// saves — which is what distinguishes it from a runtime SpawnBlueprint.
    void PlaceBlueprintInstance(const std::string &source);

    /// @brief Saves the selected entity and its subtree as a blueprint, then
    /// replaces them with an instance of it.
    ///
    /// A blueprint file is the same format a level is, so authoring one is saving
    /// a selection and instancing it back. One transaction, so a mistake is one
    /// Ctrl-Z: the entities come back and the instance goes away. The file stays
    /// on disk, as it does for any other save that is undone.
    void CreateBlueprintFromSelection(const std::string &name);

    /// @brief Rebuilds what expansion deliberately leaves out: resolved GPU asset
    /// pointers and Jolt bodies. Propagates first, because a parented member is
    /// placed from a parent matrix that does not exist until it has.
    void RebuildInstanceTransients(std::span<const Assisi::ECS::Entity> members);
    /// @brief The same, for a world that is not the one being shown — a blueprint
    /// save brings instances up to date wherever they are resident.
    void RebuildInstanceTransients(Assisi::App::World &world, std::span<const Assisi::ECS::Entity> members);

    // --- Instance overrides -------------------------------------------------

    /// @brief What @p entity's instance claims about @p component, or null if it
    /// claims nothing — the common case, and the one worth keeping cheap.
    ///
    /// A `null` claim means the instance removed the component. The returned
    /// pointer is into the instance table's row and lives until the next edit.
    [[nodiscard]] const nlohmann::json *OverrideClaimFor(Assisi::ECS::Entity entity,
                                                         const std::string &component) const;

    /// @brief Drops an override claim and lets the value fall back to the
    /// blueprint's.
    ///
    /// Pass a field name to drop one field, or an empty one to drop the whole
    /// component's claim. One transaction per gesture, carrying the record *and*
    /// the restored value — the same pairing an edit uses, for the same reason.
    void ResetOverride(Assisi::ECS::Entity entity, const std::string &component, const std::string &field);

    // --- Opening a level ---
    void LoadLevel(const std::string &name);

    // --- Selection ---------------------------------------------------------
    /// @brief Folds a click on @p entity into the selection under @p mode.
    ///
    /// Keeps `_selectedEntity` (the *active* entity — what the inspector shows
    /// and the gizmo drives) as the last entity clicked, and `_selectedInstance`
    /// in step with it. Range needs an order to walk, so it is resolved against
    /// the row order the entity list recorded while drawing; a Range with no
    /// order behaves as Replace.
    void SelectEntity(Assisi::ECS::Entity entity, SelectMode mode);
    /// @brief Empties the selection, both the active entity and the instance.
    void ClearSelection();
    /// @brief True if @p entity is anywhere in the selection, active or not.
    [[nodiscard]] bool IsSelected(Assisi::ECS::Entity entity) const;
    /// @brief Every selected entity, active one included, in click order.
    ///
    /// The single source of truth for "what is selected": `_selectedEntity` is
    /// its last element, kept as a separate member only because every existing
    /// caller reads it.
    [[nodiscard]] std::span<const Assisi::ECS::Entity> Selection() const { return _selection; }
    /// @brief True if any entity on @p entity's Parent chain is also selected.
    ///
    /// Such an entity must not be moved by the gizmo directly: its parent is
    /// already moving, and transform propagation carries it along — applying the
    /// drag to both would move it twice as far as the handle went.
    [[nodiscard]] bool HasSelectedAncestor(Assisi::ECS::Entity entity) const;
    /// @brief Drops dead entities from the selection.
    ///
    /// A delete elsewhere (undo, a play stop, a level load) can kill a selected
    /// entity without going through the selection at all, and a stale handle in
    /// the list would outline a slot something else now occupies.
    void PruneSelection();

    // --- Level paths: load and save ---
    /// @brief Drops everything the editor was holding *about* a scene that has
    /// just been replaced: play state, selection, armed pickers, undo history,
    /// an in-flight instance drag, and the pending save/re-expansion state.
    ///
    /// One function because it is one obligation. A load replaces the scene in
    /// place and the new one rebuilds entity identity densely from {0,0}, so
    /// every handle the editor kept now resolves to a live but *different*
    /// entity — passing IsAlive, failing to be what anyone meant. Spelled out at
    /// each site it used to be skippable by an early return, which is exactly
    /// what B20 is.
    ///
    /// @p virtualPath only names the level in the log line about an unanswered
    /// save; the work is the same either way.
    void ReleaseSceneBookkeeping(std::string_view virtualPath);
    /// @brief The above, plus emptying the world the failed load left behind.
    ///
    /// For a load that got past the point of no return and then refused. What it
    /// leaves is not a level — either an empty scene the deserializer gave up
    /// on, or the new level's entities with none of the systems that make them
    /// behave — and keeping the previous level's identity over it would make the
    /// next Save write one file's name onto another's contents. The editor
    /// already has an honest state for this, the one it starts in, so it goes
    /// there: no entities, no bodies, no systems, no level path.
    void AbandonReplacedScene(std::string_view virtualPath);
    /// @brief Loads a level by virtual path (e.g. "levels/Materials.alvl"), doing
    /// the cache-clear + rebind LoadLevel wraps. Returns false if the file didn't
    /// deserialize. The shared core of LoadLevel and the command-line loader.
    ///
    /// A false return means one of two things, and they are not interchangeable:
    /// either nothing was touched and the level on screen is the one that was
    /// there before, or the load got far enough to replace the scene and then
    /// failed — in which case this leaves no level open at all rather than the
    /// previous level's name and history over content that is not it (round-7
    /// B20, and ENG-126 for the systems half). Either way what is on screen and
    /// what the editor believes about it agree.
    bool LoadLevelFromPath(const std::string &virtualPath);
    /// @brief Saves the shown world to `levels/<name>.alvl`. Save As, and the
    /// Levels panel's shorthand for a level that lives where levels live.
    void SaveLevel(const std::string &name);
    /// @brief Saves the shown world to @p virtualPath verbatim, and records the
    /// history position that now matches disk.
    ///
    /// The general form, and the one a blueprint needs: a `.abp` lives under
    /// `blueprints/`, not `levels/`, and saving it through the name-shaped call
    /// would write a level file beside the wrong content. @return false if the path
    /// could not be resolved or the write failed.
    bool SaveLevelToPath(const std::string &virtualPath);

    // --- Play control (F5 run / F6 pause / F7 stop) ---
    /// @brief Enters play from the editing state: snapshots the scene so Stop can
    /// restore it, then begins simulating. No-op unless currently Editing.
    ///
    /// @p intent selects what the session does on the network. Host refuses
    /// (with a message) unless the level has been saved, and prompts when the
    /// scene has unsaved edits — clients load the last *saved* file, so hosting
    /// past that point means correcting bodies against geometry no client can
    /// see. Join enters Play immediately and builds its world when the host's
    /// handshake names one.
    void StartPlay(NetIntent intent = NetIntent::Standalone);
    /// @brief Resumes a paused simulation in place. No-op unless currently Paused.
    void ResumePlay();
    /// @brief Freezes simulation where it stands — physics, and any game-logic
    /// systems, stop ticking; nothing resets. No-op unless currently Playing.
    void PausePlay();
    /// @brief Stops simulating and restores the scene to the pre-play snapshot,
    /// discarding anything play mode changed. No-op when already Editing.
    void StopPlay();
    /// @brief True only while the world is actively simulating (physics + any game
    /// systems tick). False in both Editing and Paused; the editor camera/picking
    /// stay live regardless, so the scene is always navigable.
    [[nodiscard]] bool IsSimulating() const { return _playState == PlayState::Playing; }

    // --- Creating, deleting and composing entities ---
    /// @brief Creates a fresh entity with a Transform a few units in front of the
    /// editor camera (an empty object to build up via Add Component), selects it,
    /// and returns it. Used by the entity list's + button.
    Assisi::ECS::Entity CreateEntity();
    /// @brief Deletes @p entity and its whole subtree (descendants via Parent),
    /// tearing down each one's physics body, as one undoable transaction. Clears the
    /// selection if it fell inside the deleted subtree. Used by the Delete key and
    /// the entity list's delete button.
    void DeleteEntity(Assisi::ECS::Entity entity);
    /// @brief Deletes every selected entity and its subtree, as one transaction.
    /// Used by the Delete key and the entity list's delete button.
    void DeleteSelection();
    /// @brief Deletes the union of @p roots' subtrees as a single undoable
    /// transaction, skipping any root that is dead or not ours to edit.
    ///
    /// The union is deduplicated: two selected entities are often in one subtree,
    /// and capturing the shared part twice would make undo revive it twice.
    void DeleteEntities(std::span<const Assisi::ECS::Entity> roots);
    /// @brief Collects @p root plus every entity whose Parent chain leads to it
    /// (breadth-first over the Parent pool). Root-first order; used by DeleteEntity.
    std::vector<Assisi::ECS::Entity> GatherSubtree(Assisi::ECS::Entity root);
    /// @brief Adds @p meta's component to the selected entity with default field
    /// values, wiring up any runtime state the component needs (mesh re-resolve,
    /// physics body). No-op if the entity already has it. Used by the inspector's
    /// Add Component field.
    void AddComponentToSelected(const Assisi::Core::Reflect::ComponentMeta &meta);
    /// @brief Removes @p meta's component from the selected entity, cleaning up any
    /// associated runtime state (e.g. a RigidBodyDescriptor's Jolt body). Used by
    /// the inspector's per-component delete button.
    void RemoveComponentFromSelected(const Assisi::Core::Reflect::ComponentMeta &meta);

    // --- Camera framing ---
    /// @brief Starts the eased camera move (kCameraFocusDuration) that reframes
    /// @p entity, choosing a framing distance from its bounds. Used by an
    /// entity-list double-click.
    void FocusCameraOn(Assisi::ECS::Entity entity);

    // --- Asset reimport and staleness ---
    /// @brief Runs the editor-only reconcile pass: scans the asset root,
    /// generating a `.aast` sidecar (with a minted GUID) for any asset that
    /// lacks one and rebuilding the GUID→path database. Auto-run once at
    /// startup and re-run by the asset browser's Reimport button. References
    /// still resolve by path, so nothing rebinds — the database is built but is
    /// not yet the resolution key.
    void ReimportAssets();

    /// @brief Brings every indexed glTF's materials up to date. A glTF with no
    /// manifest is exploded into `.amat` children; one that already has a
    /// manifest is reconciled against its current source — auto-refreshing
    /// provably-safe changes (geometry-only, additive slots) and badging anything
    /// ambiguous stale (see _staleMeshes) without clobbering it. Editor-only;
    /// runs inside ReimportAssets between the two database scans. Returns whether
    /// any file was written, so the caller only rescans then.
    bool ReconcileMeshMaterials();

    /// @brief Whether a mesh asset (by virtual path) was left stale by the last
    /// reconcile — its source changed in a way that could not be auto-resolved.
    /// Drives the asset-browser stale badge.
    [[nodiscard]] bool IsAssetStale(std::string_view vpath) const;

    // --- Stale-material resolution prompt ---
    /// @brief Arms the resolution modal for a stale glTF: computes its per-slot
    /// diff and requests the popup open next frame. Reached by clicking a stale
    /// mesh tile, or auto-opened for a stale mesh live in the open scene.
    void OpenStaleResolution(const std::string &vpath);
    /// @brief Draws the modal that shows a stale mesh's material conflicts and the
    /// author's choices (regenerate from source / keep mine / later). Called from
    /// OnImGui once per frame.
    void DrawStaleResolutionModal();
    /// @brief Applies the author's choice to the current _staleResolveTarget:
    /// @p regenerate true overwrites the materials from source, false keeps them
    /// and just accepts the new source hash. Clears the stale flag and, on
    /// regenerate, rebuilds the database and re-resolves live entities.
    void ApplyStaleResolution(bool regenerate);
    /// @brief Advances the liveness queue: opens the next still-stale mesh, or
    /// clears the modal target when the queue is empty. Called after each choice.
    void AdvanceStaleQueue();

    // --- Picking ---
    // The ray, the volumes and the scene walk live in ScenePick.hpp; what stays
    // here is the part that needs the camera and the framebuffer.
    [[nodiscard]] PickRay BuildPickRay(glm::vec2 mousePos);

    Assisi::ECS::Entity PickEntity(glm::vec2 mousePos);
    /// @brief The same, reporting the hit distance so a caller can weigh it against
    /// a non-entity hit (an instance root's billboard) and take whichever is nearer.
    Assisi::ECS::Entity PickEntity(glm::vec2 mousePos, float &tOut);

    // --- Systems and input ---
    // _systems holds the *editor's* systems (picking, camera, selection): they run
    // in every play state and belong to the editor viewing a world, not to any
    // world. Game systems live in each WORLD's own registry — installed by the
    // default profile at world creation, run only while Playing — and are never
    // mixed in here.
    Assisi::App::SystemRegistry _systems;
    Assisi::Window::ActionMap _actions;

    // --- Game hooks ---
    EditorConfig _editorConfig;

    // Latches the once-per-process warning OnStart logs for a game's Render
    // systems, which the editor never runs.
    bool _warnedGameRenderSystems = false;

    // --- Worlds ---
    // Every resident level lives in the manager. The *active* world is the one
    // rendered and input-driven; the *edited* world is the one saved, dirtied and
    // undone into. They are usually the same, and diverge while a play session has
    // travelled elsewhere, or while a blueprint holds the edited role.
    // `_scene`/`_physics` are the active world's, cached so panels can reach them
    // directly.
    Assisi::App::WorldManager _worlds;
    Assisi::App::World *_world   = nullptr;                ///< The active world.
    Assisi::ECS::Scene *_scene   = nullptr;                ///< == &_world->scene.
    Assisi::Physics::PhysicsWorld *_physics = nullptr;     ///< == &_world->physics.

    // --- Networked play ---
    // `_netIntent` is the role this play session was entered for; `_joinPhase`
    // tracks a client through connect -> build -> live. Unguarded for the same
    // reason the enums are: Standalone is ordinary Play.
    NetIntent _netIntent = NetIntent::Standalone;
    JoinPhase _joinPhase = JoinPhase::None;

#if defined(ASSISI_NETWORKING)
    /// The networked session, when there is one. Created on Host/Join and
    /// destroyed on Disconnect (and before any level load), because it holds a
    /// reference to the scene it replicates and a level load replaces that
    /// scene wholesale.
    std::unique_ptr<Assisi::NetSync::NetSession> _netSession;
    /// UI state for the network panel, kept here rather than in statics so two
    /// editors in one process would not share it.
    std::array<char, 64> _netAddress{"127.0.0.1"};
    int32_t _netPort = 27015;
    /// Why the last Host/Join failed. Held here rather than read back off the
    /// session, because a failed attempt destroys the session that knows.
    std::string _netError;

    /// The content-set scan, kicked when a session starts. Until it lands, a host
    /// sends no ServerHello and a client sends no ClientHello, and the join
    /// timeout does not run — a first scan of a large asset tree is not a dead
    /// host, and timing out on it would read as one.
    Assisi::App::ContentSetHashJob _contentSetHash;
    /// Seconds spent waiting for a host's ServerHello. A join that never gets
    /// one would otherwise sit in Play forever with an empty world and no
    /// explanation.
    float _joinElapsed         = 0.f;
    static constexpr float kJoinTimeoutSeconds  = 10.f;
    /// Marshalled to OnUpdate: BuildJoinedWorld frees and re-resolves GPU
    /// assets, which must not happen from the fixed step mid-frame.
    bool _pendingJoinBuild = false;
    /// Marshalled likewise: a host that vanished, or a failed join, ends the
    /// play session at the frame's safe point rather than under the pump.
    bool _pendingStopPlay = false;
    /// The unsaved-edits host prompt: a modal, not a warning, because the
    /// consequence surfaces minutes later on someone else's screen.
    bool _hostPromptOpen  = false;
    bool _hostIgnoreDirty = false; ///< Set by "Host last-saved" for one attempt.
    // Correction-rate sampling for the Network panel. Rates rather than totals:
    // a total that keeps climbing only says the session is still running, which
    // is already visible. Sampled over a second so a frame-rate stutter does not
    // read as a bandwidth spike.
    float _netSampleSeconds         = 0.f;
    std::uint64_t _lastCorrectionBytes      = 0;
    std::uint64_t _lastCorrectionsApplied   = 0;
    float _correctionBytesPerSecond = 0.f;
    float _correctionsPerSecond     = 0.f;

    /// The mirrored world's structure revision this editor last resolved assets
    /// against. Mirrors arrive with authored asset ids and null GPU pointers;
    /// this is what tells the frame loop to look again.
    std::uint64_t _netStructureRevision = 0;
#endif // ASSISI_NETWORKING

    // --- Play session state ---
    /// The edited world as Run found it, minus its entities (those are
    /// `_playSnapshot` below, which needs exact-identity handling this does not).
    /// A join replaces the play scene with the *host's* level — its identity, its
    /// systems and its instance table — so all of it has to be put back.
    PrePlayState _prePlay;

#if defined(ASSISI_NETWORKING)
    /// The Play control's net mode, as an index into the dropdown. Sticky for
    /// the process and reset at launch: a per-session testing choice ("this time,
    /// host with two viewers"), not a preference worth persisting into
    /// options.json and being surprised by tomorrow.
    std::int32_t _playNetSelection = 0;
    /// How many play-in-editor clients the next Host launches. 0 = none, which
    /// is also what a plain "Host" (the cross-machine case) means.
    std::int32_t _pieClientCount = 0;
    /// The launched clients. Destroying one terminates it, so losing track of
    /// this vector is not a way to leak a window.
    std::vector<Assisi::App::ChildProcess> _pieClients;
    /// The temp level a PIE host wrote for its clients to load. Deleted at Stop.
    std::filesystem::path _pieTempLevel;
#endif // ASSISI_NETWORKING

    // --- Rendering ---
    // The engine's default scene-render path owns lighting + the mesh pipeline;
    // OnRender is a single Render() call.
    Assisi::Runtime::SceneRenderer _sceneRenderer;

    // Resolves each entity's mesh/materialOverrides ids to shared GPU resources;
    // owns every mesh, texture, and material the scene draws (deduped by path).
    Assisi::Render::AssetCache _assetCache;

    // Persistent arena + unit meshes for editor collider silhouette outlines (box,
    // sphere, cylinder; a capsule reuses the cylinder + sphere). Uploaded once at
    // init and never reset — unlike the asset cache's arena, which a level load
    // clears — so the MeshBuffers stay valid across level changes.
    Assisi::Render::GeometryArena _colliderArena;
    Assisi::Render::MeshBuffer _colliderBoxMesh;
    Assisi::Render::MeshBuffer _colliderSphereMesh;
    Assisi::Render::MeshBuffer _colliderCylinderMesh;

    // --- Asset database and staleness ---
    // Editor-only GUID identity index, populated by ReimportAssets() scanning the
    // asset root and generating `.aast` sidecars. Built but not yet the resolution
    // key — references still resolve by path.
    Assisi::Core::AssetDatabase _assetDatabase;

    // Mesh assets (by virtual path) the last reconcile left stale: their glTF
    // source changed in a way the conservative classifier couldn't auto-resolve.
    // Surfaced as a badge in the asset browser. Rebuilt each reconcile.
    std::unordered_set<std::string> _staleMeshes;

    // Stale-material resolution modal. The target is the glTF being resolved
    // ("" = closed); the diff is its per-slot conflict detail, computed once on
    // open. _staleResolveRequestOpen latches an ImGui::OpenPopup on the next
    // frame. The queue holds still-stale meshes that are live in the open scene,
    // prompted one after another.
    std::string _staleResolveTarget;
    Assisi::Geometry::MaterialDiff _staleResolveDiff;
    bool _staleResolveRequestOpen = false;
    std::vector<std::string>        _staleResolveQueue;

    // Smoke test for ImGui texture display — loaded once in SetupScene. Owns its
    // texture (not routed through _assetCache, which LoadLevel Clears).
    Assisi::Render::Texture _helloTexture;

    // --- Editor camera ---
    // The fly camera is not level data, so it is plain state here rather than an
    // entity: as members the pose survives a level load for free.
    // RefreshCameraMatrix() recomputes worldMatrix from the TRS (parentless, so
    // world == local) before it is read.
    Assisi::Runtime::Transform _cameraTransform;
    Assisi::Runtime::Camera _camera{60.f, 0.1f, 200.f, true};

    // Set by SetupCamera() before first use; these are just safe defaults.
    float _yaw   = 0.f;
    float _pitch = 0.f;

    static constexpr float kMoveSpeed        = 8.f;
    static constexpr float kMouseSensitivity = 0.1f;

    // --- Selection ---
    /// The *active* entity: the one the inspector shows and the gizmo drives.
    /// Always `_selection.back()`, or NullEntity when the selection is empty.
    /// Kept as its own member because nearly every panel reads it, and because
    /// "the one click acted on last" is a different question from "what is
    /// selected" the moment more than one thing is.
    Assisi::ECS::Entity _selectedEntity = Assisi::ECS::NullEntity;

    /// Everything selected, in click order, active entity last.
    std::vector<Assisi::ECS::Entity> _selection;

    /// Where a Shift-range starts: the last entity picked by a plain or
    /// Ctrl-click. Held separately from the selection because a range replaces
    /// what the previous range added without moving its own start — dragging the
    /// shift-click up and down a list has to grow and shrink one range, not
    /// stack a new one each time.
    Assisi::ECS::Entity _selectionAnchor = Assisi::ECS::NullEntity;

    /// The entity list's visible rows in draw order, rebuilt every frame. Range
    /// selection walks this; nothing else may rely on it, since it describes the
    /// list as it was last drawn rather than the scene.
    std::vector<Assisi::ECS::Entity> _entityRowOrder;

    /// A Shift-click waiting for the row order to finish being built. Resolved at
    /// the end of the list draw, when both ends of the range are known to be in
    /// `_entityRowOrder` — mid-draw, everything below the click is still missing.
    Assisi::ECS::Entity _pendingRangeTarget = Assisi::ECS::NullEntity;

    // --- Blueprint instances ---
    /// An override reset requested by the inspector, applied once its component
    /// loop has finished. **Must stay deferred**: the reset removes and re-adds
    /// the component, so doing it mid-loop leaves the loop reading a pointer into
    /// a pool that has since moved, and leaves the frame's open capture gesture
    /// straddling a value that changed underneath it.
    struct PendingOverrideReset
    {
        Assisi::ECS::Entity entity = Assisi::ECS::NullEntity;
        std::string component;
        std::string field;         ///< Empty resets the whole component's claim.
    };
    std::optional<PendingOverrideReset> _pendingOverrideReset;

    /// The blueprint instance the selection is *about*, or 0 for none.
    ///
    /// Selection has two modes. Clicking an instance's row selects the instance —
    /// the gizmo then moves the whole group and writes its placement, recording no
    /// member overrides, which is what keeps nudging a car from pinning all five of
    /// its members. Expanding the row and clicking a member selects that member,
    /// and this stays set so the inspector can say which instance it belongs to.
    ///
    /// So `_selectedInstance.IsValid() && _selectedEntity == NullEntity` is
    /// instance mode; both set is member mode.
    Assisi::ECS::InstanceId _selectedInstance;

    /// The gizmo's third frame, for a member of an instance: the blueprint root's
    /// axes, so the handles rotate with the car. A *view*, never a storage
    /// decision — the override is recorded in file space either way.
    bool _gizmoInstanceSpace = false;

    // An instance drag in progress. Snapshotted at the press edge rather than
    // captured through EditHistory's gesture machinery, which is keyed by
    // (entity, component) — this gesture moves several entities *and* a record,
    // so it has no single key. Its own type because two edit sites share it and
    // neither may decide on its own when it is over; see InstanceGesture.hpp.
    Assisi::Editor::InstanceGesture _instanceGesture;

    // --- Physics freeze while editing ---
    // Dragging a Transform field on a dynamic body would fight the solver, so the
    // body is made Static for the duration of the gesture and restored on release
    // (RequestPhysicsFreeze / ThawEditedBody).
    //
    // Shaped like _captureEditingActive: the request is a per-frame flag raised by
    // the panel, but the *release* is decided at the end of OnImGui, where it runs
    // whether or not the Inspector drew. Keying the release off the panel's own
    // edge skips it whenever the Inspector early-returns on an empty selection,
    // stranding the body Static while its descriptor still says dynamic.
    bool _physicsFreezeRequested = false;
    Assisi::ECS::Entity _frozenBodyEntity       = Assisi::ECS::NullEntity;
    /// World that owns _frozenBodyEntity, by name rather than pointer: the viewed
    /// world can change (or be destroyed) between freeze and release, and the
    /// thaw must reach the body it actually froze, not whatever is on screen now.
    std::string _frozenBodyWorld;

    // --- Collider wireframe scratch ---
    // Reused per frame by SubmitColliderWireframes: the depth-tested (unselected)
    // and on-top (selected) line batches, and the collider entities whose editor
    // billboards are suppressed. Members so drawing colliders doesn't allocate
    // every frame.
    std::vector<Assisi::Render::LineVertex> _colliderLinesDepthTested;
    std::vector<Assisi::Render::LineVertex> _colliderLinesOnTop;
    std::vector<Assisi::ECS::Entity>        _colliderEntities;

    // --- Entity list ---
    // Requests the Entities list scroll to this entity's row next time it draws
    // (set when a new entity is created, so it comes into view). NullEntity = none.
    Assisi::ECS::Entity _scrollToEntity = Assisi::ECS::NullEntity;

    // --- Transform gizmo ---
    GizmoOp _gizmoOp        = GizmoOp::Translate;
    bool _gizmoLocalSpace = false;    // false = world axes
    // The drag in progress: which entities it grabbed and whether it is still going,
    // so its release edge can be read from outside DrawTransformGizmoHandles and its
    // early returns. The gizmo force-commits its (shared) Transform gestures on that
    // edge, which is what keeps a drag its own undo entry rather than merged with a
    // later edit. See GizmoDrag.hpp.
    Assisi::Editor::GizmoDrag _gizmoDrag;

    // --- Undo/redo (editor-only) ---
    // Emplaced in OnStart once _scene exists. Captures scene edits (record-before-
    // write) and applies undo/redo in the Editing state. See EditHistory.hpp.
    // std::optional because it binds a Scene& not available until the Main scene is
    // created; it persists across play sessions (Stop restores exact identity, so
    // its entity handles stay valid — see StopPlay).
    std::optional<Assisi::Editor::EditHistory> _history;
    // A throwaway history active only while Paused: edits made during a pause are
    // undoable there, but the whole container is discarded when play resumes or
    // stops, so paused undo never leaks into the persistent editing history.
    std::optional<Assisi::Editor::EditHistory> _pausedHistory;
    // The open blueprint world's own history, live only while blueprint mode is.
    // Separate from _history rather than swapped with it: the level's undo stack
    // has to come back untouched, and EditHistory binds a Scene by reference.
    std::optional<Assisi::Editor::EditHistory> _blueprintHistory;
    // Accumulated across a frame's ImGui panels: true if an edit widget (inspector
    // drag/type, or the gizmo) is still being manipulated. The end-of-OnImGui sweep
    // reads it to decide whether an open capture gesture has ended. Reset each frame.
    bool _captureEditingActive = false;
    // The main history's state token at the last successful SaveLevel (0 = base /
    // freshly loaded). IsSceneDirty() compares the live token against it.
    std::uint64_t _savedStateToken = 0;

    // --- Blueprint editing mode ---
    // The world holding the blueprint being edited, or null when none is open. It
    // holds the *edited* role while it lives, which is what makes the panels let you
    // touch it; the level it was opened from stays resident and inspect-only.
    Assisi::App::World *_blueprintWorld = nullptr;
    // Which world to show again on close. By name, not by pointer: closing is a
    // frame or many after opening and the world could have gone in between.
    std::string _blueprintReturnWorld;
    // _blueprintHistory's token at the blueprint's last successful save.
    std::uint64_t _blueprintSavedToken = 0;
    // The lighting the blueprint editor works by. The sun is an entity (so the
    // gizmo and inspector reach it like anything else); ambient is a renderer knob,
    // since there is no such component and nothing about it belongs in a file.
    glm::vec3 _blueprintAmbientColor{1.f, 1.f, 1.f};
    float _blueprintAmbient = 0.25f;
    // Deferred, for the same reason level loads are: opening resolves assets and
    // touches GPU state, and a panel runs mid-frame.
    std::optional<std::string> _pendingBlueprintOpen;
    // Set by the panel's Close button, applied at the same safe point — closing
    // destroys a world, which must never happen from inside a panel.
    bool _pendingBlueprintClose = false;

    // --- Blueprint save: pending re-expansion ---
    // A collected re-expansion waiting on the author's answer, plus what it costs.
    // Empty the rest of the time; the modal is only raised when history is at stake.
    std::vector<PendingReexpand> _pendingReexpand;
    std::string _pendingReexpandSource;
    std::size_t _pendingReexpandUndoLoss = 0;
    // The member names the edit removes, for the prompt. Names rather than entities:
    // "lid, hinge" is what the author recognises, and one name may cover four copies.
    std::vector<std::string> _pendingReexpandRemoved;
    // Blueprints whose live copies the author chose to leave stale. **Hosting is
    // refused while this is non-empty**: a client expands the file, so the two
    // machines would build different member sets while the content-set hash agreed
    // they match — it hashes the disk, and the disks *do* match. Cleared by a level
    // load, and by a catch-up that is accepted.
    std::vector<std::string> _staleInstanceSources;

    // --- Window title, and requests deferred out of ImGui ---
    // Last dirty state pushed to the OS window title, so the title is only re-set
    // when it actually flips, not every frame.
    bool _titleDirtyShown = false;
    // A history jump requested by clicking a History-panel row: negative = undo N
    // steps, positive = redo N. Applied at the top of the next OnUpdate — never
    // mid-ImGui, which would invalidate cached component pointers.
    int32_t _pendingHistorySteps = 0;

    // --- ImGui wedge watchdog ---
    // Seconds a widget has held ActiveId with no mouse button down and no text
    // edit, and the next elapsed time at which to (re-)log it. Diagnostic for the
    // reported "UI stops responding until a new window opens" freeze.
    static constexpr float kImGuiWedgeThreshold = 3.f;
    float _imguiWedgeSeconds    = 0.f;
    float _imguiWedgeNextReport = kImGuiWedgeThreshold;

    // --- Inspector: component delete confirmation ---
    // The inspector's X button arms a two-step confirm for one component at a
    // time. Scoped to an entity so switching selection cancels a pending confirm
    // rather than deleting from the new one.
    Assisi::Core::Reflect::ComponentId _pendingDeleteComponent =
        Assisi::Core::Reflect::kInvalidComponentId;
    Assisi::ECS::Entity _pendingDeleteEntity = Assisi::ECS::NullEntity;

    // --- Overlays and debug panels ---
    /// The F11 options overlay. Held by pointer so its telemetry buffers and the
    /// GpuTelemetry header stay out of this one.
    std::unique_ptr<EditorOptionsPanel> _options;
    bool _showChiara  = false; ///< F9 performance-capture panel.

    // F11 "Editor overlays" checkbox: per-frame visibility of the selection
    // outline, entity icons, and collider wireframes, for decluttering the view
    // or a screenshot. Purely editor-side (it skips the submissions); whether the
    // passes exist at all is EditorConfig::enableEditorVisuals.
    bool _showEditorOverlays = true;

    // --- Play control (game-control window, F5/F6/F7) ---
    // Physics and any game-logic systems tick only while Playing; the editor
    // camera and picking stay live in every state so the scene is always
    // navigable. Run snapshots the scene so Stop can restore it, Pause freezes
    // in place, Stop restores the snapshot and returns to Editing.
    enum class PlayState : std::uint8_t
    {
        Editing,
        Playing,
        Paused
    };
    PlayState _playState = PlayState::Editing;

    /// @brief The one place the play state changes.
    ///
    /// The state itself is the flat freeze: while it is not Playing the fixed loop
    /// steps nothing, in any world. This also sets the viewed world's `simulate`
    /// flag, which selects among the worlds of a *running* session — but **only
    /// when that world is Active**, since a Dormant world is by definition not
    /// stepped: resuming while inspecting the dormant edited world would otherwise
    /// hand it a live flag.
    void SetPlayState(PlayState state)
    {
        _playState = state;
        if (_world != nullptr && _world->state == Assisi::App::WorldState::Active)
        {
            _world->simulate = (state == PlayState::Playing);
        }
    }

    // One entity's exact-identity snapshot for the play/stop restore. Unlike a
    // Save(), which renumbers entities to dense serial indices, this keeps the
    // exact (index, generation) handle so Stop can restore entities *in place* via
    // Scene::ReviveAt — which is what lets the editing undo history survive a play
    // session, its stored handles still resolving after Stop. Components are the
    // reflected JSON of each serializable component, captured under a raw-entity
    // context (EntityRef fields as raw handles), same as the undo capture path.
    struct PlayEntitySnapshot
    {
        Assisi::ECS::Entity handle;
        std::vector<Assisi::Editor::ComponentSnapshot> components;
    };
    std::vector<PlayEntitySnapshot> _playSnapshot; ///< Captured at Run; restored on Stop.

    // --- Camera focus animation (entity-list double-click) ---
    // An eased move that reframes the camera on an object. While active it owns the
    // camera transform: UpdateCamera advances it and skips fly control. Manual look
    // input cancels it. Always kCameraFocusDuration, whatever the travel distance.
    bool _cameraFocusActive  = false;
    float _cameraFocusElapsed = 0.f;
    glm::vec3 _cameraFocusStartPos{0.f};
    glm::vec3 _cameraFocusEndPos{0.f};
    glm::quat _cameraFocusStartRot{1.f, 0.f, 0.f, 0.f};
    glm::quat _cameraFocusEndRot{1.f, 0.f, 0.f, 0.f};
    static constexpr float kCameraFocusDuration = 0.25f;

    // --- Inspector: Add Component ---
    // The in-progress substring being typed, matched case-insensitively against
    // addable component names.
    char _addComponentBuf[64] = {};
    // Keyboard highlight into the suggestion list: Tab/Down advance it, Up retreats,
    // editing the text resets it to the first row, Enter adds the highlighted one.
    int32_t _addComponentSelected = 0;

    // --- Eyedropper ---
    // While armed, the next scene entity-pick is written into the captured
    // EntityRef field instead of changing the selection. The target is pinned by
    // (entity, component meta, field offset) rather than a raw pointer, so a pool
    // reallocation between arming and picking can't dangle it.
    bool _eyedropperArmed       = false;
    Assisi::ECS::Entity _eyedropperEntity      = Assisi::ECS::NullEntity;
    const Assisi::Core::Reflect::ComponentMeta *_eyedropperMeta        = nullptr;
    std::size_t _eyedropperFieldOffset = 0;

    // --- Asset browser ---
    // Opened from an AssetPath field's browse button, it navigates the asset
    // directory and writes the picked path back into the field. The target is
    // pinned by (entity, component meta, field offset) and re-resolved at write
    // time — same anti-dangling scheme as the eyedropper above.
    bool _assetBrowserOpen        = false;
    Assisi::ECS::Entity _assetBrowserEntity      = Assisi::ECS::NullEntity;
    const Assisi::Core::Reflect::ComponentMeta *_assetBrowserMeta        = nullptr;
    std::size_t _assetBrowserFieldOffset = 0;
    /// @brief -1 when the target field is a plain AssetPath; >= 0 when it is
    /// element `[slot]` of an AssetPathVector (a MeshRenderer material slot). In
    /// the latter mode the browser lists only materials (and folders).
    int32_t _assetBrowserVectorSlot  = -1;
    std::string _assetBrowserDir;                                 ///< Current dir, relative to the asset root ("" = root).

    // Cached listing of _assetBrowserDir — re-read only on navigation / open /
    // Refresh (see _assetBrowserDirty), never per frame.
    std::vector<std::string> _assetBrowserDirs;
    std::vector<std::string> _assetBrowserImages;
    std::vector<std::string> _assetBrowserMeshes;    ///< .glb/.gltf files (no thumbnail; shown as cube tiles).
    std::vector<std::string> _assetBrowserMaterials; ///< .amat files (shown as material-sphere tiles).
    bool _assetBrowserDirty     = true;
    bool _assetBrowserReadError = false;
    float _assetBrowserThumbSize = 256.f;                    ///< Tile size in px; adjustable via the zoom buttons.

    // Textures loaded to thumbnail the browser's image entries. Separate from
    // _assetCache so a level load (which Clears that) doesn't drop thumbnails.
    Assisi::Render::AssetCache _thumbnailCache;

    // --- Levels panel ---
    std::vector<std::string> _levelFiles;
    int32_t _selectedLevel = 0;
    char _saveAsName[128] = {};

    // --- Blueprints panel ---
    /// Every `.abp` and `.alvl` under the asset root, as virtual paths — what the
    /// Blueprints panel offers to place. Both extensions, because they are one
    /// format and the extension never gates behaviour: instancing a level into a
    /// level is legal, and the editor should not pretend otherwise.
    std::vector<std::string> _blueprintFiles;
    int32_t _selectedBlueprint = 0;
    char _newBlueprintName[128] = {};

    // --- World and level operations deferred out of ImGui ---
    // **None of these may run mid-frame.** A level load requested from a panel,
    // applied at the next OnUpdate: LoadLevel frees GPU assets (including the
    // bindless table) that this frame's already-recorded draws still reference,
    // which faults the GPU.
    std::optional<std::string> _pendingLevelLoad;

    // The Game panel's "Load as new world" debug control: it creates a second
    // resident world, which resolves assets and builds Jolt bodies — main-thread-
    // drain work.
    std::optional<std::string> _pendingWorldLoad;
    // ...and "Travel here", which additionally frees the outgoing world's GPU
    // assets — the strongest reason of the three.
    std::optional<std::string> _pendingTravel;
    // "Migrate selection": migration rebuilds physics bodies and resolves meshes,
    // so it runs at the pre-update safe point.
    std::optional<std::string> _pendingMigrate;
    // Async travel: "Preload" starts a background load, "Swap" promotes it.
    // BeginLoadLevel mutates the world store and promotion resolves/frees GPU
    // assets.
    std::optional<std::string> _pendingPreload;
    bool _pendingPromote = false;
};

} // namespace Assisi::Editor
