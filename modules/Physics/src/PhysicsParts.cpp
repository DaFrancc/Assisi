/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file PhysicsParts.cpp
/// @brief Colliders on child entities: the pieces a RigidBody's shape is built
///        from, the followers that ride along with a body or a character, and
///        the owner every one of them answers for.
///
/// A piece has no body of its own. Its shape is a child of its owner's
/// compound, carrying the piece's entity in its user data, so a hit or a
/// contact on it names the piece while the body names the owner. A follower is
/// a kinematic body of its own, put at its entity's pose after every step,
/// because Jolt makes a whole body a sensor or none of it and because a part
/// that only rides along must not move its owner's centre of mass.

#include "PhysicsInternal.hpp"

#include <Assisi/Core/Assert.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Physics/ColliderRole.hpp>

#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/Shape/MutableCompoundShape.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace Assisi::Physics
{

namespace
{

/// The most Parent links followed, as ResolveColliderPlacement does.
constexpr uint32_t kMaxDepth = 256;

/// A piece's place in its owner's body: relative to the owner's position and
/// rotation, which is the frame the body's shape is built in, and at the scale
/// composed down to it.
struct PiecePlacement
{
    glm::quat rotation{1.f, 0.f, 0.f, 0.f};
    glm::vec3 position{0.f};
    glm::vec3 scale{1.f};
};

PiecePlacement PlacementIn(const ECS::Scene &scene, ECS::Entity owner, ECS::Entity piece)
{
    const ECS::Transform ownerWorld = ECS::WorldTransformOf(scene, owner);
    const ECS::Transform pieceWorld = ECS::WorldTransformOf(scene, piece);
    const glm::quat inverse = glm::inverse(glm::normalize(ownerWorld.rotation));
    return PiecePlacement{glm::normalize(inverse * pieceWorld.rotation),
                          inverse * (pieceWorld.position - ownerWorld.position), pieceWorld.scale};
}

bool PlacementIsFinite(const PiecePlacement &placement)
{
    return IsFinite(placement.position) && IsFinite(placement.rotation) && IsFinite(placement.scale);
}

/// The indices of the entities up @p entity's Parent chain, nearest first,
/// ending where the composed pose ends.
void AncestorsOf(const ECS::Scene &scene, ECS::Entity entity, std::vector<std::uint32_t> &out)
{
    out.clear();
    ECS::Entity current = entity;
    for (uint32_t depth = 0; depth < kMaxDepth; ++depth)
    {
        const ECS::Parent *parent = scene.Get<ECS::Parent>(current);
        if (parent == nullptr || parent->parent == ECS::NullEntity || !scene.Has<ECS::Transform>(parent->parent))
        {
            return;
        }
        current = parent->parent;
        out.push_back(current.index);
    }
}

bool EntityBefore(ECS::Entity a, ECS::Entity b)
{
    return a.index < b.index || (a.index == b.index && a.generation < b.generation);
}

bool Contains(const std::vector<ECS::Entity> &entities, ECS::Entity entity)
{
    return std::find(entities.begin(), entities.end(), entity) != entities.end();
}

bool PairNames(EntityPair pair, JPH::uint64 entity)
{
    return pair.low == entity || pair.high == entity;
}

} // namespace

const Collider *EnabledCollider(const ECS::Scene &scene, ECS::Entity entity)
{
    const Collider *collider = scene.Get<Collider>(entity);
    return collider != nullptr && collider->enabled ? collider : nullptr;
}

// ---------------------------------------------------------------------------
// Owners
// ---------------------------------------------------------------------------

ECS::Entity PhysicsWorld::Impl::OwnerOf(ECS::Entity entity) const
{
    const BodySlot *slot = SlotFor(entity);
    if (slot != nullptr && (slot->kind == SlotKind::Piece || slot->kind == SlotKind::Follower))
    {
        return slot->owner;
    }
    return entity;
}

PhysicsWorld::Impl::BodySlot *PhysicsWorld::Impl::OwnBodySlot(ECS::Entity entity)
{
    BodySlot *slot = SlotFor(entity);
    if (slot == nullptr || (slot->kind != SlotKind::Body && slot->kind != SlotKind::Character))
    {
        return nullptr;
    }
    return slot;
}

PhysicsWorld::Impl::BodySlot *PhysicsWorld::Impl::OwnerSlotFor(ECS::Entity entity)
{
    return OwnBodySlot(OwnerOf(entity));
}

const PhysicsWorld::Impl::BodySlot *PhysicsWorld::Impl::OwnerSlotFor(ECS::Entity entity) const
{
    const BodySlot *slot = SlotFor(OwnerOf(entity));
    if (slot == nullptr || (slot->kind != SlotKind::Body && slot->kind != SlotKind::Character))
    {
        return nullptr;
    }
    return slot;
}

bool PhysicsWorld::Impl::IgnoresPair(ECS::Entity a, ECS::Entity b) const
{
    const ECS::Entity ownerA = OwnerOf(a);
    const ECS::Entity ownerB = OwnerOf(b);
    if (ownerA == ownerB && ownerA != ECS::NullEntity)
    {
        return true;
    }
    return !ignoredPairs.empty() && ignoredPairs.contains(PairOf(ownerA, ownerB));
}

void PhysicsWorld::Impl::BodiesOf(ECS::Entity owner, std::vector<JPH::BodyID> &out) const
{
    out.clear();
    if (const BodySlot *own = SlotFor(owner);
        own != nullptr && (own->kind == SlotKind::Body || own->kind == SlotKind::Character))
    {
        out.push_back(own->body);
    }
    const std::unordered_map<std::uint32_t, std::vector<ECS::Entity>>::const_iterator below =
        partsBelow.find(owner.index);
    if (below == partsBelow.end())
    {
        return;
    }
    for (const ECS::Entity part : below->second)
    {
        const BodySlot *slot = SlotFor(part);
        if (slot != nullptr && slot->kind == SlotKind::Follower && slot->owner == owner)
        {
            out.push_back(slot->body);
        }
    }
}

void PhysicsWorld::Impl::PiecesOf(ECS::Entity owner, std::vector<ECS::Entity> &out) const
{
    out.clear();
    const std::unordered_map<std::uint32_t, std::vector<ECS::Entity>>::const_iterator below =
        partsBelow.find(owner.index);
    if (below == partsBelow.end())
    {
        return;
    }
    for (const ECS::Entity part : below->second)
    {
        const BodySlot *slot = SlotFor(part);
        if (slot != nullptr && slot->kind == SlotKind::Piece && slot->owner == owner)
        {
            out.push_back(part);
        }
    }
    // The order the compound's children are added in, so a body built from the
    // same parts is the same body whichever order the scene listed them.
    std::sort(out.begin(), out.end(), EntityBefore);
}

// ---------------------------------------------------------------------------
// Materials
// ---------------------------------------------------------------------------

const Collider *PhysicsWorld::Impl::MaterialOf(ECS::Entity piece) const
{
    const BodySlot *slot = SlotFor(piece);
    if (slot == nullptr)
    {
        return nullptr;
    }
    if (slot->kind == SlotKind::Piece || slot->kind == SlotKind::Follower ||
        (slot->kind == SlotKind::Body && slot->ownCollider))
    {
        return &slot->collider;
    }
    return nullptr;
}

void PhysicsWorld::Impl::CombineMaterials(const JPH::Body &body1, const JPH::Body &body2,
                                          const JPH::ContactManifold &manifold, JPH::ContactSettings &settings) const
{
    const Collider *material1 = MaterialOf(PieceOf(body1, manifold.mSubShapeID1));
    const Collider *material2 = MaterialOf(PieceOf(body2, manifold.mSubShapeID2));
    const float friction1 = material1 != nullptr ? material1->friction : body1.GetFriction();
    const float friction2 = material2 != nullptr ? material2->friction : body2.GetFriction();
    const float restitution1 = material1 != nullptr ? material1->restitution : body1.GetRestitution();
    const float restitution2 = material2 != nullptr ? material2->restitution : body2.GetRestitution();
    settings.mCombinedFriction = std::sqrt(friction1 * friction2);
    settings.mCombinedRestitution = std::max(restitution1, restitution2);
}

// ---------------------------------------------------------------------------
// The parts table
// ---------------------------------------------------------------------------

void PhysicsWorld::Impl::RegisterAncestors(ECS::Entity entity)
{
    BodySlot *slot = SlotFor(entity);
    if (slot == nullptr)
    {
        return;
    }
    std::vector<std::uint32_t> chain;
    AncestorsOf(scene, entity, chain);
    if (chain == slot->ancestors)
    {
        return;
    }
    UnregisterAncestors(entity, *slot);
    for (const std::uint32_t ancestor : chain)
    {
        partsBelow[ancestor].push_back(entity);
    }
    slot->ancestors = std::move(chain);
}

void PhysicsWorld::Impl::UnregisterAncestors(ECS::Entity entity, BodySlot &slot)
{
    for (const std::uint32_t ancestor : slot.ancestors)
    {
        const std::unordered_map<std::uint32_t, std::vector<ECS::Entity>>::iterator below = partsBelow.find(ancestor);
        if (below == partsBelow.end())
        {
            continue;
        }
        std::erase(below->second, entity);
        if (below->second.empty())
        {
            partsBelow.erase(below);
        }
    }
    slot.ancestors.clear();
}

void PhysicsWorld::Impl::AppendPartsBelow(std::vector<ECS::Entity> &entities) const
{
    const std::size_t count = entities.size();
    for (std::size_t i = 0; i < count; ++i)
    {
        const std::unordered_map<std::uint32_t, std::vector<ECS::Entity>>::const_iterator below =
            partsBelow.find(entities[i].index);
        if (below != partsBelow.end())
        {
            entities.insert(entities.end(), below->second.begin(), below->second.end());
        }
    }
}

void PhysicsWorld::Impl::ForgetIgnoredPairs(ECS::Entity entity)
{
    const JPH::uint64 named = UserDataOf(entity);
    std::erase_if(ignoredPairs, [named](EntityPair pair) { return PairNames(pair, named); });
}

// ---------------------------------------------------------------------------
// Pieces
// ---------------------------------------------------------------------------

void PhysicsWorld::Impl::CreatePiece(ECS::Entity entity, const Collider &collider, ECS::Entity owner)
{
    BodySlot &slot = SlotAt(entity);
    slot = BodySlot{};
    slot.collider = collider;
    slot.owner = owner;
    slot.generation = entity.generation;
    slot.kind = SlotKind::Piece;
    StampTransform(entity);
    ownersToSync.push_back(owner);
}

void PhysicsWorld::Impl::EditPiece(ECS::Entity entity, const Collider &collider)
{
    BodySlot &slot = *SlotFor(entity);
    if (!SameShape(collider, slot.collider))
    {
        ownersToReshape.push_back(slot.owner);
    }
    // Its material is read by the contacts straight from here; its channel is
    // the body's only when it is the first piece of an owner with no Collider
    // of its own, which the owner's sync decides.
    slot.collider = collider;
    ownersToSync.push_back(slot.owner);
}

void PhysicsWorld::Impl::SyncOwner(ECS::Entity owner, bool reshape)
{
    if (!scene.IsAlive(owner) || !scene.Has<ECS::Transform>(owner))
    {
        return;
    }
    const RigidBody *rigidBody = scene.Get<RigidBody>(owner);
    BodySlot *held = SlotFor(owner);
    if (rigidBody == nullptr || (held != nullptr && held->kind != SlotKind::Body))
    {
        // No longer an owner: its own sync made it what it is now.
        return;
    }

    const Collider *own = UsableCollider(owner);
    std::vector<ECS::Entity> pieces;
    PiecesOf(owner, pieces);
    if (own == nullptr && pieces.empty())
    {
        if (held != nullptr)
        {
            DestroySlot(owner.index);
        }
        return;
    }

    // The channel, the sensor flag and the body-wide material are a body's, not
    // a shape's: the owner's own Collider's, or else its first piece's.
    const Collider face = own != nullptr ? *own : slots[pieces.front().index].collider;
    const glm::vec3 scale = scene.Get<ECS::Transform>(owner)->scale;
    // A Mesh is built as Convex while its body is dynamic, so a change of
    // motion changes the shape of a body holding one.
    const bool motionChanged = held != nullptr && held->motion != MotionOf(rigidBody);
    const bool stale = held == nullptr || reshape || held->motion == BodyMotion::Static || held->pieces != pieces ||
                       held->ownCollider != (own != nullptr) ||
                       (own != nullptr && !SameShape(*own, held->collider)) || held->worldScale != scale ||
                       (motionChanged && HoldsMesh(own, pieces));

    JPH::Ref<JPH::MutableCompoundShape> compound = held != nullptr ? held->compound : nullptr;
    JPH::ShapeRefC shape;
    if (stale)
    {
        if (own != nullptr)
        {
            WarnOnClampedScale(owner, *own, scale);
        }
        shape = MakeOwnerShape(owner, own, pieces, scale, compound);
        if (shape == nullptr)
        {
            // Every part's model failed to build, which has been logged.
            if (held != nullptr)
            {
                DestroySlot(owner.index);
            }
            return;
        }
    }

    if (held == nullptr)
    {
        CreateBody(owner, shape, face, rigidBody);
    }
    else
    {
        EditBody(owner, shape, face, rigidBody);
    }

    BodySlot *built = SlotFor(owner);
    if (built == nullptr || !stale)
    {
        return;
    }
    built->pieces = std::move(pieces);
    built->compound = compound;
    built->ownCollider = own != nullptr;
    built->worldScale = scale;
}

JPH::ShapeRefC PhysicsWorld::Impl::MakeOwnerShape(ECS::Entity owner, const Collider *own,
                                                  std::span<const ECS::Entity> pieces, glm::vec3 scale,
                                                  JPH::Ref<JPH::MutableCompoundShape> &compound)
{
    const BodyMotion motion = MotionOf(scene.Get<RigidBody>(owner));
    if (pieces.empty())
    {
        compound = nullptr;
        return MakeColliderShape(AsBuilt(owner, *own, motion), scale, owner);
    }

    // The owner's own Collider first, at the body's origin, then each piece
    // where it sits relative to the owner: the indices the pieces record are
    // these, and RePlacePiece moves a child by its index. Each child names its
    // entity's index, plus one, so a part built of a shared model is still told
    // apart from its owner.
    JPH::MutableCompoundShapeSettings settings;
    uint32_t next = 0;
    if (own != nullptr)
    {
        const JPH::ShapeRefC shape = MakeColliderShape(AsBuilt(owner, *own, motion), scale, owner);
        if (shape != nullptr)
        {
            settings.AddShape(JPH::Vec3::sZero(), JPH::Quat::sIdentity(), shape, owner.index + 1u);
            ++next;
        }
    }
    for (const ECS::Entity piece : pieces)
    {
        BodySlot &slot = slots[piece.index];
        PiecePlacement placement = PlacementIn(scene, owner, piece);
        if (!PlacementIsFinite(placement))
        {
            Core::Log::Warn("PhysicsWorld: piece {} (gen {}) has a pose that is not a number; it sits at its body's "
                            "origin.",
                            piece.index, piece.generation);
            placement = PiecePlacement{};
        }
        WarnOnClampedScale(piece, slot.collider, placement.scale);
        slot.worldScale = placement.scale;
        const JPH::ShapeRefC shape = MakeColliderShape(AsBuilt(piece, slot.collider, motion), placement.scale, piece);
        if (shape == nullptr)
        {
            slot.subShape = kNoChild;
            continue;
        }
        slot.subShape = next++;
        settings.AddShape(ToJoltVector(placement.position), ToJolt(placement.rotation), shape, piece.index + 1u);
    }
    if (next == 0)
    {
        compound = nullptr;
        return nullptr;
    }

    JPH::ShapeSettings::ShapeResult result;
    compound = new JPH::MutableCompoundShape(settings, result);
    return JPH::ShapeRefC(compound.GetPtr());
}

bool PhysicsWorld::Impl::HoldsMesh(const Collider *own, std::span<const ECS::Entity> pieces) const
{
    if (own != nullptr && own->shape == ColliderShape::Mesh)
    {
        return true;
    }
    for (const ECS::Entity piece : pieces)
    {
        if (slots[piece.index].collider.shape == ColliderShape::Mesh)
        {
            return true;
        }
    }
    return false;
}

void PhysicsWorld::Impl::RePlacePiece(ECS::Entity piece)
{
    BodySlot *slot = SlotFor(piece);
    if (slot == nullptr || slot->kind != SlotKind::Piece)
    {
        return;
    }
    // A piece its owner has not been built with has no child to move; the
    // owner's next build places it.
    BodySlot *owner = OwnBodySlot(slot->owner);
    if (owner == nullptr || owner->compound == nullptr || !Contains(owner->pieces, piece) ||
        slot->subShape == kNoChild)
    {
        return;
    }
    const PiecePlacement placement = PlacementIn(scene, slot->owner, piece);
    if (!PlacementIsFinite(placement))
    {
        return;
    }

    JPH::ShapeRefC rescaled;
    if (placement.scale != slot->worldScale)
    {
        WarnOnClampedScale(piece, slot->collider, placement.scale);
        rescaled = MakeColliderShape(AsBuilt(piece, slot->collider, owner->motion), placement.scale, piece);
        if (rescaled == nullptr)
        {
            return;
        }
        slot->worldScale = placement.scale;
    }

    // Jolt keeps a compound's children about its centre of mass, which moves
    // with them. Told where it was, NotifyShapeChanged keeps the body's origin
    // still while the centre shifts under it.
    JPH::Vec3 previousCentre;
    {
        JPH::BodyLockWrite lock(physicsSystem.GetBodyLockInterface(), owner->body);
        if (!lock.Succeeded())
        {
            return;
        }
        previousCentre = owner->compound->GetCenterOfMass();
        if (rescaled != nullptr)
        {
            owner->compound->ModifyShape(slot->subShape, ToJoltVector(placement.position), ToJolt(placement.rotation),
                                         rescaled.GetPtr());
        }
        else
        {
            owner->compound->ModifyShape(slot->subShape, ToJoltVector(placement.position), ToJolt(placement.rotation));
        }
        owner->compound->AdjustCenterOfMass();
    }
    physicsSystem.GetBodyInterface().NotifyShapeChanged(owner->body, previousCentre, /*inUpdateMassProperties=*/ false,
                                                        JPH::EActivation::Activate);
    ApplyMass(owner->body, owner->rigidBody);
}

void PhysicsWorld::Impl::RePlacePiecesBelow(ECS::Entity entity)
{
    if (OwnBodySlot(entity) != nullptr)
    {
        return;
    }
    const std::unordered_map<std::uint32_t, std::vector<ECS::Entity>>::const_iterator below =
        partsBelow.find(entity.index);
    if (below == partsBelow.end())
    {
        return;
    }
    for (const ECS::Entity part : below->second)
    {
        RePlacePiece(part);
    }
}

// ---------------------------------------------------------------------------
// Followers
// ---------------------------------------------------------------------------

void PhysicsWorld::Impl::CreateFollower(ECS::Entity entity, const Collider &collider, ECS::Entity owner)
{
    const ECS::Transform world = ECS::WorldTransformOf(scene, entity);
    if (!IsFinite(world.position) || !IsFinite(world.rotation) || !IsFinite(world.scale))
    {
        Core::Log::Error("PhysicsWorld: entity {} (gen {}) gets no body - its Transform is not a number.",
                         entity.index, entity.generation);
        return;
    }
    const CollisionFilter filter{collider.collidesWith, collider.channel};
    WarnOnClampedScale(entity, collider, world.scale);
    const JPH::ShapeRefC shape = MakeColliderShape(collider, world.scale, entity);
    if (shape == nullptr)
    {
        return;
    }

    JPH::BodyCreationSettings settings(shape, ToJolt(world.position), ToJolt(world.rotation),
                                       JPH::EMotionType::Kinematic, LayerFor(filter, BodyMotion::Kinematic));
    settings.mOverrideMassProperties = JPH::EOverrideMassProperties::MassAndInertiaProvided;
    settings.mMassPropertiesOverride = MassOfShape(*shape);
    settings.mIsSensor = collider.channel == CollisionChannel::Trigger;
    settings.mUserData = UserDataOf(entity);
    settings.mFriction = collider.friction;
    settings.mRestitution = collider.restitution;
    const JPH::BodyID id = physicsSystem.GetBodyInterface().CreateAndAddBody(settings, JPH::EActivation::Activate);
    if (id.IsInvalid())
    {
        Core::Log::Error("PhysicsWorld: entity {} (gen {}) gets no body - the world holds at most {} bodies.",
                         entity.index, entity.generation, maxBodies);
        return;
    }

    BodySlot &slot = SlotAt(entity);
    slot = BodySlot{};
    slot.collider = collider;
    slot.owner = owner;
    slot.placed = Pose{world.rotation, world.position};
    slot.worldScale = world.scale;
    slot.body = id;
    slot.filter = filter;
    slot.generation = entity.generation;
    slot.motion = BodyMotion::Kinematic;
    slot.kind = SlotKind::Follower;
    followers.push_back(entity.index);
    StampTransform(entity);
}

void PhysicsWorld::Impl::EditFollower(ECS::Entity entity, const Collider &collider)
{
    BodySlot &slot = *SlotFor(entity);
    JPH::BodyInterface &bodies = physicsSystem.GetBodyInterface();
    if (!SameShape(collider, slot.collider))
    {
        if (const JPH::ShapeRefC shape = MakeColliderShape(collider, slot.worldScale, entity); shape != nullptr)
        {
            bodies.SetShape(slot.body, shape, /*inUpdateMassProperties=*/ false, JPH::EActivation::Activate);
        }
    }
    if (collider.friction != slot.collider.friction)
    {
        bodies.SetFriction(slot.body, collider.friction);
    }
    if (collider.restitution != slot.collider.restitution)
    {
        bodies.SetRestitution(slot.body, collider.restitution);
    }
    SetBodyFilter(slot, CollisionFilter{collider.collidesWith, collider.channel}, BodyMotion::Kinematic);
    slot.collider = collider;
}

void PhysicsWorld::Impl::PlaceFollowers()
{
    JPH::BodyInterface &bodies = physicsSystem.GetBodyInterface();
    for (const std::uint32_t index : followers)
    {
        BodySlot &slot = slots[index];
        const ECS::Entity entity{index, slot.generation};
        const ECS::Transform world = ECS::WorldTransformOf(scene, entity);
        if (!IsFinite(world.position) || !IsFinite(world.rotation) || !IsFinite(world.scale))
        {
            continue;
        }
        if (world.scale != slot.worldScale)
        {
            WarnOnClampedScale(entity, slot.collider, world.scale);
            if (const JPH::ShapeRefC shape = MakeColliderShape(slot.collider, world.scale, entity); shape != nullptr)
            {
                bodies.SetShape(slot.body, shape, /*inUpdateMassProperties=*/ false, JPH::EActivation::Activate);
            }
            slot.worldScale = world.scale;
        }
        // Put there rather than swept: a follower only rides along, and a
        // sweep would arrive a step late and fling what it met with its owner's
        // speed.
        if (world.position != slot.placed.position || world.rotation != slot.placed.rotation)
        {
            bodies.SetPositionAndRotation(slot.body, ToJolt(world.position), ToJolt(world.rotation),
                                          JPH::EActivation::Activate);
            slot.placed = Pose{world.rotation, world.position};
        }
    }
}

// ---------------------------------------------------------------------------
// Exceptions
// ---------------------------------------------------------------------------

void PhysicsWorld::IgnoreCollision(ECS::Entity a, ECS::Entity b, bool ignore)
{
    ASSISI_ASSERT(!_impl->stepping, "PhysicsWorld::IgnoreCollision called while the world is stepping");

    // Resolved from the scene rather than the slots, so an exception between
    // entities spawned this frame holds before their bodies are built.
    const ECS::Entity ownerA = ResolveColliderPlacement(_impl->scene, a).owner;
    const ECS::Entity ownerB = ResolveColliderPlacement(_impl->scene, b).owner;
    const EntityPair pair = PairOf(ownerA, ownerB);
    if (ignore)
    {
        _impl->ignoredPairs.insert(pair);
    }
    else
    {
        _impl->ignoredPairs.erase(pair);
    }

    // A pair Jolt already knows keeps its cached contacts without asking again,
    // and a pair at rest is not tested at all; both are told to look afresh.
    JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();
    std::vector<JPH::BodyID> answering;
    for (const ECS::Entity owner : {ownerA, ownerB})
    {
        _impl->BodiesOf(owner, answering);
        for (const JPH::BodyID &id : answering)
        {
            bodies.InvalidateContactCache(id);
            if (bodies.GetMotionType(id) != JPH::EMotionType::Static)
            {
                bodies.ActivateBody(id);
            }
        }
    }
}

} // namespace Assisi::Physics
