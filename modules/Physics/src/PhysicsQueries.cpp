/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file PhysicsQueries.cpp
/// @brief Asking the world what is there: ray casts, shape casts and overlaps.
///
/// Each takes the filter of the thing doing the asking, so a query is filtered by
/// the same two-way rule as a collision: it finds a body only if its own mask
/// includes that body's channel and the body's mask includes its channel.
///
/// None of these may be called from inside a contact callback — Jolt asserts,
/// because the bodies are already locked.

#include "PhysicsInternal.hpp"

#include <Assisi/Core/Logger.hpp>

#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace Assisi::Physics
{

namespace
{

/// A sweep shorter than this is treated as no sweep at all.
///
/// Jolt needs a direction, and normalizing a zero-length vector yields NaNs that
/// propagate into every hit fraction. Squared, so the check costs no square root.
constexpr float kMinSweepLengthSq = 1e-12f;

/// How a ray treats what it starts in and what it meets from behind.
JPH::RayCastSettings RaySettings()
{
    JPH::RayCastSettings settings;
    // A ray that starts inside a body reports that body at fraction 0. Spelled
    // out rather than left to Jolt's default, because the single-hit entry point
    // has no settings at all and each shape decides for itself there — so the
    // behaviour would differ between a box and a mesh for no stated reason.
    settings.mTreatConvexAsSolid = true;
    settings.SetBackFaceMode(JPH::EBackFaceMode::IgnoreBackFaces);
    return settings;
}

JPH::RMat44 WorldTransformOf(const Pose &pose)
{
    const JPH::Quat rotation = JPH::Quat(pose.rotation.x, pose.rotation.y, pose.rotation.z, pose.rotation.w).Normalized();
    return JPH::RMat44::sRotationTranslation(rotation, JPH::RVec3(pose.position.x, pose.position.y, pose.position.z));
}

/// What a collider asks a query with: its own channel and mask.
CollisionFilter FilterOfCollider(const Collider &collider)
{
    return CollisionFilter{collider.collidesWith, collider.channel};
}

glm::vec3 ToGlm(JPH::Vec3Arg vector)
{
    return glm::vec3(vector.GetX(), vector.GetY(), vector.GetZ());
}

/// Keeps the first hit per piece of @p hits, in order.
void KeepFirstPerPiece(std::vector<QueryHit> &hits)
{
    std::vector<QueryHit>::iterator kept = hits.begin();
    for (std::vector<QueryHit>::iterator it = hits.begin(); it != hits.end(); ++it)
    {
        const ECS::Entity piece = it->piece;
        const bool seen = std::any_of(hits.begin(), kept, [piece](const QueryHit &hit) { return hit.piece == piece; });
        if (!seen && piece != ECS::NullEntity)
        {
            *kept = *it;
            ++kept;
        }
    }
    hits.erase(kept, hits.end());
}

bool Nearer(const QueryHit &a, const QueryHit &b)
{
    return a.distance < b.distance;
}

bool Deeper(const QueryHit &a, const QueryHit &b)
{
    return a.distance > b.distance;
}

} // namespace

QueryHit PhysicsWorld::Impl::RayHit(glm::vec3 origin, glm::vec3 sweep, const JPH::RayCastResult &result) const
{
    const glm::vec3 position = origin + sweep * result.mFraction;

    QueryHit hit;
    hit.position = position;
    hit.distance = glm::length(sweep) * result.mFraction;
    hit.entity = OwnerOf(EntityFor(result.mBodyID));
    hit.piece = hit.entity;

    // The normal has to come off the body's surface — a ray result carries only
    // which sub-shape was struck, not its orientation.
    JPH::BodyLockRead lock(physicsSystem.GetBodyLockInterface(), result.mBodyID);
    if (lock.Succeeded())
    {
        hit.normal = ToGlm(
            lock.GetBody().GetWorldSpaceSurfaceNormal(result.mSubShapeID2, JPH::RVec3(position.x, position.y, position.z)));
        hit.piece = PieceOf(lock.GetBody(), result.mSubShapeID2);
    }
    return hit;
}

ECS::Entity PhysicsWorld::Impl::PieceStruck(const JPH::BodyID &body, const JPH::SubShapeID &subShape) const
{
    JPH::BodyLockRead lock(physicsSystem.GetBodyLockInterface(), body);
    if (!lock.Succeeded())
    {
        return EntityFor(body);
    }
    return PieceOf(lock.GetBody(), subShape);
}

QueryHit PhysicsWorld::Impl::ShapeHit(glm::vec3 sweep, const JPH::ShapeCastResult &result) const
{
    const JPH::Vec3 axis = result.mPenetrationAxis.NormalizedOr(JPH::Vec3::sZero());

    QueryHit hit;
    hit.position = ToGlm(result.mContactPointOn2);
    // The penetration axis moves the hit body out of the sweep, so its opposite
    // points out of the struck surface — the same sense a ray's normal has.
    hit.normal = -ToGlm(axis);
    hit.distance = glm::length(sweep) * result.mFraction;
    hit.entity = OwnerOf(EntityFor(result.mBodyID2));
    hit.piece = PieceStruck(result.mBodyID2, result.mSubShapeID2);
    return hit;
}

QueryHit PhysicsWorld::Impl::OverlapHit(const JPH::CollideShapeResult &result) const
{
    const JPH::Vec3 axis = result.mPenetrationAxis.NormalizedOr(JPH::Vec3::sZero());

    QueryHit hit;
    hit.position = ToGlm(result.mContactPointOn2);
    hit.normal = -ToGlm(axis);
    hit.distance = result.mPenetrationDepth;
    hit.entity = OwnerOf(EntityFor(result.mBodyID2));
    hit.piece = PieceStruck(result.mBodyID2, result.mSubShapeID2);
    return hit;
}

JPH::ShapeRefC PhysicsWorld::Impl::QueryShapeOf(const Collider &collider) const
{
    if (collider.shape != ColliderShape::Mesh)
    {
        return MakeColliderShape(collider, glm::vec3(1.f), ECS::NullEntity);
    }
    Core::Log::Warn("PhysicsWorld: a query was given a Mesh collider; a triangle mesh cannot be swept or tested "
                    "for overlap, so its model's Convex shape is used.");
    Collider convex = collider;
    convex.shape = ColliderShape::Convex;
    return MakeColliderShape(convex, glm::vec3(1.f), ECS::NullEntity);
}

void PhysicsWorld::Impl::CollectShapeCast(const Collider &collider, const Pose &start, glm::vec3 sweep,
                                          ECS::Entity ignore, JPH::CastShapeCollector &hits) const
{
    // Named, not a temporary in the call below: RShapeCast keeps a bare pointer to
    // the shape, so a ShapeRefC that died at the end of that expression would
    // leave the cast pointing at freed memory.
    const JPH::ShapeRefC swept = QueryShapeOf(collider);
    if (swept == nullptr)
    {
        return;
    }
    const JPH::RShapeCast cast = JPH::RShapeCast::sFromWorldTransform(swept, JPH::Vec3::sReplicate(1.f),
                                                                      WorldTransformOf(start),
                                                                      JPH::Vec3(sweep.x, sweep.y, sweep.z));

    const JPH::ShapeCastSettings settings;
    const FilterLayerFilter layerFilter{layers, FilterOfCollider(collider)};
    const OwnerBodiesFilter bodyFilter{*this, OwnerOf(ignore), /*exceptions=*/ false};

    // Zero base offset, so the contact points come back in world space. Jolt is
    // built here at single precision, where the offset buys no accuracy.
    physicsSystem.GetNarrowPhaseQuery().CastShape(cast, settings, JPH::RVec3::sZero(), hits, {}, layerFilter,
                                                  bodyFilter);
}

void PhysicsWorld::Impl::CollectOverlap(const Collider &collider, const Pose &at, ECS::Entity ignore,
                                        JPH::CollideShapeCollector &hits) const
{
    const JPH::CollideShapeSettings settings;
    const FilterLayerFilter layerFilter{layers, FilterOfCollider(collider)};
    const OwnerBodiesFilter bodyFilter{*this, OwnerOf(ignore), /*exceptions=*/ false};

    // Jolt places the shape by its centre of mass, which an offset collider has
    // away from its entity's origin; placed by the origin, the offset would be
    // undone.
    const JPH::ShapeRefC held = QueryShapeOf(collider);
    if (held == nullptr)
    {
        return;
    }
    const JPH::RMat44 centreOfMass = WorldTransformOf(at).PreTranslated(held->GetCenterOfMass());
    physicsSystem.GetNarrowPhaseQuery().CollideShape(held, JPH::Vec3::sReplicate(1.f), centreOfMass, settings,
                                                     JPH::RVec3::sZero(), hits, {}, layerFilter, bodyFilter);
}

std::optional<QueryHit> PhysicsWorld::CastRay(glm::vec3 origin, glm::vec3 sweep, CollisionFilter filter,
                                              ECS::Entity ignore) const
{
    if (glm::dot(sweep, sweep) < kMinSweepLengthSq)
    {
        return std::nullopt;
    }

    const JPH::RRayCast ray{JPH::RVec3(origin.x, origin.y, origin.z), JPH::Vec3(sweep.x, sweep.y, sweep.z)};
    JPH::ClosestHitCollisionCollector<JPH::CastRayCollector> collector;
    const FilterLayerFilter layerFilter{_impl->layers, filter};
    const Impl::OwnerBodiesFilter bodyFilter{*_impl, _impl->OwnerOf(ignore), /*exceptions=*/ false};

    // The broad-phase filter accepts everything: a query carries no motion type,
    // so it has no business skipping the tree of things that do not move — which
    // is most of what a ray is aimed at.
    _impl->physicsSystem.GetNarrowPhaseQuery().CastRay(ray, RaySettings(), collector, {}, layerFilter, bodyFilter);
    if (!collector.HadHit())
    {
        return std::nullopt;
    }
    return _impl->RayHit(origin, sweep, collector.mHit);
}

void PhysicsWorld::CastRayAll(glm::vec3 origin, glm::vec3 sweep, CollisionFilter filter, ECS::Entity ignore,
                              std::vector<QueryHit> &out) const
{
    out.clear();
    if (glm::dot(sweep, sweep) < kMinSweepLengthSq)
    {
        return;
    }

    const JPH::RRayCast ray{JPH::RVec3(origin.x, origin.y, origin.z), JPH::Vec3(sweep.x, sweep.y, sweep.z)};
    JPH::AllHitCollisionCollector<JPH::CastRayCollector> collector;
    const FilterLayerFilter layerFilter{_impl->layers, filter};
    const Impl::OwnerBodiesFilter bodyFilter{*_impl, _impl->OwnerOf(ignore), /*exceptions=*/ false};
    _impl->physicsSystem.GetNarrowPhaseQuery().CastRay(ray, RaySettings(), collector, {}, layerFilter, bodyFilter);

    collector.Sort();
    for (const JPH::RayCastResult &result : collector.mHits)
    {
        out.push_back(_impl->RayHit(origin, sweep, result));
    }
    KeepFirstPerPiece(out);
}

std::optional<QueryHit> PhysicsWorld::CastShape(const Collider &collider, const Pose &start, glm::vec3 sweep,
                                                ECS::Entity ignore) const
{
    if (glm::dot(sweep, sweep) < kMinSweepLengthSq)
    {
        return std::nullopt;
    }

    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
    _impl->CollectShapeCast(collider, start, sweep, ignore, collector);
    if (!collector.HadHit())
    {
        return std::nullopt;
    }
    return _impl->ShapeHit(sweep, collector.mHit);
}

void PhysicsWorld::CastShapeAll(const Collider &collider, const Pose &start, glm::vec3 sweep, ECS::Entity ignore,
                                std::vector<QueryHit> &out) const
{
    out.clear();
    if (glm::dot(sweep, sweep) < kMinSweepLengthSq)
    {
        return;
    }

    JPH::AllHitCollisionCollector<JPH::CastShapeCollector> collector;
    _impl->CollectShapeCast(collider, start, sweep, ignore, collector);
    for (const JPH::ShapeCastResult &result : collector.mHits)
    {
        out.push_back(_impl->ShapeHit(sweep, result));
    }
    std::stable_sort(out.begin(), out.end(), Nearer);
    KeepFirstPerPiece(out);
}

std::optional<QueryHit> PhysicsWorld::Overlap(const Collider &collider, const Pose &at, ECS::Entity ignore) const
{
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    _impl->CollectOverlap(collider, at, ignore, collector);

    std::optional<QueryHit> deepest;
    for (const JPH::CollideShapeResult &result : collector.mHits)
    {
        const QueryHit hit = _impl->OverlapHit(result);
        if (hit.entity != ECS::NullEntity && (!deepest.has_value() || Deeper(hit, *deepest)))
        {
            deepest = hit;
        }
    }
    return deepest;
}

void PhysicsWorld::OverlapAll(const Collider &collider, const Pose &at, ECS::Entity ignore,
                              std::vector<QueryHit> &out) const
{
    out.clear();
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    _impl->CollectOverlap(collider, at, ignore, collector);

    // One entry per piece, not per contact: a shape can meet another in several
    // places, and a caller asking what is inside a volume wants the things, not
    // the number of ways it touched them.
    for (const JPH::CollideShapeResult &result : collector.mHits)
    {
        out.push_back(_impl->OverlapHit(result));
    }
    std::stable_sort(out.begin(), out.end(), Deeper);
    KeepFirstPerPiece(out);
}

} // namespace Assisi::Physics
