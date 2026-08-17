/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file EditHistory.hpp
/// @brief The editor's Ctrl-Z: a linear stack of reversible scene edits.
///
/// An edit is captured as a *delta* — the reflected JSON of the affected
/// component, entity or instance row before and after — never a whole-scene
/// snapshot and never a bespoke command object. Deltas are a tagged union
/// grouped into a Transaction: one user gesture, one Ctrl-Z.
///
/// Editor-only, never linked into a shipped game. It rests on two neutral
/// runtime primitives added for it: Scene::ReviveAt (exact-identity
/// resurrection) and SceneSerializer::ScopedRawEntityContext (raw-handle
/// EntityRef serialization).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include <Assisi/Core/AssetPath.hpp>
#include <Assisi/Core/Reflect/ComponentId.hpp>
#include <Assisi/ECS/Entity.hpp>
#include <Assisi/Runtime/Blueprint.hpp>

namespace Assisi::ECS
{
struct Scene;
}

namespace Assisi::Editor
{

/// @brief One component's reversible change on a live entity.
///
/// nullopt on a side means the component was absent there, so value edits (both
/// sides present) and add/remove (one side absent) share one shape.
struct ComponentDelta
{
    Assisi::ECS::Entity entity;            ///< Exact (index, generation) while alive.
    Assisi::Core::Reflect::ComponentId id;
    std::optional<nlohmann::json>  before; ///< nullopt = component absent before.
    std::optional<nlohmann::json>  after;  ///< nullopt = component absent after.
};

/// @brief One component's serialized state within an entity-lifetime snapshot.
///
/// A create/delete records the entity's *whole* component set as a list of
/// these. Per component rather than one opaque blob, which is what makes the
/// two-phase apply — revive the entity, then add its components — natural.
struct ComponentSnapshot
{
    Assisi::Core::Reflect::ComponentId id;
    nlohmann::json data;
};

/// @brief A whole-entity lifetime change (create or delete), possibly a subtree.
///
/// nullopt on a side means the entity did not exist there: create = {nullopt,
/// components}, delete = the reverse. A subtree delete is several EntityDeltas
/// in one Transaction.
struct EntityDelta
{
    Assisi::ECS::Entity handle;                              ///< Exact (index, generation).
    std::optional<std::vector<ComponentSnapshot>>    before;
    std::optional<std::vector<ComponentSnapshot>>    after;
};

/// @brief One blueprint instance record's reversible change.
///
/// The record — source path, placement, overrides, removal list — is not scene
/// data, so neither delta above can express it. Without it undo leaves a **fake
/// override**: the component reverts, the note saying "this instance changed
/// that field" does not.
///
/// So an inspector edit on a member is *one* transaction carrying both a
/// ComponentDelta and this; placing an instance is this plus its EntityDeltas.
/// All of it reverts together.
struct InstanceDelta
{
    Assisi::ECS::InstanceId instanceId;

    /// The row on each side, or nullopt for "the instance did not exist".
    /// Place = {nullopt, row}; delete = the reverse; an override edit has both,
    /// differing only in `overrides`.
    std::optional<Assisi::Runtime::BlueprintInstance> before;
    std::optional<Assisi::Runtime::BlueprintInstance> after;
};

/// @brief One reflected **asset** file's reversible change — a `.amat` edited in
/// the Material panel.
///
/// The only edit kind that is not scene data: an asset belongs to no entity, so
/// none of the deltas above can express it and replay cannot go through the
/// scene. It travels as its reflected JSON on each side, and is applied through
/// the hook the editor installs (see EditHistory::SetAssetApplyHook) — this
/// class knows what an asset edit *is*, never what a material *means*.
///
/// Undo restores the live value, not the file. Material edits are live before
/// they are saved, so reverting one leaves the panel dirty or clean exactly as
/// the value warrants; writing the file back would turn an undo into a save.
struct AssetDelta
{
    Assisi::Core::AssetPath path;   ///< The asset file this is about.
    std::string typeName;           ///< AssetTypeMeta::name — which codec replays it.
    nlohmann::json before;
    nlohmann::json after;
};

/// @brief The tagged union of edit kinds.
using EditCommand = std::variant<ComponentDelta, EntityDelta, InstanceDelta, AssetDelta>;

/// @brief One user gesture (a gizmo drag, a slider drag, one add-component) —
/// the atom of undo. Applying it toward `before` is undo; toward `after` is redo.
struct Transaction
{
    std::string label;              ///< "Move", "Add MeshRenderer", … (for the history panel).
    std::vector<EditCommand> cmds;
    Assisi::ECS::Entity selectionBefore = Assisi::ECS::NullEntity;
    Assisi::ECS::Entity selectionAfter  = Assisi::ECS::NullEntity;
    std::uint64_t seq             = 0;            ///< Unique sequence, assigned on Push (dirty tracking).
};

/// @brief A linear undo/redo stack over one Scene.
///
/// Push a completed Transaction; Undo()/Redo() replay it against the scene and
/// return the selection to restore. All apply happens under a raw-entity
/// serialization scope, so EntityRef fields (Parent, and any other reference)
/// restore to exact handles rather than silently flattening.
///
/// It is also why a deleted entity returns through Scene::ReviveAt at its
/// original (index, generation) rather than being recreated: every handle held
/// elsewhere — this stack, other components, the panels — stays valid, with
/// nothing to patch up. ReviveAt is sound only for a free slot under a strictly
/// linear history, which is what Push() clearing the redo stack guarantees.
class EditHistory
{
public:
    /// @brief Called once per component right after an apply restores or removes
    /// it, so the editor can rebuild the transient state serialization excludes
    /// (physics body, resolved asset pointers). This class stays agnostic about
    /// which ids need what; the hook dispatches. Its arguments are the affected
    /// entity (alive unless it is being destroyed), the component's id, and
    /// whether the component now exists.
    using RebindHook = std::function<void (Assisi::ECS::Entity entity,
                                           Assisi::Core::Reflect::ComponentId id, bool present)>;

