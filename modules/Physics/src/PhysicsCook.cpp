/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file PhysicsCook.cpp
/// @brief Shapes built from a model's collision: read once, built once per
///        model, piece, kind and density, and shared by every collider that
///        asks for the same.

#include "PhysicsInternal.hpp"

#include <Assisi/Core/Assert.hpp>
#include <Assisi/Core/Logger.hpp>

#include <Jolt/Physics/Collision/PhysicsMaterial.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Assisi::Physics
{

namespace
{

/// The least density a model's piece is built at (kg/m³), for the reason
/// MakeShape clamps a primitive's.
constexpr float kMinModelDensity = 1e-3f;

/// Two triangles sharing an edge whose normals agree this closely lie in one
/// face, and their shared edge is not drawn: it is the diagonal a hull's or a
/// mesh's flat face is split along, not an edge of the shape.
constexpr float kCoplanarCosine = 0.9999f;

/// The grid an outline's corners are matched on (m), so the copies of one
/// corner that adjacent triangles give agree.
constexpr float kOutlineWeldCell = 1e-4f;

class NoModels final : public CollisionSource
{
public:
    std::optional<Geometry::CollisionModel> Load(Core::AssetId id) const override
    {
        Core::Log::Warn("PhysicsWorld: model {} cannot be read - this host loads no collision models.",
                        id.ToString());
        return std::nullopt;
    }
};

JPH::Vec3 ToJoltPoint(const glm::vec3 &point)
{
    return JPH::Vec3(point.x, point.y, point.z);
}

JPH::Quat ToJoltRotation(const glm::quat &rotation)
{
    return JPH::Quat(rotation.x, rotation.y, rotation.z, rotation.w).Normalized();
}

/// One piece of a model as a Jolt shape at its own origin, or the reason it is
/// not one.
JPH::ShapeSettings::ShapeResult MakePiece(const Geometry::CollisionModel &model, const Geometry::CollisionPiece &piece,
                                          float density)
{
    const float radius = std::max(piece.radius, JPH::cDefaultConvexRadius);
    const float halfHeight = std::max(piece.halfHeight, JPH::cDefaultConvexRadius);
    JPH::ShapeSettings::ShapeResult result;
    switch (piece.kind)
    {
    case Geometry::CollisionPieceKind::Hull:
    {
        JPH::Array<JPH::Vec3> points;
        points.reserve(piece.pointCount);
        for (std::uint32_t i = 0; i < piece.pointCount; ++i)
        {
            points.push_back(ToJoltPoint(model.collision.points[piece.firstPoint + i]));
        }
        JPH::ConvexHullShapeSettings settings(points, JPH::cDefaultConvexRadius);
        settings.mDensity = density;
        return settings.Create();
    }
    case Geometry::CollisionPieceKind::Box:
    {
        JPH::BoxShapeSettings settings(
            JPH::Vec3::sMax(ToJoltPoint(piece.halfExtents), JPH::Vec3::sReplicate(JPH::cDefaultConvexRadius)));
        settings.mDensity = density;
        return settings.Create();
    }
    case Geometry::CollisionPieceKind::Sphere:
    {
        JPH::SphereShapeSettings settings(radius);
        settings.mDensity = density;
        return settings.Create();
    }
    case Geometry::CollisionPieceKind::Capsule:
    {
        JPH::CapsuleShapeSettings settings(halfHeight, radius);
        settings.mDensity = density;
        return settings.Create();
    }
    case Geometry::CollisionPieceKind::Cylinder:
    {
        JPH::CylinderShapeSettings settings(halfHeight, radius);
        settings.mDensity = density;
        return settings.Create();
    }
    case Geometry::CollisionPieceKind::Count_:
        break;
    }
    result.SetError("unknown piece kind");
    return result;
}

bool IsIdentity(const Geometry::CollisionPiece &piece)
{
    return piece.position == glm::vec3(0.f) && piece.rotation == glm::quat(1.f, 0.f, 0.f, 0.f);
}

/// @p piece built and placed where the model puts it, or the reason it is not.
JPH::ShapeSettings::ShapeResult MakePlacedPiece(const Geometry::CollisionModel &model,
                                                const Geometry::CollisionPiece &piece, float density)
{
    JPH::ShapeSettings::ShapeResult leaf = MakePiece(model, piece, density);
    if (leaf.HasError() || IsIdentity(piece))
    {
        return leaf;
    }
    return JPH::RotatedTranslatedShapeSettings(ToJoltPoint(piece.position), ToJoltRotation(piece.rotation),
                                               leaf.Get())
           .Create();
}

/// Every piece of @p model, in one compound when there are several.
JPH::ShapeSettings::ShapeResult MakePieces(const Geometry::CollisionModel &model, float density,
                                           std::string &failedPiece)
{
    if (model.collision.pieces.size() == 1)
    {
        failedPiece = model.collision.pieces.front().name;
        return MakePlacedPiece(model, model.collision.pieces.front(), density);
    }
    JPH::StaticCompoundShapeSettings settings;
    for (const Geometry::CollisionPiece &piece : model.collision.pieces)
    {
        JPH::ShapeSettings::ShapeResult leaf = MakePiece(model, piece, density);
        if (leaf.HasError())
        {
            failedPiece = piece.name;
            return leaf;
        }
        settings.AddShape(ToJoltPoint(piece.position), ToJoltRotation(piece.rotation), leaf.Get());
    }
    return settings.Create();
}

/// The convex hull of every point of @p model's first level.
JPH::ShapeSettings::ShapeResult MakeWholeHull(const Geometry::CollisionModel &model, float density)
{
    JPH::Array<JPH::Vec3> points;
    points.reserve(model.positions.size());
    for (const glm::vec3 &position : model.positions)
    {
        points.push_back(ToJoltPoint(position));
    }
    JPH::ConvexHullShapeSettings settings(points, JPH::cDefaultConvexRadius);
    settings.mDensity = density;
    return settings.Create();
}

/// @p model's first level as triangles.
JPH::ShapeSettings::ShapeResult MakeTriangles(const Geometry::CollisionModel &model)
{
    JPH::VertexList vertices;
    vertices.reserve(model.positions.size());
    for (const glm::vec3 &position : model.positions)
    {
        vertices.push_back(JPH::Float3(position.x, position.y, position.z));
    }
    JPH::IndexedTriangleList triangles;
    triangles.reserve(model.indices.size() / 3);
    for (std::size_t i = 0; i + 2 < model.indices.size(); i += 3)
    {
        triangles.push_back(JPH::IndexedTriangle(model.indices[i], model.indices[i + 1], model.indices[i + 2]));
    }
    return JPH::MeshShapeSettings(std::move(vertices), std::move(triangles)).Create();
}

/// The shape @p key names, built from @p model, or the reason it is not one.
JPH::ShapeSettings::ShapeResult BuildModelShape(const Geometry::CollisionModel &model, const CookedShapeKey &key,
                                                std::string &failedPiece)
{
    JPH::ShapeSettings::ShapeResult refused;
    if (key.shape == ColliderShape::Mesh)
    {
        if (model.indices.empty())
        {
            refused.SetError("the model has no triangles");
            return refused;
        }
        return MakeTriangles(model);
    }
    const std::vector<Geometry::CollisionPiece> &pieces = model.collision.pieces;
    if (key.piece != kAllCollisionPieces)
    {
        if (key.piece < 0 || static_cast<std::size_t>(key.piece) >= pieces.size())
        {
            refused.SetError("the model has no piece with that number");
            return refused;
        }
        // At the collider's own origin: the entity holding one piece is where
        // that piece is, which is how a model broken into child entities keeps
        // each piece in place.
        failedPiece = pieces[static_cast<std::size_t>(key.piece)].name;
        return MakePiece(model, pieces[static_cast<std::size_t>(key.piece)], key.density);
    }
    if (!pieces.empty())
    {
        return MakePieces(model, key.density, failedPiece);
    }
    if (model.positions.empty())
    {
        refused.SetError("the model has no points");
        return refused;
    }
    return MakeWholeHull(model, key.density);
}

/// A corner of an outline, on the matching grid.
struct OutlineCorner
{
    std::int64_t x = 0;
    std::int64_t y = 0;
    std::int64_t z = 0;

    friend bool operator==(const OutlineCorner &, const OutlineCorner &) = default;
};

OutlineCorner CornerOf(const JPH::Float3 &point)
{
    return OutlineCorner{std::llround(point.x / kOutlineWeldCell), std::llround(point.y / kOutlineWeldCell),
                         std::llround(point.z / kOutlineWeldCell)};
}

/// An edge of an outline, its corners in a fixed order whichever triangle
/// gave it.
struct OutlineEdge
{
    OutlineCorner a;
    OutlineCorner b;

    friend bool operator==(const OutlineEdge &, const OutlineEdge &) = default;
};

struct OutlineEdgeHash
{
    std::size_t operator()(const OutlineEdge &edge) const noexcept
    {
        std::size_t hash = 0;
        // Boost's combine, over the six coordinates.
        constexpr std::size_t kCombine = 0x9E3779B9u;
        for (const std::int64_t value : {edge.a.x, edge.a.y, edge.a.z, edge.b.x, edge.b.y, edge.b.z})
        {
            hash ^= std::hash<std::int64_t>{}(value) + kCombine + (hash << 6u) + (hash >> 2u);
        }
        return hash;
    }
};

bool CornerBefore(const OutlineCorner &a, const OutlineCorner &b)
{
    if (a.x != b.x)
    {
        return a.x < b.x;
    }
    if (a.y != b.y)
    {
        return a.y < b.y;
    }
    return a.z < b.z;
}

/// What is known about one edge: where it runs, and the triangles either side.
struct EdgeFaces
{
    glm::vec3 from{0.f};
    glm::vec3 to{0.f};
    glm::vec3 firstNormal{0.f};
    std::uint32_t triangles = 0;

    /// Whether two of its triangles face different ways, so it is a real edge
    /// of the shape rather than the diagonal of a flat face.
    bool bends = false;
};

using EdgeTable = std::unordered_map<OutlineEdge, EdgeFaces, OutlineEdgeHash>;

void RecordEdge(EdgeTable &edges, const JPH::Float3 &from, const JPH::Float3 &to, const glm::vec3 &normal)
{
    OutlineEdge key{CornerOf(from), CornerOf(to)};
    if (CornerBefore(key.b, key.a))
    {
        std::swap(key.a, key.b);
    }
    EdgeFaces &faces = edges[key];
    if (faces.triangles == 0)
    {
        faces.from = glm::vec3(from.x, from.y, from.z);
        faces.to = glm::vec3(to.x, to.y, to.z);
        faces.firstNormal = normal;
    }
    else if (glm::dot(faces.firstNormal, normal) < kCoplanarCosine)
    {
        faces.bends = true;
    }
    ++faces.triangles;
}

/// The edges of @p shape that bound its faces, in its own space before scale.
void OutlineShape(const JPH::Shape &shape, std::vector<glm::vec3> &out)
{
    EdgeTable edges;
    JPH::Shape::GetTrianglesContext context;
    shape.GetTrianglesStart(context, JPH::AABox::sBiggest(), shape.GetCenterOfMass(), JPH::Quat::sIdentity(),
                            JPH::Vec3::sReplicate(1.f));
    std::array<JPH::Float3, 3 * JPH::Shape::cGetTrianglesMinTrianglesRequested> corners;
    for (;;)
    {
        const std::int32_t count =
            shape.GetTrianglesNext(context, JPH::Shape::cGetTrianglesMinTrianglesRequested, corners.data());
        if (count <= 0)
        {
            break;
        }
        for (std::int32_t t = 0; t < count; ++t)
        {
            const JPH::Float3 &a = corners[static_cast<std::size_t>(3 * t)];
            const JPH::Float3 &b = corners[static_cast<std::size_t>(3 * t + 1)];
            const JPH::Float3 &c = corners[static_cast<std::size_t>(3 * t + 2)];
            const glm::vec3 cross = glm::cross(glm::vec3(b.x - a.x, b.y - a.y, b.z - a.z),
                                               glm::vec3(c.x - a.x, c.y - a.y, c.z - a.z));
            const float length = glm::length(cross);
            if (length <= 0.f)
            {
                continue;
            }
            const glm::vec3 normal = cross / length;
            RecordEdge(edges, a, b, normal);
            RecordEdge(edges, b, c, normal);
            RecordEdge(edges, c, a, normal);
        }
    }
    out.clear();
    for (const std::pair<const OutlineEdge, EdgeFaces> &edge : edges)
    {
        // An edge of one triangle bounds an open mesh; one where faces bend
        // bounds them both. Only the diagonal of a flat face is dropped.
        if (edge.second.triangles == 1 || edge.second.bends)
        {
            out.push_back(edge.second.from);
            out.push_back(edge.second.to);
        }
    }
}

} // namespace

const CollisionSource &NoCollisionAssets()
{
    static const NoModels source;
    return source;
}

CookedShapeKey PhysicsWorld::Impl::CookedKeyOf(const Collider &collider)
{
    // A triangle mesh has no volume, so no density reaches it.
    const bool convex = collider.shape == ColliderShape::Convex;
    return CookedShapeKey{.asset = collider.collisionAsset,
                          .density = convex ? std::max(collider.density, kMinModelDensity) : 0.f,
                          .piece = convex ? collider.collisionPiece : kAllCollisionPieces,
                          .shape = collider.shape};
}

const Geometry::CollisionModel *PhysicsWorld::Impl::ModelOf(Core::AssetId asset) const
{
    using Models = std::unordered_map<Core::AssetId, std::optional<Geometry::CollisionModel>>;
    Models::iterator found = models.find(asset);
    if (found == models.end())
    {
        found = models.emplace(asset, collision.Load(asset)).first;
    }
    return found->second.has_value() ? &*found->second : nullptr;
}

JPH::ShapeRefC PhysicsWorld::Impl::CookedShapeFor(const Collider &collider) const
{
    const CookedShapeKey key = CookedKeyOf(collider);
    using Cooked = std::unordered_map<CookedShapeKey, JPH::ShapeRefC, CookedShapeKeyHash>;
    if (const Cooked::const_iterator found = cooked.find(key); found != cooked.end())
    {
        return found->second;
    }

    JPH::ShapeRefC shape;
    if (collider.collisionAsset.IsNil())
    {
        Core::Log::Warn("PhysicsWorld: a {} collider names no model; it is left out.",
                        collider.shape == ColliderShape::Mesh ? "Mesh" : "Convex");
    }
    else if (const Geometry::CollisionModel *model = ModelOf(collider.collisionAsset); model != nullptr)
    {
        std::string failedPiece;
        const JPH::ShapeSettings::ShapeResult built = BuildModelShape(*model, key, failedPiece);
        if (built.HasError())
        {
            Core::Log::Warn("PhysicsWorld: model {}{}{} cannot be built ({}); colliders naming it are left out.",
                            collider.collisionAsset.ToString(), failedPiece.empty() ? "" : ", piece ", failedPiece,
                            built.GetError().c_str());
        }
        else
        {
            shape = built.Get();
        }
    }
    cooked.emplace(key, shape);
    return shape;
}

void PhysicsWorld::CollisionAssetEdges(const Collider &collider, std::vector<glm::vec3> &out) const
{
    out.clear();
    if (!IsModelShape(collider.shape))
    {
        return;
    }
    const CookedShapeKey key = Impl::CookedKeyOf(collider);
    using Outlines = std::unordered_map<CookedShapeKey, std::vector<glm::vec3>, CookedShapeKeyHash>;
    Outlines::iterator found = _impl->outlines.find(key);
    if (found == _impl->outlines.end())
    {
        std::vector<glm::vec3> edges;
        if (const JPH::ShapeRefC shape = _impl->CookedShapeFor(collider); shape != nullptr)
        {
            OutlineShape(*shape, edges);
        }
        found = _impl->outlines.emplace(key, std::move(edges)).first;
    }
    out = found->second;
}

uint32_t PhysicsWorld::CookedShapeCount() const
{
    uint32_t count = 0;
    for (const std::pair<const CookedShapeKey, JPH::ShapeRefC> &entry : _impl->cooked)
    {
        if (entry.second != nullptr)
        {
            ++count;
        }
    }
    return count;
}

void PhysicsWorld::InvalidateCollisionAssets()
{
    ASSISI_ASSERT(!_impl->stepping, "PhysicsWorld::InvalidateCollisionAssets called while the world is stepping");
    _impl->models.clear();
    _impl->cooked.clear();
    _impl->outlines.clear();
    Rebuild();
}

} // namespace Assisi::Physics
