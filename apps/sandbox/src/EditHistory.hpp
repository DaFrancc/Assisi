/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file EditHistory.hpp
/// @brief Editor-only scene undo/redo — a linear stack of reversible edits.
///
/// This is the sandbox editor's Ctrl-Z system. It lives entirely in the editor
/// app (never compiled into a shipped game — see docs/editor-undo-redo-design-notes.md
/// §9) and rests on two neutral runtime primitives added for it: Scene::ReviveAt
/// (exact-identity resurrection) and SceneSerializer::ScopedRawEntityContext
/// (raw-handle EntityRef serialization).
///
/// Model (design doc §3/§4): an edit is captured as a *delta* — the reflected
/// JSON of the affected component(s) before and after — not a whole-scene
/// snapshot and not a bespoke command object. Deltas are tagged unions
/// (std::variant) grouped into a Transaction (one user gesture = one Ctrl-Z).
///
/// This file owns the container and the *apply* engine (Stage 1). The capture
/// layer that builds Transactions from live edits is wired in Stage 2; the
/// entity create/delete gestures in Stage 3. Transactions here are built by the
/// caller (or, in tests, by hand).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include <Assisi/Core/Reflect/ComponentId.hpp>
#include <Assisi/ECS/Entity.hpp>

namespace Assisi::ECS
{
struct Scene;
}

namespace Sandbox
{

/// @brief One component's reversible change on a live entity.
///
/// `before`/`after` hold the component's reflected JSON, or nullopt for "the
/// component was absent on that side". That single optional shape covers value
/// edits (both sides present) *and* add/remove (one side absent) uniformly.
struct ComponentDelta
{
    Assisi::ECS::Entity            entity; ///< Exact (index, generation) while alive.
    Assisi::Core::Reflect::ComponentId id;
    std::optional<nlohmann::json>  before; ///< nullopt = component absent before.
    std::optional<nlohmann::json>  after;  ///< nullopt = component absent after.
};

/// @brief One component's serialized state within an entity-lifetime snapshot.
///
/// A create/delete records the entity's *whole* component set as a list of
/// these (design doc §4 calls this a "per-component blob"; a dedicated pair is
/// clearer than reusing ComponentDelta with one dead optional side). Storing per
/// component — rather than one opaque blob — is what makes the two-phase apply
/// (revive entity, then add components) natural.
struct ComponentSnapshot
{
    Assisi::Core::Reflect::ComponentId id;
    nlohmann::json                     data;
};

/// @brief A whole-entity lifetime change (create or delete), possibly a subtree.
///
/// `before`/`after` are the entity's component set on each side, or nullopt for
/// "the entity did not exist". Create = {before: nullopt, after: components};
/// delete = {before: components, after: nullopt}. A subtree delete is several
/// EntityDeltas in one Transaction.
struct EntityDelta
{
    Assisi::ECS::Entity                              handle; ///< Exact (index, generation).
    std::optional<std::vector<ComponentSnapshot>>    before;
    std::optional<std::vector<ComponentSnapshot>>    after;
};

/// @brief The tagged union of edit kinds. A component field/add/remove edit is a
/// ComponentDelta; an entity create/delete is an EntityDelta.
using EditCommand = std::variant<ComponentDelta, EntityDelta>;

/// @brief One user gesture (a gizmo drag, a slider drag, one add-component) —
/// the atom of undo. Applying it toward `before` is undo; toward `after` is redo.
struct Transaction
{
    std::string              label; ///< "Move", "Add MeshRenderer", … (for the history panel).
    std::vector<EditCommand> cmds;
    Assisi::ECS::Entity      selectionBefore = Assisi::ECS::NullEntity;
    Assisi::ECS::Entity      selectionAfter  = Assisi::ECS::NullEntity;
    std::uint64_t            seq             = 0; ///< Unique sequence, assigned on Push (dirty tracking).
};

/// @brief A linear undo/redo stack over one Scene.
///
/// Push a completed Transaction; Undo()/Redo() replay it against the scene and
/// return the selection to restore. All apply happens under a raw-entity
/// serialization scope so EntityRef fields (e.g. Parent) restore to exact
/// handles rather than silently flattening.
class EditHistory
{
  public:
    /// @brief Post-apply rebind hook: called once per component right after the
    /// engine restores or removes it, so the editor can rebuild the transient
    /// state that serialization excludes (physics body, resolved asset pointers).
    ///   entity  — the affected entity (alive at call time unless being destroyed).
    ///   id      — the component's ComponentId.
    ///   present — true if the component now exists (add/restore), false if it was
    ///             just removed (or the whole entity is about to be destroyed).
    /// The engine stays agnostic about which ids need what; the hook dispatches.
    using RebindHook = std::function<void(Assisi::ECS::Entity entity,
                                          Assisi::Core::Reflect::ComponentId id, bool present)>;

