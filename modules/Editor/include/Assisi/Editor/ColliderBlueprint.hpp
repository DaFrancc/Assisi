/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ColliderBlueprint.hpp
/// @brief A model's collision broken into child entities, one per piece: what
///        "Make collider blueprint" writes beside the model.
///
/// A model's collision is one shape, cheap and shared, whose pieces have no
/// identity. Broken into children, each piece is an entity of its own, which a
/// hit can name and gameplay can move or switch off, at the price of an entity
/// each. The blueprint keeps the pieces together and in place, and exploding
/// it gives up the grouping too.

#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Geometry/CollisionData.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace Assisi::Editor
{

/// @brief One child of a collider blueprint: where it stands under the root,
///        and its Collider.
struct ColliderPart
{
    std::string name;
    glm::quat rotation{1.f, 0.f, 0.f, 0.f};
    glm::vec3 position{0.f};
    Physics::Collider collider;
};

/// @brief The fewest pieces a model needs to be broken into a collider
///        blueprint. One piece, or none, is already a single collider: a
///        blueprint of it would add an entity and gain nothing.
inline constexpr std::size_t kMinColliderBlueprintPieces = 2;

/// @brief Whether @p collision has enough pieces to break into a blueprint.
[[nodiscard]] inline bool CanMakeColliderBlueprint(const Geometry::CollisionData &collision)
{
    return collision.pieces.size() >= kMinColliderBlueprintPieces;
}

/// @brief The children @p collision breaks into, in the model's order.
///
/// A primitive piece becomes that primitive, exactly. A hull piece names
/// @p model and its own place in the model's pieces, so its shape stays the
/// model's, shared.
[[nodiscard]] std::vector<ColliderPart> ColliderBlueprintParts(Core::AssetId model,
                                                               const Geometry::CollisionData &collision);

/// @brief Where the blueprint for the model at @p modelPath is written: beside
///        it, with the model's name and `.abp` in place of its extension.
[[nodiscard]] std::string ColliderBlueprintPath(const std::string &modelPath);

/// @brief What collision a model carries, in a line: "Collision: 2 hulls, 1
///        box", or what a Convex collider naming it uses when it authored none.
[[nodiscard]] std::string CollisionSummary(const Geometry::CollisionData &collision);

/// @brief What the asset browser shows and offers for one model.
struct ModelCollisionInfo
{
    std::string summary;

    /// Whether "Make collider blueprint" is offered: see CanMakeColliderBlueprint.
    bool breakable = false;
};

} // namespace Assisi::Editor
