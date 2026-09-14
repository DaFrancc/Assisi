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

/// Hides one body from a query.
///
/// What makes a self-cast usable: a character sweeping its own capsule forward
/// starts inside itself, and every convex shape reports a hit at distance 0 for
/// a cast that begins inside it.
///
/// Takes a BodyID rather than an entity, and overrides the *unlocked* half of
/// the filter, because both cost less where this runs. Jolt asks this question
/// once per body a query reaches, and asks the unlocked form first — answering
/// there is an integer compare, and skips locking a body only to reject it.
/// Resolving the caller's entity to a BodyID happens once, before the query.
class IgnoreBodyFilter final : public JPH::BodyFilter
{
public:
    explicit IgnoreBodyFilter(const JPH::BodyID &ignore) : _ignore(ignore) {}

    bool ShouldCollide(const JPH::BodyID &bodyId) const override
    {
        return bodyId.GetIndexAndSequenceNumber() != _ignore.GetIndexAndSequenceNumber();
    }

private:
    JPH::BodyID _ignore;
};

/// A sweep shorter than this is treated as no sweep at all.
///
/// Jolt needs a direction, and normalizing a zero-length vector yields NaNs that
/// propagate into every hit fraction. Squared, so the check costs no square root.
constexpr float kMinSweepLengthSq = 1e-12f;

} // namespace

std::optional<QueryHit> PhysicsWorld::CastRay(glm::vec3 origin, glm::vec3 sweep, CollisionFilter filter,
                                              ECS::Entity ignore) const
{
    const float sweepLengthSq = glm::dot(sweep, sweep);
    if (sweepLengthSq < kMinSweepLengthSq)
        return std::nullopt;

    const JPH::RRayCast ray{JPH::RVec3(origin.x, origin.y, origin.z),
                            JPH::Vec3(sweep.x, sweep.y, sweep.z)};

    JPH::RayCastSettings settings;
    // A ray that starts inside a body reports that body at fraction 0. Spelled
    // out rather than left to Jolt's default, because the single-hit entry point
    // has no settings at all and each shape decides for itself there — so the
    // behaviour would differ between a box and a mesh for no stated reason.
    settings.mTreatConvexAsSolid = true;
    settings.SetBackFaceMode(JPH::EBackFaceMode::IgnoreBackFaces);

    JPH::ClosestHitCollisionCollector<JPH::CastRayCollector> collector;
    const FilterLayerFilter layerFilter{filter};
    const IgnoreBodyFilter  bodyFilter{_impl->BodyFor(ignore)};

    // The broad-phase filter accepts everything: a query carries no motion type,
    // so it has no business skipping the tree of things that do not move — which
    // is most of what a ray is aimed at.
    _impl->physicsSystem.GetNarrowPhaseQuery().CastRay(ray, settings, collector, {}, layerFilter,
                                                       bodyFilter);
    if (!collector.HadHit())
        return std::nullopt;

    const float     sweepLength = std::sqrt(sweepLengthSq);
    const glm::vec3 position    = origin + sweep * collector.mHit.mFraction;

    QueryHit hit;
    hit.position = position;
    hit.distance = sweepLength * collector.mHit.mFraction;
    hit.entity   = _impl->EntityFor(collector.mHit.mBodyID);

    // The normal has to come off the body's surface — a ray result carries only
    // which sub-shape was struck, not its orientation.
    JPH::BodyLockRead lock(_impl->physicsSystem.GetBodyLockInterface(), collector.mHit.mBodyID);
    if (lock.Succeeded())
    {
        const JPH::Vec3 n = lock.GetBody().GetWorldSpaceSurfaceNormal(
            collector.mHit.mSubShapeID2, JPH::RVec3(position.x, position.y, position.z));
        hit.normal = glm::vec3(n.GetX(), n.GetY(), n.GetZ());
    }
    return hit;
}