    /// @brief Called to put a replayed AssetDelta's state back into whatever
    /// holds that asset live — the open editor panel, the renderer's cache.
    ///
    /// A hook for the same reason RebindHook is one: this class replays scene
    /// data itself because it owns the scene, but it owns no asset and has no
    /// business knowing what a `.amat` is. Arguments are the asset's reflected
    /// type name, its path, and the state to adopt.
    using AssetApplyHook =
        std::function<void (std::string_view typeName, const Assisi::Core::AssetPath &path,
                            const nlohmann::json &state)>;

    /// @brief Install the asset replay hook. Without one, an AssetDelta replays
    /// as a no-op rather than an error: a history holding asset edits is still a
    /// valid history, it simply has nowhere to put them (a test with no panel).
    void SetAssetApplyHook(AssetApplyHook hook) { _assetApply = std::move(hook); }

    /// @param scene     The scene edits apply to. Must outlive this history.
    /// @param rebind    Transient-rebuild dispatch (may be empty — then a no-op).
    /// @param instances The world's blueprint instance table, or null in a host
    ///                  with no instances (tests). Without it a member edit still
    ///                  reverts its component but records no override, so the
    ///                  level silently loses that edit on save — pass it wherever
    ///                  a level can be saved.
    EditHistory(Assisi::ECS::Scene &scene, RebindHook rebind = {},
                Assisi::Runtime::InstanceTable *instances = nullptr);

    /// @brief Records a completed transaction, clears the redo stack, and drops
    /// the oldest transaction once past kMaxDepth. No-op for an empty command
    /// list (callers drop no-op gestures upstream; this is the safety net).
    void Push(Transaction txn);

    // --- Capture: record before write --------------------------------------
    //
    // ImGui writes the new value *inside* the widget call, so the pre-edit value
    // can only be had by snapshotting before drawing. Every edit site opens a
    // gesture (RecordBefore) before writing; it closes at once (CommitGesture) for
    // an instant edit, or in the end-of-frame sweep for a multi-frame drag or
    // typing. A gesture whose before == after is dropped, which absorbs
    // click-without-drag and Escape-revert for free.

    /// @brief Idempotently opens a capture gesture for (entity, id), snapshotting
    /// the component's current reflected JSON as `before` (nullopt if absent).
    /// Call once per edit site *before* the write. A second call on an open
    /// gesture only refreshes its liveness and label, keeping the original
    /// `before`. No-op while applying. @p selection is restored with the commit.
    void RecordBefore(Assisi::ECS::Entity entity, Assisi::Core::Reflect::ComponentId id, std::string label,
                      Assisi::ECS::Entity selection);

    /// @brief Closes the gesture for (entity, id) now: serializes `after` and
    /// pushes a transaction unless before == after. For instant edits (asset
    /// pick, eyedropper, add/remove component) that finish within one call.
    /// No-op if no gesture is open for the key, or while applying.
    void CommitGesture(Assisi::ECS::Entity entity, Assisi::Core::Reflect::ComponentId id);

    /// @brief End-of-frame sweep for drag/type gestures. Commits any open gesture
    /// whose widget is no longer being manipulated (or whose component block is no
    /// longer drawn), drops no-ops, and abandons gestures whose entity has died.
    /// @p editingActive is true while an edit widget (inspector drag/type or the
    /// gizmo) is still held this frame — those gestures stay open to coalesce.
    void EndFrameSweep(bool editingActive);

    [[nodiscard]] bool CanUndo() const { return !_undo.empty(); }
    [[nodiscard]] bool CanRedo() const { return !_redo.empty(); }

