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
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/TransformPose.hpp>

#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/PhysicsSettings.h>

#include <cstdint>
#include <expected>
#include <limits>
#include <utility>

namespace Assisi::Physics
{

JPH::RefConst<JPH::Shape> MakeCharacterShape(float radius, float halfHeight)
{
    const float shapeRadius     = glm::max(radius, JPH::cDefaultConvexRadius);
    const float shapeHalfHeight = glm::max(halfHeight, JPH::cDefaultConvexRadius);

    const JPH::RefConst<JPH::Shape> capsule = new JPH::CapsuleShape(shapeHalfHeight, shapeRadius);
    return JPH::RotatedTranslatedShapeSettings(kCharacterUp * (shapeHalfHeight + shapeRadius),
                                               JPH::Quat::sIdentity(), capsule)
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
    forward  = forward.NormalizedOr(JPH::Vec3::sZero());

    const float boost       = stance == Stance::Crouching ? kJumpBoostCrouching : kJumpBoostStanding;
    const float forwardMove = wish.Dot(forward);
    const float maxSpeed    = walkSpeed * (1.f + boost);

    // The excess is taken out of the boost, so a character already over the
    // limit gets a negative one. It is applied along the facing whichever way
    // the character is travelling: facing its travel it is slowed to the limit,
    // and facing away from it the same subtraction speeds it up.
    float       addition = glm::abs(forwardMove) * boost;
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

        // Frozen: held exactly where it is, gravity included. A character that
        // kept falling would drop out from under the cursor dragging it.
        if (record.frozen)
        {
            record.prevEyeHeight = record.eyeHeight;
            character.SetLinearVelocity(JPH::Vec3::sZero());
            continue;
        }

        // Fold in the motion of whatever is underfoot before reading it, or a
        // character on a platform rides last step's velocity.
        character.UpdateGroundVelocity();

        const JPH::Vec3 currentVelocity = character.GetLinearVelocity();
        const JPH::Vec3 groundVelocity  = character.GetGroundVelocity();
        const float     currentUpSpeed  = currentVelocity.Dot(kCharacterUp);
        const float     groundUpSpeed   = groundVelocity.Dot(kCharacterUp);

        // "Standing" for movement purposes is narrower than Jolt's OnGround: a
        // character that has just jumped is still touching the floor for a step,
        // and taking the ground's velocity there would swallow the jump whole.
        const bool onGround =
            character.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
        const bool standing =
            onGround && (currentUpSpeed - groundUpSpeed) < kMaxRisingSpeedWhileGrounded;

        if (standing)
        {
            record.timeSinceGrounded   = 0.f;
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
        const bool pending   = record.jumpRequested || record.jumpBufferRemaining > 0.f;
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
        record.eyeHeight     = Approach(record.eyeHeight, eyeTarget, record.eyeSpeed * deltaTime);

        // Steering is done relative to the ground, so a character walking on a
        // moving platform accelerates from a standstill *on the platform* rather
        // than having to out-accelerate the platform's own speed to stay put —
        // and friction holds it still on the platform rather than in the world.
        const JPH::Vec3 reference          = standing ? groundVelocity : JPH::Vec3::sZero();
        const JPH::Vec3 relative           = currentVelocity - reference;
        JPH::Vec3       relativeHorizontal = relative - kCharacterUp * relative.Dot(kCharacterUp);

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
        const bool      grounded = standing && !jumping;
        const JPH::Vec3 steered  = record.Steer(relativeHorizontal, wish, grounded, deltaTime);

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

std::expected<Character, PhysicsWorld::PhysicsError> PhysicsWorld::AddCharacterFromDescriptor(
    ECS::Scene &scene, ECS::Entity entity, const ECS::Transform &transform,
    const CharacterDescriptor &descriptor, const ParentWorldFn &parentWorld)
{
    // A character is placed in world space, and a parented Transform is an offset
    // from its parent — the same mismatch AddBodyFromDescriptor undoes.
    glm::vec3 position = transform.position;
    glm::quat rotation = transform.rotation;
    if (parentWorld)
    {
        if (const glm::mat4 *parent = parentWorld(entity); parent != nullptr)
        {
            const ECS::Transform pose = ECS::PoseUnderParent(transform, *parent);
            position                  = pose.position;
            rotation                  = pose.rotation;
        }
    }

    // A crouch taller than standing is not a crouch, and a step taller than the
    // character is a wall. Both are clamped rather than refused: they arrive from
    // an inspector drag, where a refusal would mean a character that vanishes
    // partway through typing a number.
    const float crouchHalfHeight = glm::min(descriptor.crouchHalfHeight, descriptor.halfHeight);
    const float standingHeight   = CharacterHalfHeight(descriptor.radius, descriptor.halfHeight);
    const float maxStepHeight =
        glm::min(descriptor.maxStepHeight, standingHeight * kMaxStepHeightFraction);

    Impl::CharacterRecord record;
    record.standingShape  = MakeCharacterShape(descriptor.radius, descriptor.halfHeight);
    record.crouchingShape = MakeCharacterShape(descriptor.radius, crouchHalfHeight);

    JPH::CharacterVirtualSettings settings;
    settings.mShape        = record.standingShape;
    settings.mUp           = kCharacterUp;
    settings.mMaxSlopeAngle = glm::radians(descriptor.maxSlopeDegrees);
    settings.mMass          = descriptor.mass;
    settings.mMaxStrength   = descriptor.canPushBodies ? descriptor.pushStrength : 0.f;

    // Only contacts behind this plane hold the character up. At the bottom of
    // the capsule, so a hand brushing a wall at head height is something it
    // collides with rather than something it stands on.
    settings.mSupportingVolume = JPH::Plane(kCharacterUp, -descriptor.radius);

    settings.mPredictiveContactDistance  = kCharacterPredictiveContactDistance;
    settings.mPenetrationRecoverySpeed   = kCharacterPenetrationRecoverySpeed;
    settings.mCharacterPadding           = kCharacterPadding;
    settings.mEnhancedInternalEdgeRemoval = kCharacterEnhancedInternalEdgeRemoval;

    // The inner body is what the rest of the simulation sees: without it a cast
    // passes through the character, a sensor never reports it, and a fast body
    // tunnels through it. It keeps the descriptor's full mask, Trigger included,
    // which is what lets a trigger volume find it.
    settings.mInnerBodyShape = record.standingShape;
    settings.mInnerBodyLayer =
        PackLayer(CollisionFilter{descriptor.collidesWith, CollisionChannel::Character},
                  BodyMotion::Kinematic);

    const std::uint32_t id = _impl->nextCharacterId;

    record.character = new JPH::CharacterVirtual(
        &settings, JPH::RVec3(position.x, position.y, position.z),
        JPH::Quat(rotation.x, rotation.y, rotation.z, rotation.w).Normalized(), id,
        &_impl->physicsSystem);

    if (record.character->GetInnerBodyID().IsInvalid())
    {
        Assisi::Core::Log::Error(
            "PhysicsWorld: failed to create character body (body limit of {} reached?); entity will "
            "not simulate.",
            Impl::kMaxBodies);
        return std::unexpected(PhysicsError::BodyLimit);
    }

    record.entity = entity;

    // The sweep must not be stopped by sensors; the inner body above still is
    // seen by them.
    record.queryFilter = CollisionFilter{descriptor.collidesWith, CollisionChannel::Character}.Without(
        CollisionChannel::Trigger);

    record.jumpSpeed          = descriptor.jumpSpeed;
    record.walkSpeed          = descriptor.walkSpeed;
    record.friction           = descriptor.friction;
    record.stopSpeed          = descriptor.stopSpeed;
    record.groundAcceleration = descriptor.groundAcceleration;
    record.airAcceleration    = descriptor.airAcceleration;
    record.airWishSpeedCap    = descriptor.airWishSpeedCap;
    record.bunnyHopSpeedCap   = descriptor.bunnyHopSpeedCap;
    record.bunnyHop           = descriptor.bunnyHop;
    record.standingEyeHeight  = descriptor.eyeHeight;
    record.crouchEyeHeight    = descriptor.crouchEyeHeight;
    record.eyeSpeed           = descriptor.eyeSpeed;
    record.eyeHeight          = descriptor.eyeHeight;
    record.prevEyeHeight      = descriptor.eyeHeight;
    record.stanceLift         = 2.f * (standingHeight - CharacterHalfHeight(descriptor.radius, crouchHalfHeight));
    record.gravityScale       = descriptor.gravityScale;
    record.coyoteTime         = descriptor.coyoteTime;
    record.jumpBufferTime     = descriptor.jumpBufferTime;
    record.maxStepHeight      = maxStepHeight;
    record.radius             = descriptor.radius;
    record.standingHalfHeight = descriptor.halfHeight;
    record.crouchHalfHeight   = crouchHalfHeight;
    record.canPushBodies      = descriptor.canPushBodies;
    record.canBePushed        = descriptor.canBePushed;

    // Seed both snapshots with the spawn pose so the first interpolated frame,
    // before any step has run, resolves to exactly where it was placed.
    record.snapshot = Impl::MotionSnapshot{position, rotation, position, rotation};

    record.character->SetListener(&_impl->characterContacts);
    record.character->SetCharacterVsCharacterCollision(&_impl->characterVsCharacter);
    _impl->characterVsCharacter.Add(record.character);

    const JPH::BodyID innerBody = record.character->GetInnerBodyID();

    _impl->characters.emplace(id, std::move(record));
    ++_impl->nextCharacterId;
    _impl->entityCharacters[entity] = id;

    // Through the inner body, so a contact or a cast that finds a character can
    // name the entity behind it exactly as it does for a rigid body.
    _impl->bodyEntities[innerBody.GetIndexAndSequenceNumber()] = entity;
    _impl->entityBodies[entity]                                = innerBody;

    // Found now rather than on the first step: a stance asked for before then
    // has to know whether the character is standing on something, or a crouch
    // at spawn is taken for one in the air and lifts the feet off the floor.
    const FilterLayerFilter layerFilter{_impl->characters.at(id).queryFilter};
    _impl->characters.at(id).character->RefreshContacts({}, layerFilter, {}, {}, _impl->tempAlloc);

    const Character character{CharacterState{}, glm::vec3{0.f}, CharacterId{id}, Stance::Standing, false};
    (void)scene.Add<Character>(entity, character);
    return character;
}

void PhysicsWorld::RemoveCharacter(const Character &character)
{
    const auto it = _impl->characters.find(character.id.value);
    if (it == _impl->characters.end())
    {
        return;
    }
    Impl::CharacterRecord &record = it->second;

    // While the entity behind the inner body is still knowable — the same reason
    // RemoveBody builds its Exits before destroying anything.
    const JPH::BodyID innerBody = record.character->GetInnerBodyID();
    _impl->EmitExitsFor(innerBody, _impl->pendingExits);

    _impl->characterVsCharacter.Remove(record.character);

    const ECS::Entity entity = record.entity;
    _impl->bodyEntities.erase(innerBody.GetIndexAndSequenceNumber());
    if (entity != ECS::NullEntity)
    {
        _impl->entityBodies.erase(entity);
        _impl->entityCharacters.erase(entity);
    }

    // Last: the CharacterVirtual owns its inner body and destroys it here, so
    // everything above had to read the id while it still meant something.
    _impl->characters.erase(it);
}

void PhysicsWorld::MoveCharacter(const Character &character, glm::vec3 wishVelocity, bool jump)
{
    Impl::CharacterRecord *record = _impl->FindCharacter(character);
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

void PhysicsWorld::SetCharacterFacing(const Character &character, glm::vec3 forward)
{
    Impl::CharacterRecord *record = _impl->FindCharacter(character);
    if (record == nullptr)
    {
        return;
    }
    record->facing = forward;
}

bool PhysicsWorld::SetCharacterStance(const Character &character, Stance stance)
{
    Impl::CharacterRecord *record = _impl->FindCharacter(character);
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
    record->eyeHeight               -= feetShift;
    record->prevEyeHeight           -= feetShift;
    record->snapshot.prevPosition.y += feetShift;
    record->snapshot.curPosition.y  += feetShift;
    return true;
}

CharacterState PhysicsWorld::GetCharacterState(const Character &character) const
{
    const Impl::CharacterRecord *record = _impl->FindCharacter(character);
    if (record == nullptr)
    {
        return CharacterState{};
    }

    const JPH::CharacterVirtual &virtualCharacter = *record->character;

    CharacterState state;

    const JPH::Vec3 velocity = virtualCharacter.GetLinearVelocity();
    state.velocity           = glm::vec3(velocity.GetX(), velocity.GetY(), velocity.GetZ());

    const JPH::Vec3 normal = virtualCharacter.GetGroundNormal();
    state.groundNormal     = glm::vec3(normal.GetX(), normal.GetY(), normal.GetZ());

    const JPH::Vec3 groundVelocity = virtualCharacter.GetGroundVelocity();
    state.groundVelocity = glm::vec3(groundVelocity.GetX(), groundVelocity.GetY(), groundVelocity.GetZ());

    state.groundEntity      = _impl->EntityFor(virtualCharacter.GetGroundBodyID());
    state.timeSinceGrounded = record->timeSinceGrounded;
    state.eyeHeight         = record->eyeHeight;
    state.stance            = record->stance;
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

float PhysicsWorld::GetCharacterEyeHeight(const Character &character, float alpha) const
{
    const Impl::CharacterRecord *record = _impl->FindCharacter(character);
    if (record == nullptr)
    {
        return 0.f;
    }
    return glm::mix(record->prevEyeHeight, record->eyeHeight, alpha);
}

void PhysicsWorld::SetCharacterTransform(const Character &character, glm::vec3 position,
                                         glm::quat rotation)
{
    Impl::CharacterRecord *record = _impl->FindCharacter(character);
    if (record == nullptr)
    {
        return;
    }

    const JPH::Quat joltRotation =
        JPH::Quat(rotation.x, rotation.y, rotation.z, rotation.w).Normalized();
    record->character->SetPosition(JPH::RVec3(position.x, position.y, position.z));
    record->character->SetRotation(joltRotation);

    // The character moves its inner body only as far as its own bookkeeping —
    // the body interface has to be told, or a cast made before the next step
    // finds the character where it used to be.
    const JPH::BodyID innerBody = record->character->GetInnerBodyID();
    if (!innerBody.IsInvalid())
    {
        _impl->physicsSystem.GetBodyInterface().SetPositionAndRotation(
            innerBody, JPH::RVec3(position.x, position.y, position.z), joltRotation,
            JPH::EActivation::Activate);
    }

    // Collapse both snapshots onto the target, or the next frame blends from
    // where it was and slides across the gap instead of arriving.
    record->snapshot = Impl::MotionSnapshot{position, rotation, position, rotation};

    // What it was touching is about to be wrong. Re-finding it here rather than
    // waiting a step keeps the ground state honest for anything that reads it
    // between the teleport and the next Update.
    const FilterLayerFilter layerFilter{record->queryFilter};
    record->character->RefreshContacts({}, layerFilter, {}, {}, _impl->tempAlloc);
}

void PhysicsWorld::SetCharacterFrozen(const Character &character, bool frozen)
{
    Impl::CharacterRecord *record = _impl->FindCharacter(character);
    if (record == nullptr)
    {
        return;
    }

    record->frozen = frozen;

    // Cleared in both directions: freezing must not leave a jump queued to fire
    // the instant it thaws, and thawing must not resume a velocity earned before
    // the drag that moved it somewhere else entirely.
    record->character->SetLinearVelocity(JPH::Vec3::sZero());
    record->wishVelocity        = glm::vec3(0.f);
    record->jumpBufferRemaining = 0.f;
    record->jumpRequested       = false;
}

// ---------------------------------------------------------------------------
// Physics by entity
// ---------------------------------------------------------------------------

std::expected<void, PhysicsWorld::PhysicsError> PhysicsWorld::RebuildEntityPhysics(
    ECS::Scene &scene, ECS::Entity entity, const ParentWorldFn &parentWorld)
{
    RemoveEntityPhysics(scene, entity);

    const RigidBodyDescriptor  *bodyDescriptor      = scene.Get<RigidBodyDescriptor>(entity);
    const CharacterDescriptor  *characterDescriptor = scene.Get<CharacterDescriptor>(entity);

    if (bodyDescriptor == nullptr && characterDescriptor == nullptr)
    {
        return {};
    }

    // A character already owns a rigid body. Building both would have it collide
    // with its own, so neither is built rather than one silently winning.
    if (bodyDescriptor != nullptr && characterDescriptor != nullptr)
    {
        return std::unexpected(PhysicsError::ConflictingDescriptors);
    }

    const ECS::Transform *transform = scene.Get<ECS::Transform>(entity);
    if (transform == nullptr)
    {
        return std::unexpected(PhysicsError::NoTransform);
    }

    if (characterDescriptor != nullptr)
    {
        const std::expected<Character, PhysicsError> added =
            AddCharacterFromDescriptor(scene, entity, *transform, *characterDescriptor, parentWorld);
        if (!added)
        {
            return std::unexpected(added.error());
        }
        return {};
    }

    const RigidBody body = AddBodyFromDescriptor(scene, entity, *transform, *bodyDescriptor, parentWorld);
    if (!body.bodyId.IsValid())
    {
        return std::unexpected(PhysicsError::BodyLimit);
    }
    return {};
}

void PhysicsWorld::RemoveEntityPhysics(ECS::Scene &scene, ECS::Entity entity)
{
    // Resolved through this world's own maps rather than through the scene: a
    // play-session teardown defers component destruction, so the handle
    // component may already be gone while the object is still here.
    if (const auto it = _impl->entityCharacters.find(entity); it != _impl->entityCharacters.end())
    {
        RemoveCharacter(
            Character{CharacterState{}, glm::vec3{0.f}, CharacterId{it->second}, Stance::Standing, false});
    }
    else if (const auto body = _impl->entityBodies.find(entity); body != _impl->entityBodies.end())
    {
        RemoveBody(RigidBody{FromJolt(body->second)});
    }

    if (!scene.IsAlive(entity))
    {
        return;
    }
    scene.Remove<Character>(entity);
    scene.Remove<RigidBody>(entity);
}

void PhysicsWorld::ReconfigureEntityPhysics(ECS::Scene &scene, ECS::Entity entity,
                                            const ParentWorldFn &parentWorld)
{
    if (const CharacterDescriptor *descriptor = scene.Get<CharacterDescriptor>(entity))
    {
        const Character *character = scene.Get<Character>(entity);
        if (character == nullptr)
        {
            return;
        }

        // A character's shape, slope and step height are baked into the solver
        // when it is created, so there is nothing to retune in place. Position
        // and stance are carried across; velocity is not, which is invisible in
        // an editor and a small jolt if something edits a descriptor mid-play.
        const Impl::CharacterRecord *record = _impl->FindCharacter(*character);
        if (record == nullptr)
        {
            return;
        }

        const JPH::RVec3 position = record->character->GetPosition();
        const JPH::Quat  rotation = record->character->GetRotation();
        const Stance     stance   = record->stance;

        RemoveEntityPhysics(scene, entity);

        const ECS::Transform *transform = scene.Get<ECS::Transform>(entity);
        if (transform == nullptr)
        {
            return;
        }

        const std::expected<Character, PhysicsError> rebuilt =
            AddCharacterFromDescriptor(scene, entity, *transform, *descriptor, parentWorld);
        if (!rebuilt)
        {
            return;
        }

        SetCharacterTransform(*rebuilt,
                              glm::vec3(position.GetX(), position.GetY(), position.GetZ()),
                              glm::quat(rotation.GetW(), rotation.GetX(), rotation.GetY(), rotation.GetZ()));
        (void)SetCharacterStance(*rebuilt, stance);
        return;
    }

    const RigidBodyDescriptor *descriptor = scene.Get<RigidBodyDescriptor>(entity);
    const RigidBody           *body       = scene.Get<RigidBody>(entity);
    if (descriptor == nullptr || body == nullptr)
    {
        return;
    }

    ReshapeBody(*body, ColliderShapeDesc{.shape       = descriptor->shape,
                                         .halfExtents = descriptor->halfExtents,
                                         .radius      = descriptor->radius,
                                         .halfHeight  = descriptor->halfHeight});
    SetBodyCCD(*body, descriptor->enableCCD);

    // The channel and its mask live on the body's collision layer, which nothing
    // else here writes. Left out, an edit changes the descriptor and not the
    // simulation, and only shows up once something rebuilds the body from the
    // descriptor — a play session later.
    SetBodyCollisionFilter(*body, CollisionFilter{descriptor->collidesWith, descriptor->channel});
}

void PhysicsWorld::SetEntityPhysicsFrozen(ECS::Scene &scene, ECS::Entity entity, bool frozen)
{
    if (const Character *character = scene.Get<Character>(entity))
    {
        SetCharacterFrozen(*character, frozen);
        return;
    }

    const RigidBody *body = scene.Get<RigidBody>(entity);
    if (body == nullptr)
    {
        return;
    }

    if (frozen)
    {
        SetBodyMotionType(*body, BodyMotion::Static);
        return;
    }

    // Back to whatever the descriptor authored, never unconditionally Dynamic:
    // the freeze has to be invisible, including for bodies that were Static all
    // along.
    const RigidBodyDescriptor *descriptor = scene.Get<RigidBodyDescriptor>(entity);
    const bool                 isStatic   = descriptor != nullptr && descriptor->isStatic;
    SetBodyMotionType(*body, isStatic ? BodyMotion::Static : BodyMotion::Dynamic);
}

void PhysicsWorld::SetEntityTransform(ECS::Scene &scene, ECS::Entity entity, glm::vec3 position,
                                      glm::quat rotation)
{
    if (const Character *character = scene.Get<Character>(entity))
    {
        SetCharacterTransform(*character, position, rotation);
        return;
    }
    if (const RigidBody *body = scene.Get<RigidBody>(entity))
    {
        SetBodyTransform(*body, position, rotation);
    }
}

} // namespace Assisi::Physics
