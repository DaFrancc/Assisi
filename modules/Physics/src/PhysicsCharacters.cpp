/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file PhysicsCharacters.cpp
/// @brief Locomotion: the swept capsule a player or an NPC is, and everything
///        about how it moves.
///
/// A character is not a rigid body being pushed around. Rigid body physics has
/// no concept of a step height, skates down slopes it should hold on, and turns
/// a jump into a guess about impulses. A character is swept through the world
/// instead, under rules this file owns: lose speed to the ground, gain it along
/// the direction asked for, keep in the air whatever it left the ground with,
/// climb what is short enough, slide off what is too steep, and stay on the floor
/// while walking down stairs.
///
/// All of that lives here rather than in a gameplay system because it needs the
/// ground state the sweep produces, and that is stale by a tick anywhere else.
/// What a gameplay system supplies is intent — a direction and a jump — through
/// MoveCharacter.

#include "PhysicsInternal.hpp"

#include <Assisi/Core/Logger.hpp>
#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/TransformPose.hpp>

#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/PhysicsSettings.h>

#include <cstdint>
#include <limits>
#include <map>
#include <utility>

namespace Assisi::Physics
{

JPH::RefConst<JPH::Shape> MakeCharacterShape(float radius, float halfHeight)
{
    const float shapeRadius = glm::max(radius, JPH::cDefaultConvexRadius);
    const float shapeHalfHeight = glm::max(halfHeight, JPH::cDefaultConvexRadius);

    const JPH::RefConst<JPH::Shape> capsule = new JPH::CapsuleShape(shapeHalfHeight, shapeRadius);
    return JPH::RotatedTranslatedShapeSettings(kCharacterUp * (shapeHalfHeight + shapeRadius), JPH::Quat::sIdentity(),
                                               capsule)
           .Create()
           .Get();
}

float CharacterHalfHeight(float radius, float halfHeight)
{
    return glm::max(halfHeight, JPH::cDefaultConvexRadius) + glm::max(radius, JPH::cDefaultConvexRadius);
}

JPH::Vec3 ApplyFriction(JPH::Vec3Arg velocity, float friction, float stopSpeed, float deltaTime)
{
    const float speed = velocity.Length();
    if (speed < kFrictionMinSpeed)
    {
        return JPH::Vec3::sZero();
    }

    const float drop = glm::max(speed, stopSpeed) * friction * deltaTime;
    return velocity * (glm::max(speed - drop, 0.f) / speed);
}

JPH::Vec3 Accelerate(JPH::Vec3Arg velocity, JPH::Vec3Arg wishDirection, float targetSpeed, float maxGain)
{
    const float addSpeed = targetSpeed - velocity.Dot(wishDirection);
    if (addSpeed <= 0.f)
    {
        return velocity;
    }
    return velocity + wishDirection * glm::min(addSpeed, maxGain);
}

JPH::Vec3 LimitSpeed(JPH::Vec3Arg velocity, float maxSpeed)
{
    const float speed = velocity.Length();
    if (speed <= maxSpeed)
    {
        return velocity;
    }
    return velocity * (maxSpeed / speed);
}

JPH::Vec3 PhysicsWorld::Impl::CharacterRecord::Steer(JPH::Vec3Arg velocity, JPH::Vec3Arg wish, bool grounded,
                                                     float deltaTime) const
{
    const JPH::Vec3 slowed = grounded ? ApplyFriction(velocity, friction, stopSpeed, deltaTime) : velocity;

    const float wishSpeed = wish.Length();
    if (wishSpeed < kMinWishSpeed)
    {
        return slowed;
    }
    const JPH::Vec3 wishDirection = wish / wishSpeed;

    if (grounded)
    {
        return Accelerate(slowed, wishDirection, wishSpeed, groundAcceleration * wishSpeed * deltaTime);
    }

    // The target is cut down and the gain is not. A request held straight ahead
    // is already past the small target and adds nothing; one turned away from
    // the velocity is short of it every step and keeps adding at the full rate.
    return Accelerate(slowed, wishDirection, glm::min(wishSpeed, airWishSpeedCap),
                      airAcceleration * wishSpeed * deltaTime);
}

float PhysicsWorld::Impl::CharacterRecord::TakeOffSpeedLimit() const
{
    if (bunnyHop == BunnyHopPolicy::Cap)
    {
        return bunnyHopSpeedCap * walkSpeed;
    }
    if (bunnyHop == BunnyHopPolicy::Disallow)
    {
        return walkSpeed;
    }
    return std::numeric_limits<float>::max();
}

JPH::Vec3 PhysicsWorld::Impl::CharacterRecord::BoostTakeOff(JPH::Vec3Arg velocity, JPH::Vec3Arg wish) const
{
    JPH::Vec3 forward{facing.x, facing.y, facing.z};
    forward -= kCharacterUp * forward.Dot(kCharacterUp);
    forward = forward.NormalizedOr(JPH::Vec3::sZero());

    const float boost = stance == Stance::Crouching ? kJumpBoostCrouching : kJumpBoostStanding;
    const float forwardMove = wish.Dot(forward);
    const float maxSpeed = walkSpeed * (1.f + boost);

    // The excess is taken out of the boost, so a character already over the
    // limit gets a negative one. It is applied along the facing whichever way
    // the character is travelling: facing its travel it is slowed to the limit,
    // and facing away from it the same subtraction speeds it up.
    float addition = glm::abs(forwardMove) * boost;
    const float newSpeed = addition + velocity.Length();
    if (newSpeed > maxSpeed)
    {
        addition -= newSpeed - maxSpeed;
    }
    if (forwardMove < 0.f)
    {
        addition = -addition;
    }
    return velocity + forward * addition;
}

namespace
{

/// Moves @p from toward @p to by at most @p maxDelta without passing it.
float Approach(float from, float to, float maxDelta)
{
    return from < to ? glm::min(from + maxDelta, to) : glm::max(from - maxDelta, to);
}

} // namespace

void PhysicsWorld::Impl::StepCharacters(float deltaTime)
{
    const JPH::Vec3 worldGravity = physicsSystem.GetGravity();

    for (auto &[id, record] : characters)
    {
        (void)id;
        JPH::CharacterVirtual &character = *record.character;

        // Fold in the motion of whatever is underfoot before reading it, or a
        // character on a platform rides last step's velocity.
        character.UpdateGroundVelocity();

        const JPH::Vec3 currentVelocity = character.GetLinearVelocity();
        const JPH::Vec3 groundVelocity = character.GetGroundVelocity();
        const float currentUpSpeed = currentVelocity.Dot(kCharacterUp);
        const float groundUpSpeed = groundVelocity.Dot(kCharacterUp);

        // "Standing" for movement purposes is narrower than Jolt's OnGround: a
        // character that has just jumped is still touching the floor for a step,
        // and taking the ground's velocity there would swallow the jump whole.
        const bool onGround = character.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
        const bool standing = onGround && (currentUpSpeed - groundUpSpeed) < kMaxRisingSpeedWhileGrounded;

        if (standing)
        {
            record.timeSinceGrounded = 0.f;
            record.jumpedSinceGrounded = false;
        }
        else
        {
            record.timeSinceGrounded += deltaTime;
        }

        // A request is live on the step it arrives and for jumpBufferTime after,
        // so a character with no buffer at all still jumps on the step it was
        // asked — the buffer lengthens the request rather than creating it.
        if (record.jumpRequested)
        {
            record.jumpBufferRemaining = record.jumpBufferTime;
        }
        const bool pending = record.jumpRequested || record.jumpBufferRemaining > 0.f;
        record.jumpRequested = false;

        // A jump may fire late (coyote time) but only once per time in the air:
        // without the flag, a character that jumped inside the coyote window
        // would still be inside it on the next step and jump again.
        const bool mayJump = !record.jumpedSinceGrounded && record.timeSinceGrounded <= record.coyoteTime;
        const bool jumping = pending && mayJump;
        if (jumping)
        {
            record.jumpedSinceGrounded = true;
            record.jumpBufferRemaining = 0.f;
        }
        else
        {
            record.jumpBufferRemaining = glm::max(record.jumpBufferRemaining - deltaTime, 0.f);
        }

        const float eyeTarget = record.stance == Stance::Crouching ? record.crouchEyeHeight : record.standingEyeHeight;
        record.prevEyeHeight = record.eyeHeight;
        record.eyeHeight = Approach(record.eyeHeight, eyeTarget, record.eyeSpeed * deltaTime);

        // Steering is done relative to the ground, so a character walking on a
        // moving platform accelerates from a standstill *on the platform* rather
        // than having to out-accelerate the platform's own speed to stay put —
        // and friction holds it still on the platform rather than in the world.
        const JPH::Vec3 reference = standing ? groundVelocity : JPH::Vec3::sZero();
        const JPH::Vec3 relative = currentVelocity - reference;
        JPH::Vec3 relativeHorizontal = relative - kCharacterUp * relative.Dot(kCharacterUp);

        JPH::Vec3 wish{record.wishVelocity.x, record.wishVelocity.y, record.wishVelocity.z};
        wish -= kCharacterUp * wish.Dot(kCharacterUp);
        if (standing)
        {
            // Walking into a slope too steep to climb must not press the
            // character into it — the component heading uphill is dropped, so
            // what is left slides along.
            wish = character.CancelVelocityTowardsSteepSlopes(wish);
        }

        if (jumping)
        {
            relativeHorizontal = record.bunnyHop == BunnyHopPolicy::Boost
                                     ? record.BoostTakeOff(relativeHorizontal, wish)
                                     : LimitSpeed(relativeHorizontal, record.TakeOffSpeedLimit());
        }

        // A character that jumps this step has already left the ground as far
        // as movement goes: it takes no friction and steers by the air rules.
        // That is what lets a jump on the landing step keep its speed.
        const bool grounded = standing && !jumping;
        const JPH::Vec3 steered = record.Steer(relativeHorizontal, wish, grounded, deltaTime);

        JPH::Vec3 newVelocity = reference - kCharacterUp * reference.Dot(kCharacterUp) + steered +
                                kCharacterUp * (standing ? groundUpSpeed : currentUpSpeed);
        if (jumping)
        {
            newVelocity += kCharacterUp * record.jumpSpeed;
        }

        const JPH::Vec3 gravity = worldGravity * record.gravityScale;
        newVelocity += gravity * deltaTime;
        character.SetLinearVelocity(newVelocity);

        JPH::CharacterVirtual::ExtendedUpdateSettings settings;
        settings.mWalkStairsStepUp = kCharacterUp * record.maxStepHeight;

        // Held down to the floor by as much as it can climb: a stair walked up
        // has to be walkable back down, and without this the character leaves
        // the ground at every descending edge and arrives as a series of little
        // falls.
        settings.mStickToFloorStepDown = -kCharacterUp * record.maxStepHeight;

        const FilterLayerFilter layerFilter{record.queryFilter};

        character.ExtendedUpdate(deltaTime, gravity, settings, {}, layerFilter, {}, {}, tempAlloc);
    }
}

void PhysicsWorld::Impl::CharacterRecord::Retune(const CharacterDescriptor &descriptor)
{
    // A crouch taller than standing is not a crouch, and a step taller than the
    // character is a wall. Both are clamped rather than refused: they arrive from
    // an inspector drag, where a refusal would mean a character that vanishes
    // partway through typing a number.
    const float clampedCrouch = glm::min(descriptor.crouchHalfHeight, descriptor.halfHeight);
    const float standingHeight = CharacterHalfHeight(descriptor.radius, descriptor.halfHeight);

    // The sweep must not be stopped by sensors; the inner body still is seen by
    // them.
    queryFilter = CollisionFilter{descriptor.collidesWith, CollisionChannel::Character}.Without(CollisionChannel::Trigger);

    jumpSpeed = descriptor.jumpSpeed;
    walkSpeed = descriptor.walkSpeed;
    friction = descriptor.friction;
    stopSpeed = descriptor.stopSpeed;
    groundAcceleration = descriptor.groundAcceleration;
    airAcceleration = descriptor.airAcceleration;
    airWishSpeedCap = descriptor.airWishSpeedCap;
    bunnyHopSpeedCap = descriptor.bunnyHopSpeedCap;
    bunnyHop = descriptor.bunnyHop;
    standingEyeHeight = descriptor.eyeHeight;
    crouchEyeHeight = descriptor.crouchEyeHeight;
    eyeSpeed = descriptor.eyeSpeed;
    stanceLift = 2.f * (standingHeight - CharacterHalfHeight(descriptor.radius, clampedCrouch));
    gravityScale = descriptor.gravityScale;
    coyoteTime = descriptor.coyoteTime;
    jumpBufferTime = descriptor.jumpBufferTime;
    maxStepHeight = glm::min(descriptor.maxStepHeight, standingHeight * kMaxStepHeightFraction);
    radius = descriptor.radius;
    standingHalfHeight = descriptor.halfHeight;
    crouchHalfHeight = clampedCrouch;
    canPushBodies = descriptor.canPushBodies;
    canBePushed = descriptor.canBePushed;
}

PhysicsWorld::Impl::CharacterRecord *PhysicsWorld::Impl::FindCharacter(ECS::Entity entity)
{
    if (SlotFor(entity) == nullptr)
    {
        return nullptr;
    }
    const std::map<std::uint32_t, CharacterRecord>::iterator it = characters.find(entity.index);
    return it == characters.end() ? nullptr : &it->second;
}

const PhysicsWorld::Impl::CharacterRecord *PhysicsWorld::Impl::FindCharacter(ECS::Entity entity) const
{
    if (SlotFor(entity) == nullptr)
    {
        return nullptr;
    }
    const std::map<std::uint32_t, CharacterRecord>::const_iterator it = characters.find(entity.index);
    return it == characters.end() ? nullptr : &it->second;
}

bool PhysicsWorld::Impl::BuildCharacterVirtual(CharacterRecord &record, const CharacterDescriptor &descriptor,
                                               const Pose &pose)
{
    const float crouchHalfHeight = glm::min(descriptor.crouchHalfHeight, descriptor.halfHeight);
    record.standingShape = MakeCharacterShape(descriptor.radius, descriptor.halfHeight);
    record.crouchingShape = MakeCharacterShape(descriptor.radius, crouchHalfHeight);

    JPH::CharacterVirtualSettings settings;
    settings.mShape = record.standingShape;
    settings.mUp = kCharacterUp;
    settings.mMaxSlopeAngle = glm::radians(descriptor.maxSlopeDegrees);
    settings.mMass = descriptor.mass;
    settings.mMaxStrength = descriptor.canPushBodies ? descriptor.pushStrength : 0.f;

    // Only contacts behind this plane hold the character up. At the bottom of
    // the capsule, so a hand brushing a wall at head height is something it
    // collides with rather than something it stands on.
    settings.mSupportingVolume = JPH::Plane(kCharacterUp, -descriptor.radius);

    settings.mPredictiveContactDistance = kCharacterPredictiveContactDistance;
    settings.mPenetrationRecoverySpeed = kCharacterPenetrationRecoverySpeed;
    settings.mCharacterPadding = kCharacterPadding;
    settings.mEnhancedInternalEdgeRemoval = kCharacterEnhancedInternalEdgeRemoval;

    // The inner body is what the rest of the simulation sees: without it a cast
    // passes through the character, a sensor never reports it, and a fast body
    // tunnels through it. It keeps the descriptor's full mask, Trigger included,
    // which is what lets a trigger volume find it.
    settings.mInnerBodyShape = record.standingShape;
    settings.mInnerBodyLayer =
        PackLayer(CollisionFilter{descriptor.collidesWith, CollisionChannel::Character}, BodyMotion::Kinematic);

    // The entity rides in the character's user data, which Jolt copies onto the
    // inner body, so a contact or a cast that finds a character names it exactly
    // as it does a rigid body.
    JPH::Ref<JPH::CharacterVirtual> built = new JPH::CharacterVirtual(
        &settings, JPH::RVec3(pose.position.x, pose.position.y, pose.position.z),
        JPH::Quat(pose.rotation.x, pose.rotation.y, pose.rotation.z, pose.rotation.w).Normalized(),
        UserDataOf(record.entity), &physicsSystem);
    if (built->GetInnerBodyID().IsInvalid())
    {
        return false;
    }

    if (record.character != nullptr)
    {
        characterVsCharacter.Remove(record.character);
    }
    record.character = built;
    record.character->SetListener(&characterContacts);
    record.character->SetCharacterVsCharacterCollision(&characterVsCharacter);
    characterVsCharacter.Add(record.character);
    return true;
}

void PhysicsWorld::Impl::CreateCharacter(ECS::Entity entity, const CharacterDescriptor &descriptor)
{
    const ECS::Transform &transform = *scene.Get<ECS::Transform>(entity);

    // A character is placed in world space, and a parented Transform is an
    // offset from its parent.
    Pose pose{transform.rotation, transform.position};
    if (const glm::mat4 *parent = ECS::ParentWorldMatrix(scene, entity); parent != nullptr)
    {
        const ECS::Transform world = ECS::PoseUnderParent(transform, *parent);
        pose = Pose{world.rotation, world.position};
    }

    CharacterRecord record;
    record.entity = entity;
    record.Retune(descriptor);
    record.eyeHeight = descriptor.eyeHeight;
    record.prevEyeHeight = descriptor.eyeHeight;
    if (!BuildCharacterVirtual(record, descriptor, pose))
    {
        Core::Log::Error("PhysicsWorld: entity {} (gen {}) gets no character - the world holds at most {} bodies.",
                         entity.index, entity.generation, maxBodies);
        return;
    }

    BodySlot &slot = SlotAt(entity);
    slot = BodySlot{};
    slot.body = record.character->GetInnerBodyID();
    slot.filter = CollisionFilter{descriptor.collidesWith, CollisionChannel::Character};
    slot.generation = entity.generation;
    slot.motion = BodyMotion::Kinematic;
    slot.kind = SlotKind::Character;
    CollapseSnapshot(slot, pose);

    CharacterRecord &placed = characters.insert_or_assign(entity.index, std::move(record)).first->second;
    StampTransform(entity);

    // Found now rather than on the first step: a stance asked for before then
    // has to know whether the character is standing on something, or a crouch
    // at spawn is taken for one in the air and lifts the feet off the floor.
    const FilterLayerFilter layerFilter{placed.queryFilter};
    placed.character->RefreshContacts({}, layerFilter, {}, {}, tempAlloc);
}

void PhysicsWorld::Impl::EditCharacter(ECS::Entity entity, const CharacterDescriptor &descriptor)
{
    CharacterRecord &record = *FindCharacter(entity);
    BodySlot &slot = *SlotFor(entity);
    const float crouchHalfHeight = glm::min(descriptor.crouchHalfHeight, descriptor.halfHeight);
    const bool resized = descriptor.radius != record.radius || descriptor.halfHeight != record.standingHalfHeight ||
                         crouchHalfHeight != record.crouchHalfHeight;

    record.Retune(descriptor);

    JPH::CharacterVirtual &character = *record.character;
    if (resized)
    {
        // The capsule is baked into the solver, so a new one is built where the
        // old one stands, carrying its velocity across. The timers, stance and
        // intent live on the record and are untouched.
        const JPH::Vec3 velocity = character.GetLinearVelocity();
        const JPH::RVec3 position = character.GetPosition();
        const JPH::Quat rotation = character.GetRotation();
        const Pose pose{glm::quat(rotation.GetW(), rotation.GetX(), rotation.GetY(), rotation.GetZ()),
                        glm::vec3(position.GetX(), position.GetY(), position.GetZ())};
        const Stance stance = record.stance;

        // The old inner body goes with the old capsule, and its pairs with it.
        EmitExitsFor(slot.body, pendingExits);
        if (!BuildCharacterVirtual(record, descriptor, pose))
        {
            Core::Log::Error("PhysicsWorld: entity {} (gen {}) could not be resized - the world is full.",
                             entity.index, entity.generation);
            return;
        }
        record.character->SetLinearVelocity(velocity);
        slot.body = record.character->GetInnerBodyID();

        // Built standing; a crouched character goes back into its crouch,
        // which is a shrink and always fits.
        if (stance == Stance::Crouching)
        {
            const FilterLayerFilter layerFilter{record.queryFilter};
            const float maxPenetration =
                kStanceChangePenetrationSlopFactor * physicsSystem.GetPhysicsSettings().mPenetrationSlop;
            (void)record.character->SetShape(record.crouchingShape, maxPenetration, {}, layerFilter, {}, {}, tempAlloc);
            record.character->SetInnerBodyShape(record.crouchingShape);
        }
        return;
    }

    // Everything else the solver holds can be changed on the one it has.
    character.SetMaxSlopeAngle(glm::radians(descriptor.maxSlopeDegrees));
    character.SetMass(descriptor.mass);
    character.SetMaxStrength(descriptor.canPushBodies ? descriptor.pushStrength : 0.f);

    const CollisionFilter filter{descriptor.collidesWith, CollisionChannel::Character};
    physicsSystem.GetBodyInterface().SetObjectLayer(slot.body, PackLayer(filter, BodyMotion::Kinematic));
    slot.filter = filter;
}

void PhysicsWorld::MoveCharacter(ECS::Entity entity, glm::vec3 wishVelocity, bool jump)
{
    Impl::CharacterRecord *record = _impl->FindCharacter(entity);
    if (record == nullptr)
    {
        return;
    }

    record->wishVelocity = wishVelocity;

    // Recorded rather than acted on: whether it fires is the step's decision, and
    // the step is also what gives the request its lifetime, so one made just
    // before landing is not thrown away.
    if (jump)
    {
        record->jumpRequested = true;
    }
}

void PhysicsWorld::SetCharacterFacing(ECS::Entity entity, glm::vec3 forward)
{
    Impl::CharacterRecord *record = _impl->FindCharacter(entity);
    if (record == nullptr)
    {
        return;
    }
    record->facing = forward;
}

bool PhysicsWorld::SetCharacterStance(ECS::Entity entity, Stance stance)
{
    Impl::CharacterRecord *record = _impl->FindCharacter(entity);
    if (record == nullptr)
    {
        return false;
    }
    if (record->stance == stance)
    {
        return true;
    }

    const JPH::Shape *shape =
        stance == Stance::Crouching ? record->crouchingShape.GetPtr() : record->standingShape.GetPtr();

    const FilterLayerFilter layerFilter{record->queryFilter};
    const float maxPenetration =
        kStanceChangePenetrationSlopFactor * _impl->physicsSystem.GetPhysicsSettings().mPenetrationSlop;

    // On the ground the two shapes share their feet and the head moves. In the
    // air they share their head instead and the feet move: crouching pulls them
    // up, which is what lets a crouched jump clear a ledge the jump alone does
    // not, and standing puts them back down.
    const bool airborne = record->character->GetGroundState() != JPH::CharacterBase::EGroundState::OnGround;
    const float feetShift = !airborne ? 0.f : (stance == Stance::Crouching ? record->stanceLift : -record->stanceLift);

    const JPH::RVec3 position = record->character->GetPosition();
    record->character->SetPosition(position + kCharacterUp * feetShift);

    // Growing into something solid fails and changes nothing, which is what makes
    // "stand up" a question rather than a command. Shrinking always succeeds.
    if (!record->character->SetShape(shape, maxPenetration, {}, layerFilter, {}, {}, _impl->tempAlloc))
    {
        record->character->SetPosition(position);
        return false;
    }

    // The inner body is a separate shape and Jolt does not carry this across to
    // it. Left out, a crouched character is still shot at head height.
    record->character->SetInnerBodyShape(shape);
    record->stance = stance;

    // The feet moved and the eye did not, so the eye is that much nearer to or
    // further from them. Both halves of each render blend move together: one
    // blended across the shift and the other not would show the feet
    // travelling under an eye height that had already changed, and the view
    // would dip for a frame.
    Impl::BodySlot &slot = *_impl->SlotFor(entity);
    record->eyeHeight -= feetShift;
    record->prevEyeHeight -= feetShift;
    slot.snapshot.prevPosition.y += feetShift;
    slot.snapshot.curPosition.y += feetShift;
    return true;
}

CharacterState PhysicsWorld::GetCharacterState(ECS::Entity entity) const
{
    const Impl::CharacterRecord *record = _impl->FindCharacter(entity);
    if (record == nullptr)
    {
        return CharacterState{};
    }

    const JPH::CharacterVirtual &virtualCharacter = *record->character;

    CharacterState state;

    const JPH::Vec3 velocity = virtualCharacter.GetLinearVelocity();
    state.velocity = glm::vec3(velocity.GetX(), velocity.GetY(), velocity.GetZ());

    const JPH::Vec3 normal = virtualCharacter.GetGroundNormal();
    state.groundNormal = glm::vec3(normal.GetX(), normal.GetY(), normal.GetZ());

    const JPH::Vec3 groundVelocity = virtualCharacter.GetGroundVelocity();
    state.groundVelocity = glm::vec3(groundVelocity.GetX(), groundVelocity.GetY(), groundVelocity.GetZ());

    state.groundEntity = _impl->EntityFor(virtualCharacter.GetGroundBodyID());
    state.timeSinceGrounded = record->timeSinceGrounded;
    state.eyeHeight = record->eyeHeight;
    state.stance = record->stance;
    state.canJump = !record->jumpedSinceGrounded && record->timeSinceGrounded <= record->coyoteTime;

    // Every enumerator is spelled out rather than defaulted, so a Jolt release
    // that adds one fails the build here instead of silently reading as InAir.
    switch (virtualCharacter.GetGroundState())
    {
    case JPH::CharacterBase::EGroundState::OnGround:
        state.ground = GroundState::OnGround;
        break;
    case JPH::CharacterBase::EGroundState::OnSteepGround:
        state.ground = GroundState::OnSteepGround;
        break;
    case JPH::CharacterBase::EGroundState::NotSupported:
        state.ground = GroundState::NotSupported;
        break;
    case JPH::CharacterBase::EGroundState::InAir:
        state.ground = GroundState::InAir;
        break;
    }

    return state;
}

float PhysicsWorld::GetCharacterEyeHeight(ECS::Entity entity, float alpha) const
{
    const Impl::CharacterRecord *record = _impl->FindCharacter(entity);
    if (record == nullptr)
    {
        return 0.f;
    }
    return glm::mix(record->prevEyeHeight, record->eyeHeight, alpha);
}

} // namespace Assisi::Physics