std::optional<QueryHit> PhysicsWorld::CastShape(const ColliderShapeDesc &shape, const Pose &start,
                                                glm::vec3 sweep, CollisionFilter filter,
                                                ECS::Entity ignore) const
{
    const float sweepLengthSq = glm::dot(sweep, sweep);
    if (sweepLengthSq < kMinSweepLengthSq)
        return std::nullopt;

    const JPH::Quat rotation =
        JPH::Quat(start.rotation.x, start.rotation.y, start.rotation.z, start.rotation.w).Normalized();
    const JPH::RMat44 transform =
        JPH::RMat44::sRotationTranslation(rotation, JPH::RVec3(start.position.x, start.position.y,
                                                               start.position.z));

    // Named, not a temporary in the call below: RShapeCast keeps a bare pointer to
    // the shape, so a ShapeRefC that died at the end of that expression would
    // leave the cast pointing at freed memory.
    const JPH::ShapeRefC   swept = MakeShape(shape);
    const JPH::RShapeCast  cast  = JPH::RShapeCast::sFromWorldTransform(
        swept, JPH::Vec3::sReplicate(1.f), transform, JPH::Vec3(sweep.x, sweep.y, sweep.z));

    const JPH::ShapeCastSettings settings;
    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
    const FilterLayerFilter layerFilter{filter};
    const IgnoreBodyFilter  bodyFilter{_impl->BodyFor(ignore)};

    // Zero base offset, so the contact points come back in world space. Jolt is
    // built here at single precision, where the offset buys no accuracy.
    _impl->physicsSystem.GetNarrowPhaseQuery().CastShape(cast, settings, JPH::RVec3::sZero(), collector,
                                                         {}, layerFilter, bodyFilter);
    if (!collector.HadHit())
        return std::nullopt;

    const JPH::Vec3 contact = collector.mHit.mContactPointOn2;
    const JPH::Vec3 axis    = collector.mHit.mPenetrationAxis.NormalizedOr(JPH::Vec3::sZero());

    QueryHit hit;
    hit.position = glm::vec3(contact.GetX(), contact.GetY(), contact.GetZ());
    // The penetration axis moves the hit body out of the sweep, so its opposite
    // points out of the struck surface — the same sense a ray's normal has.
    hit.normal   = -glm::vec3(axis.GetX(), axis.GetY(), axis.GetZ());
    hit.distance = std::sqrt(sweepLengthSq) * collector.mHit.mFraction;
    hit.entity   = _impl->EntityFor(collector.mHit.mBodyID2);
    return hit;
}

std::vector<ECS::Entity> PhysicsWorld::Overlap(const ColliderShapeDesc &shape, const Pose &at,
                                               CollisionFilter filter, ECS::Entity ignore) const
{
    const JPH::Quat rotation =
        JPH::Quat(at.rotation.x, at.rotation.y, at.rotation.z, at.rotation.w).Normalized();
    const JPH::RMat44 transform = JPH::RMat44::sRotationTranslation(
        rotation, JPH::RVec3(at.position.x, at.position.y, at.position.z));

    const JPH::CollideShapeSettings settings;
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    const FilterLayerFilter layerFilter{filter};
    const IgnoreBodyFilter  bodyFilter{_impl->BodyFor(ignore)};

    _impl->physicsSystem.GetNarrowPhaseQuery().CollideShape(
        MakeShape(shape), JPH::Vec3::sReplicate(1.f), transform, settings, JPH::RVec3::sZero(), collector,
        {}, layerFilter, bodyFilter);

    // One entry per entity, not per contact: a shape can meet another in several
    // places, and a caller asking what is inside a volume wants the things, not
    // the number of ways it touched them.
    std::vector<ECS::Entity> found;
    found.reserve(collector.mHits.size());
    for (const JPH::CollideShapeResult &result : collector.mHits)
    {
        const ECS::Entity entity = _impl->EntityFor(result.mBodyID2);
        if (entity == ECS::NullEntity)
            continue;
        if (std::find(found.begin(), found.end(), entity) == found.end())
            found.push_back(entity);
    }
    return found;
}

} // namespace Assisi::Physics
