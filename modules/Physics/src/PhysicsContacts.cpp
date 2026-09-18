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
#include <mutex>
#include <span>
#include <vector>

namespace Assisi::Physics
{

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

    const TouchRecord record{glm::vec3{n.GetX(), n.GetY(), n.GetZ()},
                             linearVelocity(body1),
                             linearVelocity(body2),
                             body1.GetID(),
                             body2.GetID(),
                             body1.IsSensor() || body2.IsSensor()};

    const std::lock_guard<std::mutex> lock(touchMutex);
    touchedThisStep.push_back(record);
}

void PhysicsWorld::Impl::RecordCharacterTouch(const JPH::BodyID &innerBody, const JPH::BodyID &other,
                                              glm::vec3 normal, glm::vec3 characterVelocity)
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

    const bool sensor = bodies.IsAdded(other) && IsTriggerLayer(bodies.GetObjectLayer(other));

    // The character is body1, so the normal it is handed points away from it —
    // the same sense the body-vs-body path records, which is what lets both
    // arrive at a consumer looking identical.
    const TouchRecord record{normal, characterVelocity, otherVelocity, innerBody, other, sensor};

    const std::lock_guard<std::mutex> lock(touchMutex);
    touchedThisStep.push_back(record);
}

CollisionFilter PhysicsWorld::Impl::FilterOf(const JPH::BodyID &id) const
{
    const JPH::ObjectLayer layer = physicsSystem.GetBodyInterface().GetObjectLayer(id);
    return CollisionFilter{MaskOf(layer), static_cast<CollisionChannel>(ChannelOf(layer))};
}

void PhysicsWorld::Impl::WakeInside(const JPH::AABox &bounds, CollisionFilter filter)
{
    const FilterLayerFilter layerFilter{filter};
    physicsSystem.GetBodyInterface().ActivateBodiesInAABox(bounds, {}, layerFilter);
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
    WakeInside(bounds, CollisionFilter{MaskOf(layer), static_cast<CollisionChannel>(ChannelOf(layer))});
}

void PhysicsWorld::Impl::EmitPair(const PairState &state, ContactPhase phase, std::vector<ContactEvent> &out)
{
    const ECS::Entity e1 = EntityFor(state.id1);
    const ECS::Entity e2 = EntityFor(state.id2);

    // Each side is given the normal pointing away from the *other*, which is what
    // a reflection wants and what spares a consumer working out the pair's order.
    if (e1 != ECS::NullEntity)
        out.push_back(ContactEvent{-state.normal, state.velocity1, e1, e2, phase, state.sensor});
    if (e2 != ECS::NullEntity)
        out.push_back(ContactEvent{state.normal, state.velocity2, e2, e1, phase, state.sensor});
}

void PhysicsWorld::Impl::EmitExitsFor(const JPH::BodyID &id, std::vector<ContactEvent> &out)
{
    const std::uint32_t goingKey = id.GetIndexAndSequenceNumber();
    for (auto it = pairs.begin(); it != pairs.end();)
    {
        const PairState &state = it->second;
        if (state.id1.GetIndexAndSequenceNumber() == goingKey ||
            state.id2.GetIndexAndSequenceNumber() == goingKey)
        {
            EmitPair(state, ContactPhase::Exit, out);
            it = pairs.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void PhysicsWorld::Impl::ResolveContactEvents()
{
    // No Jolt worker is inside a callback here, so the buffer is ours alone.
    for (const TouchRecord &touch : touchedThisStep)
    {
        const PairKey key = KeyFor(touch.id1, touch.id2);
        const auto [it, inserted] = pairs.try_emplace(key);
        PairState &state = it->second;

        // Several manifolds, and several collision substeps, report the same pair
        // within one step. The first sets the phase; the rest only refresh what is
        // known about it, so a pair yields one event however often it was seen.
        // That is also what makes a character touching a dynamic body harmless to
        // record twice — once by its own sweep, once by the body-vs-body listener.
        const bool firstThisStep = state.stamp != step;

        state.normal    = touch.normal;
        state.velocity1 = touch.velocity1;
        state.velocity2 = touch.velocity2;
        state.id1       = touch.id1;
        state.id2       = touch.id2;
        state.sensor    = touch.sensor;
        state.stamp     = step;

        if (firstThisStep)
            EmitPair(state, inserted ? ContactPhase::Enter : ContactPhase::Stay, events);
    }
    touchedThisStep.clear();

    // Anything not seen this step either ended or went quiet. A body that fell
    // asleep has not moved, and Jolt simply stopped testing it, so a pair whose
    // bodies are all asleep is still touching and keeps saying so.
    const JPH::BodyInterface &bodies = physicsSystem.GetBodyInterface();
    for (auto it = pairs.begin(); it != pairs.end();)
    {
        PairState &state = it->second;
        if (state.stamp == step)
        {
            ++it;
            continue;
        }

        const bool eitherAwake = (bodies.IsAdded(state.id1) && bodies.IsActive(state.id1)) ||
                                 (bodies.IsAdded(state.id2) && bodies.IsActive(state.id2));
        if (eitherAwake)
        {
            EmitPair(state, ContactPhase::Exit, events);
            it = pairs.erase(it);
        }
        else
        {
            EmitPair(state, ContactPhase::Stay, events);
            state.stamp = step;
            ++it;
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
    (void)subShapeId;
    (void)contactPosition;

    const auto it = _owner.characters.find(static_cast<std::uint32_t>(character->GetUserData()));
    if (it == _owner.characters.end())
    {
        return;
    }
    const CharacterRecord &record = it->second;

    // Jolt's spelling of these is from the body's point of view, which is the
    // opposite of how a character is authored: "can this body push the
    // character" is the character's canBePushed.
    settings.mCanPushCharacter   = record.canBePushed;
    settings.mCanReceiveImpulses = record.canPushBodies;

    const JPH::Vec3 velocity = character->GetLinearVelocity();
    _owner.RecordCharacterTouch(character->GetInnerBodyID(), bodyId,
                                glm::vec3(contactNormal.GetX(), contactNormal.GetY(), contactNormal.GetZ()),
                                glm::vec3(velocity.GetX(), velocity.GetY(), velocity.GetZ()));
}

void PhysicsWorld::Impl::CharacterContacts::OnCharacterContactAdded(
    const JPH::CharacterVirtual *character, const JPH::CharacterVirtual *other,
    const JPH::SubShapeID &subShapeId, JPH::RVec3Arg contactPosition, JPH::Vec3Arg contactNormal,
    JPH::CharacterContactSettings &settings)
{
    (void)subShapeId;
    (void)contactPosition;

    const auto it = _owner.characters.find(static_cast<std::uint32_t>(character->GetUserData()));
    if (it == _owner.characters.end())
    {
        return;
    }
    const CharacterRecord &record = it->second;

    settings.mCanPushCharacter = record.canBePushed;

    // Neither character can be given an impulse: a character is moved by its own
    // step and nothing else, so one shoving the other is gameplay's to express.
    settings.mCanReceiveImpulses = false;

    const JPH::Vec3 velocity = character->GetLinearVelocity();
    _owner.RecordCharacterTouch(character->GetInnerBodyID(), other->GetInnerBodyID(),
                                glm::vec3(contactNormal.GetX(), contactNormal.GetY(), contactNormal.GetZ()),
                                glm::vec3(velocity.GetX(), velocity.GetY(), velocity.GetZ()));
}

std::span<const ContactEvent> PhysicsWorld::ContactEvents() const
{
    return {_impl->events.data(), _impl->events.size()};
}

} // namespace Assisi::Physics