    /// @brief Reverts the most recent transaction. Returns the selection to
    /// restore (its `selectionBefore`), or nullopt if there was nothing to undo.
    std::optional<Assisi::ECS::Entity> Undo();

    /// @brief Re-applies the most recently undone transaction. Returns the
    /// selection to restore (its `selectionAfter`), or nullopt if nothing to redo.
    std::optional<Assisi::ECS::Entity> Redo();

    /// @brief Empties both stacks and abandons every open gesture.
    ///
    /// Call whenever entity identity is rebuilt densely — a level load,
    /// Scene::Clear: every stored handle then dangles or, worse, aliases an
    /// unrelated live entity that passes IsAlive.
    ///
    /// **Not** on Stop-play: the restore revives entities at their pre-play
    /// handles, so the editing history stays replayable and survives the session.
    /// Edits made while paused belong to a separate scratch history, discarded on
    /// resume or stop.
    void Clear();

    /// @brief Drops the undo steps that can no longer be replayed because the
    /// entities in @p destroyed are gone, and returns how many went.
    ///
    /// Saving a blueprint that dropped a member destroys it in every live copy. A
    /// transaction naming one cannot be applied: the handle is dead, and ReviveAt
    /// needs a free slot, which the next entity created will take.
    ///
    /// **A suffix of the replay order, not a filter.** Undo replays newest first,
    /// so once a step is unreplayable nothing older is reachable either: the newest
    /// offender and everything older than it go. Dropping only the offenders leaves
    /// steps whose `before` was never restored, each applying against the wrong
    /// world, quietly. The redo stack goes too — a redo naming a destroyed entity
    /// would recreate it into a slot the world has moved on from.
    ///
    /// Handles from a scene other than @p scene are refused wholesale: a handle is
    /// (slot, generation) with **no scene identity in it** and every Scene numbers
    /// from {0,0}, so a doomed handle in one world compares equal to a live,
    /// unrelated entity in another. The one caller sweeps every resident world at
    /// once (a blueprint save re-expands every live copy), so without this a
    /// level's dead members would truncate the blueprint world's history.
    std::size_t ForgetEntities(const Assisi::ECS::Scene &scene,
                               std::span<const Assisi::ECS::Entity> destroyed);

    /// @brief What ForgetEntities would drop, without dropping it, so a save can
    /// say how much history is at stake first. Same cross-scene rule: @p scene
    /// must be the one @p destroyed came from.
    [[nodiscard]] std::size_t CountForgettable(const Assisi::ECS::Scene &scene,
                                               std::span<const Assisi::ECS::Entity> destroyed) const;

    /// @brief True while an Undo()/Redo() is applying. The capture layer checks
    /// it so an apply's own writes are not recorded as fresh edits.
    [[nodiscard]] bool IsApplying() const { return _applying; }

    [[nodiscard]] std::size_t UndoDepth() const { return _undo.size(); }
    [[nodiscard]] std::size_t RedoDepth() const { return _redo.size(); }

    /// @brief Label the next Undo()/Redo() would apply, or "" if that stack is
    /// empty. For the Edit menu and the history panel.
    [[nodiscard]] const std::string &NextUndoLabel() const;
    [[nodiscard]] const std::string &NextRedoLabel() const;

    /// @brief Undo-stack labels oldest→newest: the last entry is what the next
    /// Undo() reverts. For the history panel.
    [[nodiscard]] std::vector<std::string> UndoLabels() const;
    /// @brief Redo-stack labels next-to-redo→oldest-redone: index 0 is what the
    /// next Redo() re-applies.
    [[nodiscard]] std::vector<std::string> RedoLabels() const;

    /// @brief An opaque token for the current position in history. Stable across
    /// an undo→redo round-trip, distinct after any new edit; 0 is the base state.
    /// Save it at SaveLevel and compare to detect unsaved changes (the `*` marker).
    [[nodiscard]] std::uint64_t CurrentStateToken() const
    {
        return _undo.empty() ? 0 : _undo.back().seq;
    }

    /// @brief Snapshot every serializable component of @p entity to JSON under a
    /// raw-entity context — the payload of a create/delete EntityDelta. Empty for
    /// a bare entity. Public so the create/delete edit sites can build one.
    [[nodiscard]] std::vector<ComponentSnapshot> CaptureEntityComponents(Assisi::ECS::Entity entity) const;

    /// @brief Snapshot one component to JSON under a raw-entity context, or nullopt
    /// if absent. Public for edit sites that build a transaction by hand rather
    /// than through the gesture machinery — an instance drag moves several entities
    /// and a record in one gesture, so it has no single (entity, id) key.
    [[nodiscard]] std::optional<nlohmann::json> CaptureComponent(Assisi::ECS::Entity entity,
                                                                 Assisi::Core::Reflect::ComponentId id) const
    {
        return SnapshotComponent(entity, id);
    }