    /// @param scene   The scene edits apply to. Must outlive this history.
    /// @param rebind  Transient-rebuild dispatch (may be empty — then a no-op).
    EditHistory(Assisi::ECS::Scene &scene, RebindHook rebind = {});

    /// @brief Records a completed transaction and clears the redo stack.
    ///
    /// No-op if the transaction has no commands (the caller drops no-op gestures
    /// upstream, but this is a safety net). Enforces the depth cap by dropping the
    /// oldest transaction.
    void Push(Transaction txn);

    // --- Capture (record-before-write, design doc §5) ----------------------
    //
    // ImGui writes a value *inside* the widget call, so the only way to know the
    // pre-edit value is to snapshot it before drawing. Each edit site opens a
    // gesture (RecordBefore) before writing; the gesture is closed either
    // immediately (CommitGesture, for instant edits) or by the end-of-frame sweep
    // (for multi-frame drags/typing). A gesture whose before == after is dropped,
    // which absorbs click-without-drag and Escape-revert for free.

    /// @brief Idempotently opens a capture gesture for (entity, id), snapshotting
    /// the component's current reflected JSON as `before` (nullopt if absent).
    /// Call once per edit site *before* the write. A second call for an
    /// already-open gesture only refreshes its liveness (keeps the original
    /// `before`). No-op while applying. @p selection is restored with the commit.
    void RecordBefore(Assisi::ECS::Entity entity, Assisi::Core::Reflect::ComponentId id, std::string label,
                      Assisi::ECS::Entity selection);

    /// @brief Immediately closes the gesture for (entity, id): serializes `after`
    /// and pushes a transaction unless before == after. For instant edits (asset
    /// pick, eyedropper, add/remove component) that complete within one call.
    /// No-op if no gesture is open for the key, or while applying.
    void CommitGesture(Assisi::ECS::Entity entity, Assisi::Core::Reflect::ComponentId id);

    /// @brief End-of-frame sweep for drag/type gestures. Commits any open gesture
    /// whose widget is no longer being manipulated (or whose component block is no
    /// longer drawn), drops no-ops, and abandons gestures whose entity has died.
    /// @p editingActive is true while an edit widget (inspector drag/type or the
    /// gizmo) is still being held this frame — such gestures stay open.
    void EndFrameSweep(bool editingActive);

    [[nodiscard]] bool CanUndo() const { return !_undo.empty(); }
    [[nodiscard]] bool CanRedo() const { return !_redo.empty(); }

    /// @brief Reverts the most recent transaction. Returns the selection to
    /// restore (its `selectionBefore`), or nullopt if there was nothing to undo.
    std::optional<Assisi::ECS::Entity> Undo();

    /// @brief Re-applies the most recently undone transaction. Returns the
    /// selection to restore (its `selectionAfter`), or nullopt if nothing to redo.
    std::optional<Assisi::ECS::Entity> Redo();

    /// @brief Empties both stacks — call on level load, Stop-play, and Scene::Clear,
    /// which rebuild entity identity densely and dangle every stored handle.
    void Clear();

    /// @brief True while an Undo()/Redo() is applying. The capture layer checks
    /// this to avoid recording the edits the apply itself makes (re-entrancy).
    [[nodiscard]] bool IsApplying() const { return _applying; }

