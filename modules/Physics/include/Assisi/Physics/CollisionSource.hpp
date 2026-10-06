/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file CollisionSource.hpp
/// @brief Where a physics world gets the collision a model carries.
///
/// A Collider whose shape is Convex or Mesh names a model by asset id. The world
/// asks its source for that model once, the first time any collider names it,
/// and keeps what it builds from it for as long as the world lives. Each host
/// passes the source that reads what it has: a game its package, the editor its
/// source files.

#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Geometry/CollisionData.hpp>

#include <optional>

namespace Assisi::Physics
{

/// @brief Reads a model's collision by asset id.
///
/// Called by whichever thread reconciles a world, which for a level loading in
/// the background is a worker, so an implementation must be safe to call from
/// any thread, and from several worlds at once.
class CollisionSource
{
public:
    virtual ~CollisionSource() = default;

    /// @brief The collision model of the asset @p id, or nullopt when there is
    ///        no such model or it cannot be read. The source logs why.
    [[nodiscard]] virtual std::optional<Geometry::CollisionModel> Load(Core::AssetId id) const = 0;
};

/// @brief A source with no models: every Convex or Mesh collider is left out of
///        the simulation, with a warning naming its asset. For a host that
///        loads no assets, such as a test of primitives.
[[nodiscard]] const CollisionSource &NoCollisionAssets();

} // namespace Assisi::Physics