    /// @brief Cap on retained transactions; the oldest goes past this. JSON
    /// payloads are heavy, so history stays bounded whatever the edit count.
    static constexpr std::size_t kMaxDepth = 256;

private:
    enum class Direction : std::uint8_t
    {
        Undo,
        Redo
    };

    /// @brief One in-progress capture gesture: a component whose `before` is
    /// snapshotted and whose commit is still pending.
    struct OpenGesture
    {
        Assisi::ECS::Entity entity;
        Assisi::Core::Reflect::ComponentId id;
        std::string label;
        std::optional<nlohmann::json>      before;
        Assisi::ECS::Entity selection;
        bool touchedThisFrame = true;
    };

    OpenGesture *FindOpen(Assisi::ECS::Entity entity, Assisi::Core::Reflect::ComponentId id);

    /// @brief Serialize a live component to JSON under a raw-entity scope, so its
    /// EntityRef fields capture as raw handles; nullopt if the component is absent.
    [[nodiscard]] std::optional<nlohmann::json> SnapshotComponent(Assisi::ECS::Entity entity,
                                                                  Assisi::Core::Reflect::ComponentId id) const;

    /// @brief Turn a resolved gesture into a transaction if before != after.
    /// Returns true if a transaction was pushed.
    bool CommitOpenGesture(const OpenGesture &gesture);

    /// @brief If @p entity is a blueprint member, folds the gesture's own change
    /// into its instance's override record and returns the record's before/after.
    ///
    /// **The per-field override is derived by diffing this gesture's before and
    /// after**, never by comparing the live scene against the blueprint. That
    /// comparison spans edits, so a blueprint edit would freeze the old values into
    /// every instance as fake overrides.
    ///
    /// Returns nullopt when there is nothing to record: no table, not a member, or
    /// an instance the table no longer knows.
    std::optional<InstanceDelta> RecordOverride(Assisi::ECS::Entity entity,
                                                Assisi::Core::Reflect::ComponentId id,
                                                const std::optional<nlohmann::json> &before,
                                                const std::optional<nlohmann::json> &after);

    /// @brief A captured component's JSON with its EntityRef fields rewritten from
    /// raw handles into the names a file addresses by.
    ///
    /// Captures pack references as (slot, generation): right for replaying an undo,
    /// wrong for a file, where they would name nothing on the next load. A
    /// reference this cannot name — no Name, or a member of an instance the table
    /// has forgotten — is dropped from the claim with a warning rather than written
    /// wrong: an override that means something else is worse than a missing one.
    [[nodiscard]] nlohmann::json ReferenceSafeOverride(const nlohmann::json &component,
                                                       const Assisi::Core::Reflect::ComponentMeta &meta,
                                                       Assisi::ECS::InstanceId instanceId) const;

    /// @brief How a file that overrides a member of @p instanceId should name
    /// @p target: instance-relative for a member of the same instance, `/…` for
    /// anything else the writing file can see. Nullopt if it cannot be named.
    [[nodiscard]] std::optional<std::string> NameForOverrideTarget(Assisi::ECS::Entity target,
                                                                   Assisi::ECS::InstanceId instanceId) const;

    void ApplyTransaction(const Transaction &txn, Direction dir);

    /// @brief Brings component `id` on `entity` to `target`: nullopt removes it,
    /// a value restores it (remove-first-then-add, since Scene::Add rejects an
    /// existing component). Invokes the rebind hook after.
    void RestoreComponent(Assisi::ECS::Entity entity, Assisi::Core::Reflect::ComponentId id,
                          const std::optional<nlohmann::json> &target);

    /// @brief Adds (remove-first-then-add) component `id` from `data` **without**
    /// firing the rebind hook. True if it was added — a serializable component
    /// with an addToScene hook — false if skipped.
    ///
    /// Lets a whole entity's component set be restored before any rebind runs, so
    /// each hook sees all its siblings: the physics rebind needs the entity's
    /// Transform, which sorts *after* RigidBodyDescriptor.
    bool AddComponentForRestore(Assisi::ECS::Entity entity, Assisi::Core::Reflect::ComponentId id,
                                const nlohmann::json &data);

    Assisi::ECS::Scene &_scene;
    Assisi::Runtime::InstanceTable *_instances = nullptr;
    RebindHook _rebind;
    AssetApplyHook _assetApply;
    std::vector<Transaction> _undo;
    std::vector<Transaction> _redo;
    std::vector<OpenGesture> _open; ///< Capture gestures awaiting commit.
    std::uint64_t _nextSeq  = 1;            ///< Next transaction sequence (0 = base state).
    bool _applying = false;
};

} // namespace Assisi::Editor