    [[nodiscard]] std::size_t UndoDepth() const { return _undo.size(); }
    [[nodiscard]] std::size_t RedoDepth() const { return _redo.size(); }

    /// @brief Label of the transaction the next Undo()/Redo() would apply, or ""
    /// if the corresponding stack is empty (for the Edit menu / history panel).
    [[nodiscard]] const std::string &NextUndoLabel() const;
    [[nodiscard]] const std::string &NextRedoLabel() const;

    /// @brief Undo-stack labels oldest→newest (index 0 = oldest retained edit; the
    /// last entry is what the next Undo() reverts). For the history panel.
    [[nodiscard]] std::vector<std::string> UndoLabels() const;
    /// @brief Redo-stack labels next-to-redo→oldest-redone (index 0 = what the next
    /// Redo() re-applies).
    [[nodiscard]] std::vector<std::string> RedoLabels() const;

    /// @brief An opaque token identifying the current position in history. Stable
    /// across an undo→redo round-trip, distinct after any new edit. Save it at
    /// SaveLevel and compare to detect unsaved changes (the dirty `*` marker). 0
    /// means "at the base state" (empty undo stack).
    [[nodiscard]] std::uint64_t CurrentStateToken() const
    {
        return _undo.empty() ? 0 : _undo.back().seq;
    }

    /// @brief Snapshot every serializable component of @p entity to JSON under a
    /// raw-entity context — the per-entity payload of an entity create/delete
    /// EntityDelta. Empty for a bare entity. Public so the create/delete edit sites
    /// can build EntityDelta transactions.
    [[nodiscard]] std::vector<ComponentSnapshot> CaptureEntityComponents(Assisi::ECS::Entity entity) const;

    /// @brief Cap on retained transactions; the oldest is dropped past this. JSON
    /// payloads are heavy, so history is bounded regardless of edit count.
    static constexpr std::size_t kMaxDepth = 256;

  private:
    enum class Direction : std::uint8_t
    {
        Undo,
        Redo
    };

    /// @brief One in-progress capture gesture — a component whose `before` has
    /// been snapshotted and whose commit is pending (a drag in progress, or an
    /// instant edit about to be committed this frame).
    struct OpenGesture
    {
        Assisi::ECS::Entity                entity;
        Assisi::Core::Reflect::ComponentId id;
        std::string                        label;
        std::optional<nlohmann::json>      before;
        Assisi::ECS::Entity                selection;
        bool                               touchedThisFrame = true;
    };

    OpenGesture *FindOpen(Assisi::ECS::Entity entity, Assisi::Core::Reflect::ComponentId id);

    /// @brief Serialize a live component to JSON under a raw-entity scope (so
    /// EntityRef fields capture as raw handles), or nullopt if absent. Shared by
    /// capture snapshotting.
    [[nodiscard]] std::optional<nlohmann::json> SnapshotComponent(Assisi::ECS::Entity entity,
                                                                  Assisi::Core::Reflect::ComponentId id) const;

    /// @brief Turn a resolved gesture into a transaction if before != after.
    /// Returns true if a transaction was pushed.
    bool CommitOpenGesture(const OpenGesture &gesture);

    void ApplyTransaction(const Transaction &txn, Direction dir);

    /// @brief Brings component `id` on `entity` to `target`: nullopt removes it,
    /// a value restores it (remove-first-then-add, since Scene::Add rejects an
    /// existing component). Invokes the rebind hook after.
    void RestoreComponent(Assisi::ECS::Entity entity, Assisi::Core::Reflect::ComponentId id,
                          const std::optional<nlohmann::json> &target);

    Assisi::ECS::Scene      &_scene;
    RebindHook               _rebind;
    std::vector<Transaction> _undo;
    std::vector<Transaction> _redo;
    std::vector<OpenGesture> _open; ///< Capture gestures awaiting commit (§5).
    std::uint64_t            _nextSeq  = 1; ///< Next transaction sequence (0 = base state).
    bool                     _applying = false;
};

} // namespace Sandbox
