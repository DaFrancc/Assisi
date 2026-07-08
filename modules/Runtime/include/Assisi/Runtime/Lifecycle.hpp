/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Runtime/Lifecycle.hpp
/// @brief Entity lifecycle utilities.
///
/// Add DestroyTag to an entity to have it destroyed by a deferred-cleanup pass
/// (conventionally a CleanupDestroyTag system run at the end of PostUpdate that
/// calls DestroyMarked).
///
/// @code
/// scene.Add<DestroyTag>(entity);  // entity is gone next PostUpdate
/// @endcode

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Prelude.hpp>

#include <vector>

namespace Assisi::Runtime
{

/// @brief Marker component — entity is destroyed at end of PostUpdate.
///
/// No data.  Presence on an entity is the signal; the value is irrelevant.
struct DestroyTag
{
};

/// @brief Destroy all entities that carry DestroyTag.
///
/// Run this once per frame (conventionally a CleanupDestroyTag system at the end
/// of PostUpdate); it can also be called manually in custom loops or unit tests.
inline void DestroyMarked(ECS::Scene &scene)
{
    std::vector<ECS::Entity> dying;
    for (auto [e, tag] : scene.Query<DestroyTag>())
    {
        (void)tag;
        dying.push_back(e);
    }
    for (ECS::Entity e : dying)
        scene.Destroy(e);
}

} // namespace Assisi::Runtime