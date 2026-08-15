/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file GizmoDrag.hpp
/// @brief One in-progress transform-gizmo drag: the entities it grabbed, and the
/// release edge that turns them into undo entries.
///
/// A gizmo drag is a Transform edit, so it rides EditHistory's ordinary capture
/// gestures — one per (entity, component) — rather than a transaction type of its
/// own. What it adds is force-committing them on release, so a drag is always its
/// OWN undo entry, separate from any inspector Transform edit before or after it.
///
/// **Two things the drawing code cannot be trusted with.**
///
/// The release edge is driven from outside the draw function, not from its bottom.
/// Every early return there — instance mode, a dead or non-editable entity, a
/// Transform that went away — would skip the commit and leave the edge raised, for
/// the next frame that does reach the bottom to fire against whatever is selected
/// by then. The draw function only reports whether the handles are held.
///
/// And the entity set is the *drag's*, fixed at the press edge. The selection is
/// free to change under an open drag; rebuilding the set from it each frame would
/// commit entities the drag never moved.
///
/// Same shape as InstanceGesture: state that outlives one panel's frame does not
/// belong to the panel that draws it.

#include <span>
#include <vector>

#include <Assisi/Core/Reflect/ComponentId.hpp>
#include <Assisi/ECS/Entity.hpp>

namespace Assisi::ECS
{
struct Scene;
}

namespace Assisi::Editor
{

class EditHistory;

/// @brief The open gizmo drag, or nothing. One per editor.
class GizmoDrag
{
public:
    /// @brief Whether a drag is in progress. This *is* the open flag — the entity
    /// set doubles as it, so there is no separate bool to fall out of step.
    [[nodiscard]] bool IsOpen() const { return !_entities.empty(); }

    /// @brief "The handles are held this frame."
    ///
    /// Opens the drag on @p handle plus @p alsoDragged the first time it is called;
    /// every later call on the open drag is a no-op, which is what fixes the set at
    /// the press edge.
    void Hold(Assisi::ECS::Entity handle, std::span<const Assisi::ECS::Entity> alsoDragged);

    /// @brief The release edge: commits every entity the drag opened against as its
    /// own transaction and closes.
    ///
    /// No-op when no drag is open, so the caller may — and does — run it on every
    /// frame the handles are not held, including frames the gizmo never drew.
    ///
    /// An entity that died mid-drag is skipped rather than committed: its component
    /// is gone, so CommitGesture would read the absence as a *removal* and record
    /// one. EditHistory's end-of-frame sweep drops those gestures on the same rule.
    /// @p scene and @p history may be null (no world, or nothing capturing); the
    /// drag still closes.
    void Release(Assisi::ECS::Scene *scene, EditHistory *history, Assisi::Core::Reflect::ComponentId id);

    /// @brief The entities this drag moves, handle first. Empty when none is open.
    ///
    /// Read back by the draw code so mid-drag frames carry the set the drag started
    /// with instead of the current selection.
    [[nodiscard]] std::span<const Assisi::ECS::Entity> Entities() const { return _entities; }

    /// @brief Drops the drag without committing, for when the scene it grabbed is
    /// going away (level load, world switch). The handles it holds mean nothing in
    /// the next scene.
    void Abandon() { _entities.clear(); }

private:
    /// The handle's entity followed by the rest of the multi-selection riding it.
    /// Non-empty exactly while a drag is open.
    std::vector<Assisi::ECS::Entity> _entities;
};

} // namespace Assisi::Editor
