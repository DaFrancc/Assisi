/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file InstanceGesture.hpp
/// @brief One in-progress move of a blueprint instance's placement, from the
/// frame an edit site grabs it to the frame every site has let go.
///
/// An instance has no root entity to grab — the root evaporates at expansion
/// (docs/blueprint-system-concept.md) — so "moving an instance" means moving a
/// field on a table row and carrying every member the placement reaches. Two
/// sites do that: the gizmo handles and the Inspector's Placement fields. They
/// share one gesture, because they are one edit as far as the author is concerned,
/// and because an undo entry needs the record *and* every pose from before the
/// drag started, neither of which is reconstructible afterwards.
///
/// **No site decides when the gesture is over.** A site holding the placement
/// raises Hold() and nothing else; committing belongs to EndFrame(), called once
/// after every panel has drawn. When each site judged for itself, the gizmo — which
/// draws first and sees "the gizmo is not being held" throughout an Inspector
/// scrub — committed a fresh entry every frame of that scrub, sixty times a second,
/// until the real history had been evicted by its own noise.
///
/// Same accumulate-then-sweep shape as the capture gestures
/// (EditHistory::EndFrameSweep) and the physics freeze after it, for the same
/// reason: a release keyed off one panel's own edge is a release that panel can
/// miss — by not drawing, by drawing in the wrong order, or by early-returning when
/// the selection goes away mid-drag.

#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <Assisi/ECS/Entity.hpp>
#include <Assisi/Runtime/Blueprint.hpp>

namespace Assisi::ECS
{
struct Scene;
}

namespace Assisi::Editor
{

class EditHistory;

/// @brief The open placement gesture, or nothing. One per editor.
class InstanceGesture
{
public:
    /// @brief Whether a gesture is in progress at all.
    [[nodiscard]] bool IsOpen() const { return _instanceId.IsValid(); }

    /// @brief "An edit site still has the placement this frame."
    ///
    /// Raised by the gizmo while its handles are held and by the Inspector while
    /// one of its Placement fields is active. Cleared by EndFrame, so it says
    /// nothing about any frame but this one — and deliberately nothing about
    /// *which* site holds it: two sites cannot hold it at once, and if they could,
    /// the gesture they share would still be one gesture.
    void Hold() { _held = true; }

    /// @brief Opens a gesture on @p instanceId, snapshotting the record and every
    /// member's pose. Idempotent while the same gesture is open, so an edit site
    /// may call it on every frame of a drag without losing the original `before`.
    ///
    /// @p history may be null (nothing is capturing — a play session). The gesture
    /// still opens: the sites' behaviour must not depend on whether history is on.
    void Begin(Assisi::ECS::Scene &scene, const Assisi::Runtime::InstanceTable &instances,
               EditHistory *history, Assisi::ECS::InstanceId instanceId);

    /// @brief Called once per frame, after every panel has drawn.
    ///
    /// Commits the open gesture as one transaction labelled @p label unless a site
    /// held it this frame. Nothing is recorded for a gesture that moved nothing — a
    /// click without a drag is not an edit — and nothing is recorded when
    /// @p history is null.
    ///
    /// "Moved nothing" means the *placement* is where it was, not that no member
    /// shifted. The two part company on an instance the placement reaches no member
    /// of — every member parented elsewhere, or no members left — where taking the
    /// members as the evidence left the move saved but not undoable.
    void EndFrame(Assisi::ECS::Scene &scene, const Assisi::Runtime::InstanceTable &instances,
                  EditHistory *history, const char *label);

    /// @brief Drops the gesture without recording it, for when the scene it was
    /// captured against is going away (level load, world switch, play stop). The
    /// snapshots name entities in that scene and mean nothing in the next one.
    void Abandon();

private:
    /// The instance being moved; invalid when no gesture is open. This *is* the
    /// open flag — there is no separate bool to fall out of step with it.
    Assisi::ECS::InstanceId _instanceId;

    /// The record as it stood before the drag, and every member pose likewise.
    /// Captured once at Begin because the drag overwrites both in place.
    Assisi::Runtime::BlueprintInstance _row;
    std::vector<std::pair<Assisi::ECS::Entity, nlohmann::json>> _poses;

    /// Raised by the edit sites during the frame, read and cleared by EndFrame.
    bool _held = false;
};

} // namespace Assisi::Editor
