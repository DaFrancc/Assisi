/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file PhysicsContacts.cpp
/// @brief Turning "these two touched" into Enter, Stay and Exit.
///
/// Always on. Trigger volumes are authored data, and a switch a level had to
/// remember to flip would make one silently do nothing.
///
/// Jolt's own contact callbacks cannot be the source of these. Its "contact
/// removed" callback fires when a body falls *asleep*, and forbids touching
/// either body because one may already have been destroyed — so a design that
/// trusted it would report a motionless character as having left the volume it is
/// standing in. Instead the callbacks only record which pairs touched, and the
/// phases are derived after the step by comparing that against the previous
/// step's pairs.
///
/// Characters arrive here by a second route. A character's inner body is
/// kinematic, and a kinematic body resting on a static one generates no contact
/// at all, so a character standing on a floor would be invisible to the callbacks
/// above. Its own sweep reports what it touched instead, through the same pair
/// table, and the two routes produce events a consumer cannot tell apart.

#include "PhysicsInternal.hpp"

#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/ContactListener.h>

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <mutex>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Assisi::Physics
{

namespace
{

/// The middle of a manifold's contact points, halfway between the two
/// surfaces: one point that stands for the whole patch.
glm::vec3 ContactMiddle(const JPH::ContactManifold &manifold)
{
    const JPH::uint count = manifold.mRelativeContactPointsOn1.size();
    if (count == 0)
    {
        const JPH::RVec3 base = manifold.mBaseOffset;
        return glm::vec3(base.GetX(), base.GetY(), base.GetZ());
    }
    JPH::Vec3 sum = JPH::Vec3::sZero();
    for (JPH::uint i = 0; i < count; ++i)
    {
        sum += manifold.mRelativeContactPointsOn1[i] + manifold.mRelativeContactPointsOn2[i];
    }
    const JPH::RVec3 middle = manifold.mBaseOffset + sum / (2.f * static_cast<float>(count));
    return glm::vec3(middle.GetX(), middle.GetY(), middle.GetZ());
}

/// The piece pair's order, for a tie between two equally deep touches.
bool PiecesBefore(ECS::Entity a1, ECS::Entity a2, ECS::Entity b1, ECS::Entity b2)
{
    if (a1.index != b1.index)
    {
        return a1.index < b1.index;
    }
    return a2.index < b2.index;
}

} // namespace

PhysicsWorld::Impl::TouchRecord PhysicsWorld::Impl::OrientedAs(const TouchRecord &touch, const PairState &state)
{
    if (touch.id1 == state.id1)
    {
        return touch;
    }
    // Jolt names a pair's bodies in either order, and a character's sweep
    // names the character first; the pair keeps the order it was first seen in.
    TouchRecord swapped = touch;
    swapped.normal = -touch.normal;
    swapped.velocity1 = touch.velocity2;
    swapped.velocity2 = touch.velocity1;
    swapped.piece1 = touch.piece2;
    swapped.piece2 = touch.piece1;
    swapped.id1 = touch.id2;
    swapped.id2 = touch.id1;
    return swapped;
}

bool PhysicsWorld::Impl::DeeperTouch(const TouchRecord &touch, const PairState &state)
{
    if (touch.depth != state.depth)
    {
        return touch.depth > state.depth;
    }
    return PiecesBefore(touch.piece1, touch.piece2, state.piece1, state.piece2);
}

void PhysicsWorld::Impl::RecordTouch(const JPH::Body &body1, const JPH::Body &body2,
                                     const JPH::ContactManifold &manifold)
{
    // Jolt's manifold normal is the direction body2 must move to separate from
    // body1, so it already points away from body1's surface. Which side gets which
    // sign is decided in EmitPair, where the participants are known.
    const JPH::Vec3 n = manifold.mWorldSpaceNormal;

    // Body::GetLinearVelocity asserts on a static body (no motion state to read).
    const auto linearVelocity = [](const JPH::Body &body)
                                {
                                    if (body.IsStatic())
                                        return glm::vec3(0.f);
                                    const JPH::Vec3 v = body.GetLinearVelocity();
                                    return glm::vec3(v.GetX(), v.GetY(), v.GetZ());
                                };

    const TouchRecord record{.normal = glm::vec3{n.GetX(), n.GetY(), n.GetZ()},
                             .point = ContactMiddle(manifold),
                             .velocity1 = linearVelocity(body1),
                             .velocity2 = linearVelocity(body2),
                             .piece1 = PieceOf(body1, manifold.mSubShapeID1),
                             .piece2 = PieceOf(body2, manifold.mSubShapeID2),
                             .id1 = body1.GetID(),
                             .id2 = body2.GetID(),
                             .depth = manifold.mPenetrationDepth,
                             .sensor = body1.IsSensor() || body2.IsSensor()};

    const std::lock_guard<std::mutex> lock(touchMutex);
    touchedThisStep.push_back(record);
}

void PhysicsWorld::Impl::RecordCharacterTouch(const JPH::CharacterVirtual &character, const JPH::BodyID &other,
                                              ECS::Entity otherPiece, JPH::RVec3Arg position, JPH::Vec3Arg normal)
{
    // The no-lock interface, deliberately: this runs inside the character's own
    // sweep, which may already hold the touched body's lock, and it runs on the
    // thread doing the stepping with no Jolt jobs in flight — so the lock would
    // be both a deadlock risk and pointless.
    const JPH::BodyInterface &bodies = physicsSystem.GetBodyInterfaceNoLock();

    glm::vec3 otherVelocity{0.f};
    if (bodies.IsAdded(other) && bodies.GetMotionType(other) != JPH::EMotionType::Static)
    {
        const JPH::Vec3 v = bodies.GetLinearVelocity(other);
        otherVelocity     = glm::vec3(v.GetX(), v.GetY(), v.GetZ());
    }

    const bool sensor = bodies.IsAdded(other) && layers.EntryOf(bodies.GetObjectLayer(other)).trigger;

    // The character is body1, so the normal it is handed points away from it —
    // the same sense the body-vs-body path records, which is what lets both
    // arrive at a consumer looking identical.
    const JPH::Vec3 velocity = character.GetLinearVelocity();
    const TouchRecord record{.normal = glm::vec3(normal.GetX(), normal.GetY(), normal.GetZ()),
                             .point = glm::vec3(position.GetX(), position.GetY(), position.GetZ()),
                             .velocity1 = glm::vec3(velocity.GetX(), velocity.GetY(), velocity.GetZ()),
                             .velocity2 = otherVelocity,
                             .piece1 = EntityOfUserData(character.GetUserData()),
                             .piece2 = otherPiece,
                             .id1 = character.GetInnerBodyID(),
                             .id2 = other,
                             .sensor = sensor};

    const std::lock_guard<std::mutex> lock(touchMutex);
    touchedThisStep.push_back(record);
}

CollisionFilter PhysicsWorld::Impl::FilterOf(const JPH::BodyID &id) const
{
    return layers.FilterOf(physicsSystem.GetBodyInterface().GetObjectLayer(id));
}

void PhysicsWorld::Impl::WakeInside(const JPH::AABox &bounds, CollisionFilter filter)
{
    const FilterLayerFilter layerFilter{layers, filter};
    physicsSystem.GetBodyInterface().ActivateBodiesInAABox(bounds, {}, layerFilter);
}

JPH::AABox PhysicsWorld::Impl::BoundsOf(const JPH::BodyID &id) const
{
    JPH::BodyLockRead lock(physicsSystem.GetBodyLockInterface(), id);
    if (!lock.Succeeded())
        return JPH::AABox{};
    return lock.GetBody().GetWorldSpaceBounds();
}

void PhysicsWorld::Impl::WakeInside(const JPH::BodyID &id)
{
    JPH::BodyLockRead lock(physicsSystem.GetBodyLockInterface(), id);
    if (!lock.Succeeded())
        return;
    const JPH::AABox bounds = lock.GetBody().GetWorldSpaceBounds();
    const JPH::ObjectLayer layer = lock.GetBody().GetObjectLayer();
    lock.ReleaseLock();

    // The lock is released first: ActivateBodiesInAABox takes its own locks, and
    // holding one while it does would be a deadlock waiting for the right pair of
    // bodies.
    WakeInside(bounds, layers.FilterOf(layer));
}

void PhysicsWorld::Impl::EmitPair(const PairState &state, ContactPhase phase, std::vector<ContactEvent> &out)
{
    const ECS::Entity e1 = state.owner1;
    const ECS::Entity e2 = state.owner2;

    // Each side is given the normal pointing away from the *other*, which is what
    // a reflection wants and what spares a consumer working out the pair's order.
    if (e1 != ECS::NullEntity)
    {
        out.push_back(ContactEvent{-state.normal, state.point, state.velocity1, e1, e2, state.piece1, state.piece2,
                                   phase, state.sensor});
    }
    if (e2 != ECS::NullEntity)
    {
        out.push_back(ContactEvent{state.normal, state.point, state.velocity2, e2, e1, state.piece2, state.piece1,
                                   phase, state.sensor});
    }
}

std::pair<PhysicsWorld::Impl::PairState *, bool> PhysicsWorld::Impl::InsertPair(PairKey key,
                                                                                const TouchRecord &touch)
{
    const std::pair<std::unordered_map<PairKey, PairState, PairKeyHash>::iterator, bool> found =
        pairs.try_emplace(key);
    PairState &state = found.first->second;
    if (found.second)
    {
        // Read while both bodies certainly exist, so the Exit a destroyed body
        // causes later can still say who it was. The order the pair is first
        // seen in is the order it keeps.
        state.id1 = touch.id1;
        state.id2 = touch.id2;
        state.entity1 = EntityFor(touch.id1);
        state.entity2 = EntityFor(touch.id2);
        state.owner1 = OwnerOf(state.entity1);
        state.owner2 = OwnerOf(state.entity2);
        for (const ECS::Entity entity : {state.entity1, state.entity2})
        {
            if (BodySlot *slot = SlotFor(entity); slot != nullptr)
            {
                slot->pairKeys.push_back(key);
            }
        }
    }
    return {&state, found.second};
}

void PhysicsWorld::Impl::ErasePair(PairKey key)
{
    const std::unordered_map<PairKey, PairState, PairKeyHash>::iterator found = pairs.find(key);
    if (found == pairs.end())
    {
        return;
    }
    for (const ECS::Entity entity : {found->second.entity1, found->second.entity2})
    {
        if (BodySlot *slot = SlotFor(entity); slot != nullptr)
        {
            std::erase(slot->pairKeys, key);
        }
    }
    pairs.erase(found);
}

void PhysicsWorld::Impl::EndPair(PairKey key)
{
    const std::unordered_map<PairKey, PairState, PairKeyHash>::iterator found = pairs.find(key);
    if (found == pairs.end())
    {
        return;
    }
    EmitPair(found->second, ContactPhase::Exit, events);
    ErasePair(key);
}

void PhysicsWorld::Impl::EmitExitsFor(const JPH::BodyID &id, std::vector<ContactEvent> &out)
{
    const BodySlot *slot = SlotFor(EntityFor(id));
    if (slot == nullptr)
    {
        return;
    }
    // A copy: erasing each pair takes it out of this very list.
    const std::vector<PairKey> keys = slot->pairKeys;
    for (const PairKey key : keys)
    {
        EmitPair(pairs.at(key), ContactPhase::Exit, out);
        ErasePair(key);
    }
}

void PhysicsWorld::Impl::EndUnseenPairsOf(const JPH::BodyID &id)
{
    const BodySlot *slot = SlotFor(EntityFor(id));
    if (slot == nullptr)
    {
        return;
    }
    const std::vector<PairKey> keys = slot->pairKeys;
    for (const PairKey key : keys)
    {
        if (pairs.at(key).stamp != step)
        {
            EndPair(key);
        }
    }
}

void PhysicsWorld::Impl::ResolveContactEvents()
{
    // No Jolt worker is inside a callback here, so the buffers are ours alone.
    for (const TouchRecord &touch : touchedThisStep)
    {
        const std::pair<PairState *, bool> found = InsertPair(KeyFor(touch.id1, touch.id2), touch);
        PairState &state = *found.first;

        // Several manifolds, and several collision substeps, report the same pair
        // within one step. The first sets the phase; the rest only refresh what is
        // known about it, so a pair yields one event however often it was seen.
        // That is also what makes a character touching a dynamic body harmless to
        // record twice — once by its own sweep, once by the body-vs-body listener.
        const bool firstThisStep = state.stamp != step;

        // Of the touches between one pair in a step, the deepest says where they
        // touch and with which pieces; the order Jolt's jobs report them in
        // must not decide it.
        const TouchRecord oriented = OrientedAs(touch, state);
        if (firstThisStep || DeeperTouch(oriented, state))
        {
            state.normal = oriented.normal;
            state.point = oriented.point;
            state.piece1 = oriented.piece1;
            state.piece2 = oriented.piece2;
            state.depth = oriented.depth;
        }
        state.velocity1 = oriented.velocity1;
        state.velocity2 = oriented.velocity2;
        state.sensor = oriented.sensor;
        state.stamp = step;

        if (found.second)
        {
            EmitPair(state, ContactPhase::Enter, events);
        }
        else if (firstThisStep && stayReported)
        {
            EmitPair(state, ContactPhase::Stay, events);
        }
    }
    touchedThisStep.clear();

    // A pair not seen this step ended, or went quiet because its bodies fell
    // asleep: a sleeping body has not moved, Jolt merely stopped testing it.
    // Only three kinds of pair can have ended, so only those are looked at, and
    // a settled scene costs nothing here.
    //
    // A body that was awake for the whole step had every contact it still has
    // reported in it, so any pair of its not seen is over.
    for (const JPH::BodyID &id : activatedAtStart)
    {
        EndUnseenPairsOf(id);
    }
    activatedAtStart.clear();

    // A pair Jolt stopped reporting is over unless its bodies went to sleep
    // together, in which case it is still touching.
    const JPH::BodyInterface &bodies = physicsSystem.GetBodyInterface();
    for (const PairKey key : removedThisStep)
    {
        const std::unordered_map<PairKey, PairState, PairKeyHash>::iterator found = pairs.find(key);
        if (found == pairs.end() || found->second.stamp == step)
        {
            continue;
        }
        const PairState &state = found->second;
        const bool eitherAwake = (bodies.IsAdded(state.id1) && bodies.IsActive(state.id1)) ||
                                 (bodies.IsAdded(state.id2) && bodies.IsActive(state.id2));
        if (eitherAwake)
        {
            EndPair(key);
        }
    }
    removedThisStep.clear();

    // A character's contacts come only from its own sweep, which reports every
    // one of them every step, and a character never sleeps.
    for (const std::pair<const std::uint32_t, CharacterRecord> &entry : characters)
    {
        EndUnseenPairsOf(entry.second.character->GetInnerBodyID());
    }

    // What is left unseen is asleep and still touching.
    if (stayReported)
    {
        for (const std::pair<const PairKey, PairState> &entry : pairs)
        {
            if (entry.second.stamp != step)
            {
                EmitPair(entry.second, ContactPhase::Stay, events);
            }
        }
    }

    // Jolt discovers contacts in whatever order its jobs happened to run, so two
    // runs of the same simulation can produce the same events in different orders.
    // A consumer that accumulates — summing damage, picking a ground contact —
    // would then disagree with itself run to run.
    std::sort(events.begin(), events.end(),
              [](const ContactEvent &a, const ContactEvent &b)
              {
                  if (a.entity.index != b.entity.index)
                      return a.entity.index < b.entity.index;
                  if (a.entity.generation != b.entity.generation)
                      return a.entity.generation < b.entity.generation;
                  if (a.other.index != b.other.index)
                      return a.other.index < b.other.index;
                  if (a.other.generation != b.other.generation)
                      return a.other.generation < b.other.generation;
                  return a.phase < b.phase;
              });
}

void PhysicsWorld::Impl::CharacterContacts::OnContactAdded(const JPH::CharacterVirtual *character,
                                                           const JPH::BodyID &bodyId,
                                                           const JPH::SubShapeID &subShapeId,
                                                           JPH::RVec3Arg contactPosition,
                                                           JPH::Vec3Arg contactNormal,
                                                           JPH::CharacterContactSettings &settings)
{
    const CharacterRecord *found = _owner.FindCharacter(EntityOfUserData(character->GetUserData()));
    if (found == nullptr)
    {
        return;
    }
    const CharacterRecord &record = *found;

    // Jolt's spelling of these is from the body's point of view, which is the
    // opposite of how a character is authored: "can this body push the
    // character" is the character's Pushable.
    settings.mCanPushCharacter   = record.options.Has(CharacterOption::Pushable);
    settings.mCanReceiveImpulses = record.options.Has(CharacterOption::PushesBodies);

    // The no-lock interface for the same reason RecordCharacterTouch gives:
    // the sweep may hold this body's lock already.
    const JPH::BodyInterface &bodies = _owner.physicsSystem.GetBodyInterfaceNoLock();
    ECS::Entity piece = EntityOfUserData(bodies.GetShape(bodyId)->GetSubShapeUserData(subShapeId));
    if (piece == ECS::NullEntity)
    {
        piece = _owner.EntityFor(bodyId);
    }
    _owner.RecordCharacterTouch(*character, bodyId, piece, contactPosition, contactNormal);
}

void PhysicsWorld::Impl::CharacterContacts::OnCharacterContactAdded(
    const JPH::CharacterVirtual *character, const JPH::CharacterVirtual *other,
    const JPH::SubShapeID &subShapeId, JPH::RVec3Arg contactPosition, JPH::Vec3Arg contactNormal,
    JPH::CharacterContactSettings &settings)
{
    (void)subShapeId;

    const CharacterRecord *found = _owner.FindCharacter(EntityOfUserData(character->GetUserData()));
    if (found == nullptr)
    {
        return;
    }
    const CharacterRecord &record = *found;

    settings.mCanPushCharacter = record.options.Has(CharacterOption::Pushable);

    // Neither character can be given an impulse: a character is moved by its own
    // step and nothing else, so one shoving the other is gameplay's to express.
    settings.mCanReceiveImpulses = false;

    _owner.RecordCharacterTouch(*character, other->GetInnerBodyID(), EntityOfUserData(other->GetUserData()),
                                contactPosition, contactNormal);
}

void PhysicsWorld::Impl::CharacterContacts::OnAdjustBodyVelocity(const JPH::CharacterVirtual *character,
                                                                 const JPH::Body &body, JPH::Vec3 &linearVelocity,
                                                                 JPH::Vec3 &angularVelocity)
{
    const CharacterRecord *found = _owner.FindCharacter(EntityOfUserData(character->GetUserData()));
    if (found == nullptr || found->base == ECS::NullEntity)
    {
        return;
    }

    // Read live rather than cached, so the base's own velocity cancels exactly:
    // any remainder would read as the deck creeping under a still character.
    // No-lock for the reason RecordCharacterTouch gives.
    const JPH::BodyInterface &bodies = _owner.physicsSystem.GetBodyInterfaceNoLock();
    const JPH::Vec3 baseLinear = bodies.GetLinearVelocity(found->baseBody);
    const JPH::Vec3 baseAngular = bodies.GetAngularVelocity(found->baseBody);
    const JPH::Vec3 offset = JPH::Vec3(body.GetCenterOfMassPosition() - bodies.GetCenterOfMassPosition(found->baseBody));
    linearVelocity -= baseLinear + baseAngular.Cross(offset);
    angularVelocity -= baseAngular;
}

std::span<const ContactEvent> PhysicsWorld::ContactEvents() const
{
    return {_impl->events.data(), _impl->events.size()};
}

void PhysicsWorld::SetStayEventsReported(bool reported)
{
    _impl->stayReported = reported;
}

bool PhysicsWorld::StayEventsReported() const
{
    return _impl->stayReported;
}

bool PhysicsWorld::IsTouching(ECS::Entity a, ECS::Entity b) const
{
    // Owner against owner, through every body each answers with: a hitbox
    // touching something is its character touching it.
    const ECS::Entity ownerA = _impl->OwnerOf(a);
    const ECS::Entity ownerB = _impl->OwnerOf(b);
    std::vector<JPH::BodyID> bodies;
    _impl->BodiesOf(ownerA, bodies);
    for (const JPH::BodyID &id : bodies)
    {
        const Impl::BodySlot *slot = _impl->SlotFor(_impl->EntityFor(id));
        if (slot == nullptr)
        {
            continue;
        }
        for (const PairKey key : slot->pairKeys)
        {
            const Impl::PairState &state = _impl->pairs.at(key);
            if ((state.owner1 == ownerA && state.owner2 == ownerB) || (state.owner1 == ownerB && state.owner2 == ownerA))
            {
                return true;
            }
        }
    }
    return false;
}

void PhysicsWorld::Touching(ECS::Entity entity, std::vector<ECS::Entity> &out) const
{
    out.clear();
    const ECS::Entity owner = _impl->OwnerOf(entity);
    std::vector<JPH::BodyID> bodies;
    _impl->BodiesOf(owner, bodies);
    for (const JPH::BodyID &id : bodies)
    {
        const Impl::BodySlot *slot = _impl->SlotFor(_impl->EntityFor(id));
        if (slot == nullptr)
        {
            continue;
        }
        for (const PairKey key : slot->pairKeys)
        {
            const Impl::PairState &state = _impl->pairs.at(key);
            const ECS::Entity other = state.owner1 == owner ? state.owner2 : state.owner1;
            if (other != ECS::NullEntity && std::find(out.begin(), out.end(), other) == out.end())
            {
                out.push_back(other);
            }
        }
    }
}

} // namespace Assisi::Physics
