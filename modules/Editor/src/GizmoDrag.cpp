/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Editor/GizmoDrag.hpp>

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Editor/EditHistory.hpp>

namespace Assisi::Editor
{

void GizmoDrag::Hold(Assisi::ECS::Entity handle, std::span<const Assisi::ECS::Entity> alsoDragged)
{
    if (IsOpen())
        return; // the set is fixed at the press edge; later frames only re-assert the hold

    _entities.reserve(alsoDragged.size() + 1);
    _entities.push_back(handle);
    _entities.insert(_entities.end(), alsoDragged.begin(), alsoDragged.end());
}

void GizmoDrag::Release(Assisi::ECS::Scene *scene, EditHistory *history,
                        Assisi::Core::Reflect::ComponentId id)
{
    if (!IsOpen())
        return;

    // Cleared whatever happens below: a release that cannot commit — no scene, no
    // history, a dead entity — is still a release, and an edge left raised is read
    // by a later frame as if this drag were still going.
    const std::vector<Assisi::ECS::Entity> dragged = std::move(_entities);
    _entities.clear();

    if (scene == nullptr || history == nullptr)
        return;

    for (const Assisi::ECS::Entity entity : dragged)
    {
        // A dead entity has no component left to serialize, so committing would
        // record the absence as a removal. Leave it to the end-of-frame sweep, which
        // drops such gestures on the same rule.
        if (scene->IsAlive(entity))
            history->CommitGesture(entity, id);
    }
}

} // namespace Assisi::Editor
