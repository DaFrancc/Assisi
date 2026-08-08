/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Editor/InstanceGesture.hpp>

#include <utility>

#include <Assisi/Core/Reflect/ComponentId.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Editor/EditHistory.hpp>
#include <Assisi/Runtime/Components.hpp>

namespace Assisi::Editor
{

namespace
{
namespace Rt = Assisi::Runtime;

/// @brief Whether the placement is not the one that was snapshotted.
///
/// Exact, with no tolerance. `InstancesForSave` writes the row verbatim, so a
/// placement a tolerance forgave would still reach disk — the same hole this
/// comparison exists to close, one size smaller.
///
/// Field by field rather than `operator==`: Transform carries `worldMatrix`,
/// which PropagateTransforms owns and no author edits, so a defaulted comparison
/// would answer a question about derived state.
bool PlacementChanged(const Rt::Transform &before, const Rt::Transform &after)
{
    return before.position != after.position || before.rotation != after.rotation ||
           before.scale != after.scale;
}
} // namespace

void InstanceGesture::Begin(Assisi::ECS::Scene &scene, const Rt::InstanceTable &instances,
                            EditHistory *history, Assisi::ECS::InstanceId instanceId)
{
    // Already open on this instance: keep the snapshot taken when the drag started.
    // Re-capturing here would make `before` the previous frame's value, which is
    // the per-frame history bug wearing a different hat — one entry, but one that
    // only undoes the last frame of the drag.
    if (!instanceId.IsValid() || _instanceId == instanceId)
        return;

    const Rt::BlueprintInstance *row = instances.Find(instanceId);
    if (row == nullptr)
        return;

    _instanceId = instanceId;
    _row        = *row;
    _poses.clear();

    if (history == nullptr)
        return;

    const auto transformId = Assisi::Core::Reflect::ComponentIdOf<Rt::Transform>();
    for (const Assisi::ECS::Entity member : Rt::MembersOf(scene, instanceId))
    {
        if (std::optional<nlohmann::json> pose = history->CaptureComponent(member, transformId))
            _poses.emplace_back(member, std::move(*pose));
    }
}

void InstanceGesture::EndFrame(Assisi::ECS::Scene &scene, const Rt::InstanceTable &instances,
                               EditHistory *history, const char *label)
{
    // Read and clear first, so the flag always describes the frame that just ran
    // whichever branch below is taken.
    const bool held = std::exchange(_held, false);

    if (!_instanceId.IsValid() || held)
        return;

    if (history != nullptr)
    {
        if (const Rt::BlueprintInstance *now = instances.Find(_instanceId))
        {
            Transaction txn;
            txn.label = label;
            txn.cmds.push_back(InstanceDelta{.instanceId = _instanceId, .before = _row, .after = *now});

            const auto transformId = Assisi::Core::Reflect::ComponentIdOf<Rt::Transform>();
            for (const auto &[member, before] : _poses)
            {
                if (!scene.IsAlive(member))
                    continue;
                std::optional<nlohmann::json> after = history->CaptureComponent(member, transformId);
                if (after != std::optional<nlohmann::json>{before})
                    txn.cmds.push_back(ComponentDelta{member, transformId, before, after});
            }

            // Only if something actually moved — a click without a drag is not an
            // edit, the same rule the capture gestures apply.
            //
            // The placement is asked about directly, because it is what a save
            // writes. Counting member deltas instead was asking a proxy question:
            // "did anything the placement carries move?" agrees with "did the
            // placement move?" only while an instance has a member the placement
            // reaches. It has none when every member is parented elsewhere — those
            // ride along through their parent, so ApplyInstancePlacement skips them
            // — or when the members have been deleted out from under the row. Then
            // the move was written to the file, absent from the history, and never
            // dirtied the title bar: the three disagreeing, with the one that
            // touches disk winning (round-7 S15).
            //
            // Both halves are needed. A member can move without the placement
            // moving, which is an override on that member, and that still records.
            if (PlacementChanged(_row.transform, now->transform) || txn.cmds.size() > 1)
                history->Push(std::move(txn));
        }
    }

    Abandon();
}

void InstanceGesture::Abandon()
{
    _instanceId = {};
    _poses.clear();
    _held = false;
}

} // namespace Assisi::Editor
