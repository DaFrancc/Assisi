/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Runtime/Lifecycle.hpp
/// @brief Entity lifecycle utilities.
///
/// Add DestroyTag to an entity to have it destroyed by a cleanup pass
/// (conventionally a CleanupDestroyTag system run at the end of PostUpdate that
/// calls DestroyMarked). The tag is a convenient batch/marker on top of
/// Scene::Destroy — which is itself deferred — so the actual removal lands at
/// the frame's FlushDestroyed(), never mid-Query.
///
/// @code
/// scene.Add<DestroyTag>(entity);  // removed at the next frame flush
/// @endcode

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Prelude.hpp>

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
    // Scene::Destroy is deferred and touches no pool until FlushDestroyed(), so
    // destroying inline while iterating the DestroyTag pool is safe — no need to
    // collect into a temporary first.
    for (auto [e, tag] : scene.Query<DestroyTag>())
    {
        (void)tag;
        scene.Destroy(e);
    }
}

} // namespace Assisi::Runtime