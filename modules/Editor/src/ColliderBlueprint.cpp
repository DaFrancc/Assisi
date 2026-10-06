/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Editor/ColliderBlueprint.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string_view>

namespace Assisi::Editor
{

namespace
{

/// The Collider a primitive piece is exactly.
Physics::Collider PrimitiveCollider(const Geometry::CollisionPiece &piece)
{
    Physics::Collider collider;
    collider.halfExtents = piece.halfExtents;
    collider.radius = piece.radius;
    collider.halfHeight = piece.halfHeight;
    switch (piece.kind)
    {
    case Geometry::CollisionPieceKind::Sphere:
        collider.shape = Physics::ColliderShape::Sphere;
        break;
    case Geometry::CollisionPieceKind::Capsule:
        collider.shape = Physics::ColliderShape::Capsule;
        break;
    case Geometry::CollisionPieceKind::Cylinder:
        collider.shape = Physics::ColliderShape::Cylinder;
        break;
    case Geometry::CollisionPieceKind::Box:
    case Geometry::CollisionPieceKind::Hull:
    case Geometry::CollisionPieceKind::Count:
        collider.shape = Physics::ColliderShape::Box;
        break;
    }
    return collider;
}

} // namespace

std::vector<ColliderPart> ColliderBlueprintParts(Core::AssetId model, const Geometry::CollisionData &collision)
{
    std::vector<ColliderPart> parts;
    parts.reserve(collision.pieces.size());
    for (std::size_t i = 0; i < collision.pieces.size(); ++i)
    {
        const Geometry::CollisionPiece &piece = collision.pieces[i];
        ColliderPart part{.name = piece.name, .rotation = piece.rotation, .position = piece.position, .collider = {}};
        if (piece.kind == Geometry::CollisionPieceKind::Hull)
        {
            part.collider.shape = Physics::ColliderShape::Convex;
            part.collider.collisionAsset = model;
            part.collider.collisionPiece = static_cast<int32_t>(i);
        }
        else
        {
            part.collider = PrimitiveCollider(piece);
        }
        parts.push_back(std::move(part));
    }
    return parts;
}

std::string CollisionSummary(const Geometry::CollisionData &collision)
{
    if (collision.pieces.empty())
    {
        return "Collision: none authored. Convex uses the hull of the whole model.";
    }
    constexpr std::size_t kKinds = static_cast<std::size_t>(Geometry::CollisionPieceKind::Count);
    constexpr std::array<std::string_view, kKinds> kSingular{"hull", "box", "sphere", "capsule", "cylinder"};
    constexpr std::array<std::string_view, kKinds> kPlural{"hulls", "boxes", "spheres", "capsules", "cylinders"};
    std::array<std::size_t, kKinds> counts{};
    for (const Geometry::CollisionPiece &piece : collision.pieces)
    {
        ++counts[static_cast<std::size_t>(piece.kind)];
    }
    std::string summary = "Collision:";
    const char *separator = " ";
    for (std::size_t kind = 0; kind < kKinds; ++kind)
    {
        if (counts[kind] == 0)
        {
            continue;
        }
        summary += std::format("{}{} {}", separator, counts[kind], counts[kind] == 1 ? kSingular[kind] : kPlural[kind]);
        separator = ", ";
    }
    return summary;
}

std::string ColliderBlueprintPath(const std::string &modelPath)
{
    const std::size_t slash = modelPath.find_last_of('/');
    const std::size_t dot = modelPath.find_last_of('.');
    const bool hasExtension = dot != std::string::npos && (slash == std::string::npos || dot > slash);
    return (hasExtension ? modelPath.substr(0, dot) : modelPath) + ".abp";
}

} // namespace Assisi::Editor
