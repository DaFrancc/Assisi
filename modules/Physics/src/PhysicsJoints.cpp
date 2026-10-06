/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file PhysicsJoints.cpp
/// @brief Joints: the Jolt constraints built from the joint components, kept
///        in step with them and with the bodies they join, and broken when
///        pulled too hard.
///
/// A joint component may name a body that does not exist yet, or one rebuilt
/// with a new BodyID by an edit, so a record outlives its constraint: it is
/// built when both bodies are there, taken down before either body is
/// destroyed, and built again from the frame it first captured.

#include "PhysicsInternal.hpp"

#include <Assisi/Core/Assert.hpp>
#include <Assisi/Core/Logger.hpp>

#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace Assisi::Physics
{

namespace
{

/// The spring a Position motor drives its joint to the target with (Hz). Two
/// swings a second reaches a door's target in about half a second.
constexpr float kMotorSpringFrequency = 2.f;

/// Critically damped, so a motor reaches its target without swinging past it.
constexpr float kMotorSpringDamping = 1.f;

/// Below this length an axis has no direction to build a hinge or a rail on.
constexpr float kMinAxisLength = 1e-6f;

/// A normal is crossed with world X unless the axis is within this cosine of
/// X, where the cross product would be too short to normalize; then with Y.
constexpr float kParallelCosine = 0.9f;

/// Where a joint kind sits in a record's key, below the owner's index.
constexpr std::uint32_t kKindBits = 8;

std::size_t KindIndex(JointKind kind)
{
    return static_cast<std::size_t>(kind);
}

std::uint64_t JointKey(ECS::Entity owner, JointKind kind)
{
    return (static_cast<std::uint64_t>(owner.index) << kKindBits) | static_cast<std::uint64_t>(kind);
}

glm::vec3 ToGlm(JPH::Vec3Arg vector)
{
    return glm::vec3(vector.GetX(), vector.GetY(), vector.GetZ());
}

glm::quat ToGlm(JPH::QuatArg rotation)
{
    return glm::quat(rotation.GetW(), rotation.GetX(), rotation.GetY(), rotation.GetZ());
}

/// A unit vector at right angles to the unit @p axis, the same one every time.
glm::vec3 PerpendicularTo(glm::vec3 axis)
{
    const glm::vec3 reference = std::abs(axis.x) < kParallelCosine ? glm::vec3(1.f, 0.f, 0.f) : glm::vec3(0.f, 1.f, 0.f);
    return glm::normalize(glm::cross(axis, reference));
}

bool SpecIsFinite(const JointSpec &spec)
{
    return IsFinite(spec.anchor) && IsFinite(spec.axis) && IsFinite(spec.otherAnchor) && std::isfinite(spec.minLimit) &&
           std::isfinite(spec.maxLimit) && std::isfinite(spec.swing) && std::isfinite(spec.springFrequency) &&
           std::isfinite(spec.springDamping) && std::isfinite(spec.friction) && std::isfinite(spec.motorTarget) &&
           std::isfinite(spec.motorMax);
}

bool KindHasAxis(JointKind kind)
{
    return kind == JointKind::Hinge || kind == JointKind::Slider || kind == JointKind::SwingTwist;
}

/// @p frame placed by a body at @p position turned by @p rotation.
JointFrame Placed(const JointFrame &frame, glm::vec3 position,
                  glm::quat rotation)
{
    return JointFrame{position + rotation * frame.point, rotation *frame.axis,
                      rotation *frame.normal};
}

/// @p frame, in the world, as seen by a body at @p position turned by
/// @p rotation.
JointFrame Unplaced(const JointFrame &frame, glm::vec3 position,
                    glm::quat rotation)
{
    const glm::quat inverse = glm::inverse(rotation);
    return JointFrame{inverse *(frame.point - position), inverse *frame.axis,
                      inverse *frame.normal};
}

JPH::SpringSettings LimitSpring(const JointSpec &spec)
{
    return JPH::SpringSettings(JPH::ESpringMode::FrequencyAndDamping, std::max(spec.springFrequency, 0.f),
                               std::max(spec.springDamping, 0.f));
}

/// The most a motor may push with: unlimited for 0.
float MotorLimit(float authored)
{
    return authored > 0.f ? authored : FLT_MAX;
}

JPH::EMotorState MotorStateOf(const JointSpec &spec)
{
    if (!spec.hasMotor)
    {
        return JPH::EMotorState::Off;
    }
    return spec.motorMode == MotorMode::Position ? JPH::EMotorState::Position : JPH::EMotorState::Velocity;
}

void SetMotorSpring(JPH::MotorSettings &motor)
{
    motor.mSpringSettings = JPH::SpringSettings(JPH::ESpringMode::FrequencyAndDamping, kMotorSpringFrequency,
                                                kMotorSpringDamping);
}

// The fields every joint component has, read the same way from each.
template <typename T> void ReadCommon(const T &joint, JointSpec &spec)
{
    spec.anchor = joint.anchor;
    spec.other = joint.other;
    spec.breakForce = joint.breakForce;
    spec.collideConnected = joint.collideConnected;
}

JointSpec SpecOfFixed(const FixedJoint &joint)
{
    JointSpec spec;
    ReadCommon(joint, spec);
    spec.breakTorque = joint.breakTorque;
    return spec;
}

JointSpec SpecOfPoint(const PointJoint &joint)
{
    JointSpec spec;
    ReadCommon(joint, spec);
    return spec;
}

JointSpec SpecOfHinge(const HingeJoint &joint, const HingeMotor *motor)
{
    JointSpec spec;
    ReadCommon(joint, spec);
    spec.axis = joint.axis;
    spec.minLimit = glm::radians(glm::clamp(joint.minAngle, -180.f, 0.f));
    spec.maxLimit = glm::radians(glm::clamp(joint.maxAngle, 0.f, 180.f));
    spec.springFrequency = joint.limitSpringFrequency;
    spec.springDamping = joint.limitSpringDamping;
    spec.friction = joint.friction;
    spec.breakTorque = joint.breakTorque;
    if (motor != nullptr)
    {
        spec.hasMotor = true;
        spec.motorMode = motor->mode;
        spec.motorTarget = glm::radians(motor->target);
        spec.motorMax = motor->maxTorque;
    }
    return spec;
}

JointSpec SpecOfSlider(const SliderJoint &joint, const SliderMotor *motor)
{
    JointSpec spec;
    ReadCommon(joint, spec);
    spec.axis = joint.axis;
    // Jolt measures a slider from where it was built, so zero is always inside
    // its range.
    spec.minLimit = std::min(joint.minDistance, 0.f);
    spec.maxLimit = std::max(joint.maxDistance, 0.f);
    spec.springFrequency = joint.limitSpringFrequency;
    spec.springDamping = joint.limitSpringDamping;
    spec.friction = joint.friction;
    spec.breakTorque = joint.breakTorque;
    if (motor != nullptr)
    {
        spec.hasMotor = true;
        spec.motorMode = motor->mode;
        spec.motorTarget = motor->target;
        spec.motorMax = motor->maxForce;
    }
    return spec;
}

JointSpec SpecOfDistance(const DistanceJoint &joint)
{
    JointSpec spec;
    ReadCommon(joint, spec);
    spec.otherAnchor = joint.otherAnchor;
    spec.maxLimit = std::max(joint.maxDistance, 0.f);
    spec.minLimit = glm::clamp(joint.minDistance, 0.f, spec.maxLimit);
    spec.springFrequency = joint.limitSpringFrequency;
    spec.springDamping = joint.limitSpringDamping;
    return spec;
}

JointSpec SpecOfSwingTwist(const SwingTwistJoint &joint)
{
    JointSpec spec;
    ReadCommon(joint, spec);
    spec.axis = joint.axis;
    spec.swing = glm::radians(glm::clamp(joint.swingAngle, 0.f, 180.f));
    spec.minLimit = glm::radians(glm::clamp(joint.minTwist, -180.f, 0.f));
    spec.maxLimit = glm::radians(glm::clamp(joint.maxTwist, 0.f, 180.f));
    spec.friction = joint.friction;
    spec.breakTorque = joint.breakTorque;
    return spec;
}

/// Every entity whose @p T changed or went since @p since, into @p changed and
/// @p removed. False when the removal log no longer reaches back that far.
template <typename T>
bool CollectJointChanges(const ECS::Scene &scene, uint64_t since, std::vector<ECS::Entity> &changed,
                         std::vector<ECS::Entity> &removed)
{
    scene.ChangedSince<T>(since, changed);
    return scene.RemovedSince<T>(since, removed);
}

/// Every entity carrying @p T, into @p out.
template <typename T> void CollectAll(const ECS::Scene &scene, std::vector<ECS::Entity> &out)
{
    for (auto [entity, joint] : scene.Query<T>())
    {
        (void)joint;
        out.push_back(entity);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Registering
// ---------------------------------------------------------------------------

std::optional<JointSpec> PhysicsWorld::Impl::SpecOf(const JointRecord &record) const
{
    const ECS::Entity owner = record.owner;
    if (!scene.IsAlive(owner))
    {
        return std::nullopt;
    }
    switch (record.kind)
    {
    case JointKind::Fixed:
        if (const FixedJoint *joint = scene.Get<FixedJoint>(owner); joint != nullptr)
        {
            return SpecOfFixed(*joint);
        }
        break;
    case JointKind::Point:
        if (const PointJoint *joint = scene.Get<PointJoint>(owner); joint != nullptr)
        {
            return SpecOfPoint(*joint);
        }
        break;
    case JointKind::Hinge:
        if (const HingeJoint *joint = scene.Get<HingeJoint>(owner); joint != nullptr)
        {
            return SpecOfHinge(*joint, scene.Get<HingeMotor>(owner));
        }
        break;
    case JointKind::Slider:
        if (const SliderJoint *joint = scene.Get<SliderJoint>(owner); joint != nullptr)
        {
            return SpecOfSlider(*joint, scene.Get<SliderMotor>(owner));
        }
        break;
    case JointKind::Distance:
        if (const DistanceJoint *joint = scene.Get<DistanceJoint>(owner); joint != nullptr)
        {
            return SpecOfDistance(*joint);
        }
        break;
    case JointKind::SwingTwist:
        if (const SwingTwistJoint *joint = scene.Get<SwingTwistJoint>(owner); joint != nullptr)
        {
            return SpecOfSwingTwist(*joint);
        }
        break;
    case JointKind::Count:
        break;
    }
    return std::nullopt;
}

void PhysicsWorld::Impl::RegisterJoints(bool complete)
{
    // One list per kind, in JointKind order.
    std::vector<ECS::Entity> changed[static_cast<std::size_t>(JointKind::Count)];
    std::vector<ECS::Entity> removed;
    bool logComplete = complete;
    logComplete = CollectJointChanges<FixedJoint>(scene, changeCursor, changed[KindIndex(JointKind::Fixed)], removed) && logComplete;
    logComplete = CollectJointChanges<PointJoint>(scene, changeCursor, changed[KindIndex(JointKind::Point)], removed) && logComplete;
    logComplete = CollectJointChanges<HingeJoint>(scene, changeCursor, changed[KindIndex(JointKind::Hinge)], removed) && logComplete;
    logComplete = CollectJointChanges<SliderJoint>(scene, changeCursor, changed[KindIndex(JointKind::Slider)], removed) && logComplete;
    logComplete = CollectJointChanges<DistanceJoint>(scene, changeCursor, changed[KindIndex(JointKind::Distance)], removed) && logComplete;
    logComplete = CollectJointChanges<SwingTwistJoint>(scene, changeCursor, changed[KindIndex(JointKind::SwingTwist)], removed) && logComplete;

    // A motor added, edited or taken off retunes its joint, which the sync
    // below finds by its change tick.
    std::vector<ECS::Entity> motors;
    logComplete = CollectJointChanges<HingeMotor>(scene, changeCursor, motors, motors) && logComplete;
    logComplete = CollectJointChanges<SliderMotor>(scene, changeCursor, motors, motors) && logComplete;

    if (!logComplete)
    {
        // What went is unknown, so every component is listed afresh and every
        // record is checked against the scene below.
        for (std::vector<ECS::Entity> &list : changed)
        {
            list.clear();
        }
        CollectAll<FixedJoint>(scene, changed[KindIndex(JointKind::Fixed)]);
        CollectAll<PointJoint>(scene, changed[KindIndex(JointKind::Point)]);
        CollectAll<HingeJoint>(scene, changed[KindIndex(JointKind::Hinge)]);
        CollectAll<SliderJoint>(scene, changed[KindIndex(JointKind::Slider)]);
        CollectAll<DistanceJoint>(scene, changed[KindIndex(JointKind::Distance)]);
        CollectAll<SwingTwistJoint>(scene, changed[KindIndex(JointKind::SwingTwist)]);
        for (std::pair<const std::uint64_t, JointRecord> &entry : joints)
        {
            removed.push_back(entry.second.owner);
        }
    }

    // A removed component's record goes, as does one an index's earlier life
    // left. Kept when the entity has the component again: a component removed
    // and added back in one frame is still one joint.
    for (const ECS::Entity entity : removed)
    {
        for (std::uint32_t kind = 0; kind < static_cast<std::uint32_t>(JointKind::Count); ++kind)
        {
            const std::map<std::uint64_t, JointRecord>::iterator found =
                joints.find(JointKey(entity, static_cast<JointKind>(kind)));
            if (found == joints.end() || (found->second.owner == entity && SpecOf(found->second).has_value()))
            {
                continue;
            }
            DetachJoint(found->second);
            joints.erase(found);
        }
    }

    for (std::uint32_t kind = 0; kind < static_cast<std::uint32_t>(JointKind::Count); ++kind)
    {
        for (const ECS::Entity entity : changed[kind])
        {
            JointRecord &record = joints[JointKey(entity, static_cast<JointKind>(kind))];
            if (record.owner != entity)
            {
                // A new joint, or an index's new life: nothing carries over.
                DetachJoint(record);
                record = JointRecord{};
                record.owner = entity;
                record.kind = static_cast<JointKind>(kind);
            }
        }
    }

    const bool anything = !removed.empty() || !motors.empty();
    for (const std::vector<ECS::Entity> &list : changed)
    {
        if (anything || !list.empty())
        {
            MarkJointsDirty();
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Building
// ---------------------------------------------------------------------------

void PhysicsWorld::Impl::SyncJoints()
{
    if (!jointsDirty)
    {
        return;
    }
    jointsDirty = false;
    std::vector<std::uint64_t> gone;
    for (std::pair<const std::uint64_t, JointRecord> &entry : joints)
    {
        if (!SpecOf(entry.second).has_value())
        {
            DetachJoint(entry.second);
            gone.push_back(entry.first);
            continue;
        }
        SyncJoint(entry.second);
    }
    for (const std::uint64_t key : gone)
    {
        joints.erase(key);
    }
}

bool PhysicsWorld::Impl::ResolveJointBodies(JointRecord &record, const JointSpec &spec)
{
    const BodySlot *own = SlotFor(record.owner);
    if (own == nullptr)
    {
        // Not built yet, or never will be; either way there is nothing to hold.
        return false;
    }
    if (own->kind != SlotKind::Body)
    {
        if (!record.refusalLogged)
        {
            Core::Log::Error("PhysicsWorld: entity {} (gen {}) has a joint but no body of its own - a collider under "
                             "a RigidBody is part of that body. Put the joint on the RigidBody.",
                             record.owner.index, record.owner.generation);
            record.refusalLogged = true;
        }
        return false;
    }

    ECS::Entity otherBody = ECS::NullEntity;
    if (spec.other != ECS::NullEntity)
    {
        if (!scene.IsAlive(spec.other))
        {
            return false;
        }
        // A piece or a follower is part of its owner's body.
        otherBody = OwnerOf(spec.other);
        const BodySlot *other = SlotFor(otherBody);
        if (other == nullptr)
        {
            return false;
        }
        if (other->kind != SlotKind::Body || otherBody == record.owner)
        {
            if (!record.refusalLogged)
            {
                Core::Log::Error("PhysicsWorld: entity {} (gen {}) has a joint to entity {} (gen {}), which is a "
                                 "character or its own body; a joint joins two bodies.",
                                 record.owner.index, record.owner.generation, spec.other.index,
                                 spec.other.generation);
                record.refusalLogged = true;
            }
            return false;
        }
    }

    // Jolt cannot hold a joint where nothing can move.
    const BodySlot *other = otherBody == ECS::NullEntity ? nullptr : SlotFor(otherBody);
    const bool dynamic = own->motion == BodyMotion::Dynamic || (other != nullptr && other->motion == BodyMotion::Dynamic);
    if (!dynamic)
    {
        return false;
    }
    if (!SpecIsFinite(spec) || (KindHasAxis(record.kind) && glm::length(spec.axis) < kMinAxisLength))
    {
        if (!record.refusalLogged)
        {
            Core::Log::Error("PhysicsWorld: entity {} (gen {}) has a joint with no axis, or a value that is not a "
                             "number; it is not built until that is fixed.",
                             record.owner.index, record.owner.generation);
            record.refusalLogged = true;
        }
        return false;
    }
    record.otherBody = otherBody;
    return true;
}

void PhysicsWorld::Impl::SyncJoint(JointRecord &record)
{
    const JointSpec spec = *SpecOf(record);
    if (!ResolveJointBodies(record, spec))
    {
        DetachJoint(record);
        return;
    }
    record.refusalLogged = false;

    const JPH::BodyID body1 = record.otherBody == ECS::NullEntity ? JPH::BodyID{} : SlotFor(record.otherBody)->body;
    const JPH::BodyID body2 = SlotFor(record.owner)->body;
    const bool centresMoved = record.constraint != nullptr &&
                              (CentreOf(body1) != record.centre1 || CentreOf(body2) != record.centre2);
    const bool frameEdited = !record.captured || spec.anchor != record.capturedAnchor ||
                             spec.axis != record.capturedAxis || spec.otherAnchor != record.capturedOtherAnchor ||
                             spec.other != record.capturedOther;
    if (record.constraint == nullptr || body1 != record.body1 || body2 != record.body2 || frameEdited || centresMoved)
    {
        DetachJoint(record);
        if (frameEdited)
        {
            record.captured = false;
        }
        BuildJoint(record, spec);
    }
    else
    {
        const std::pair<uint64_t, uint64_t> ticks = TicksOf(record);
        if (ticks.first != record.tick || ticks.second != record.motorTick)
        {
            TuneJoint(record, spec);
            physicsSystem.GetBodyInterface().ActivateConstraint(record.constraint);
        }
    }
    SetJointedPair(record, record.constraint != nullptr && !spec.collideConnected);
}

glm::vec3 PhysicsWorld::Impl::CentreOf(const JPH::BodyID &body) const
{
    if (body.IsInvalid())
    {
        return glm::vec3(0.f);
    }
    const JPH::RefConst<JPH::Shape> shape = physicsSystem.GetBodyInterface().GetShape(body);
    return shape == nullptr ? glm::vec3(0.f) : ToGlm(shape->GetCenterOfMass());
}

std::pair<uint64_t, uint64_t> PhysicsWorld::Impl::TicksOf(const JointRecord &record) const
{
    const ECS::Entity owner = record.owner;
    switch (record.kind)
    {
    case JointKind::Fixed:
        return {scene.ChangeTick<FixedJoint>(owner), 0};
    case JointKind::Point:
        return {scene.ChangeTick<PointJoint>(owner), 0};
    case JointKind::Hinge:
        return {scene.ChangeTick<HingeJoint>(owner), scene.ChangeTick<HingeMotor>(owner)};
    case JointKind::Slider:
        return {scene.ChangeTick<SliderJoint>(owner), scene.ChangeTick<SliderMotor>(owner)};
    case JointKind::Distance:
        return {scene.ChangeTick<DistanceJoint>(owner), 0};
    case JointKind::SwingTwist:
        return {scene.ChangeTick<SwingTwistJoint>(owner), 0};
    case JointKind::Count:
        break;
    }
    return {0, 0};
}

void PhysicsWorld::Impl::BuildJoint(JointRecord &record, const JointSpec &spec)
{
    const BodySlot &own = *SlotFor(record.owner);
    const JPH::BodyID body1 = record.otherBody == ECS::NullEntity ? JPH::BodyID{} : SlotFor(record.otherBody)->body;
    const JPH::BodyID body2 = own.body;
    JPH::BodyInterface &bodies = physicsSystem.GetBodyInterface();

    // The owner's end, from the component, scaled with the entity as a
    // collider's offset is.
    const glm::vec3 ownerPosition = ToGlm(JPH::Vec3(bodies.GetPosition(body2)));
    const glm::quat ownerRotation = ToGlm(bodies.GetRotation(body2));
    const glm::vec3 axis = KindHasAxis(record.kind) ? glm::normalize(spec.axis) : glm::vec3(0.f, 1.f, 0.f);
    const JointFrame ownerLocal{spec.anchor * own.worldScale, axis, PerpendicularTo(axis)};
    const JointFrame ownerEnd = Placed(ownerLocal, ownerPosition, ownerRotation);

    glm::vec3 otherPosition{0.f};
    glm::quat otherRotation{1.f, 0.f, 0.f, 0.f};
    if (!body1.IsInvalid())
    {
        otherPosition = ToGlm(JPH::Vec3(bodies.GetPosition(body1)));
        otherRotation = ToGlm(bodies.GetRotation(body1));
    }
    if (!record.captured)
    {
        // The other end starts where the owner's is, so the joint holds the two
        // as they stand; a Distance joint's starts at its own anchor.
        JointFrame otherEnd = ownerEnd;
        if (record.kind == JointKind::Distance)
        {
            otherEnd.point = ownerPosition + ownerRotation * (spec.otherAnchor * own.worldScale);
        }
        record.otherFrame = Unplaced(otherEnd, otherPosition, otherRotation);
        record.capturedAnchor = spec.anchor;
        record.capturedAxis = spec.axis;
        record.capturedOtherAnchor = spec.otherAnchor;
        record.capturedOther = spec.other;
        record.captured = true;
    }
    const JointFrame otherEnd = Placed(record.otherFrame, otherPosition, otherRotation);

    JPH::Ref<JPH::TwoBodyConstraintSettings> settings = MakeJointSettings(record.kind, otherEnd, ownerEnd, spec);
    record.constraint = bodies.CreateConstraint(settings.GetPtr(), body1, body2);
    if (record.constraint == nullptr)
    {
        return;
    }
    physicsSystem.AddConstraint(record.constraint);
    record.body1 = body1;
    record.body2 = body2;
    record.centre1 = CentreOf(body1);
    record.centre2 = CentreOf(body2);
    TuneJoint(record, spec);
    bodies.ActivateConstraint(record.constraint);
}

void PhysicsWorld::Impl::TuneJoint(JointRecord &record, const JointSpec &spec)
{
    const std::pair<uint64_t, uint64_t> ticks = TicksOf(record);
    record.tick = ticks.first;
    record.motorTick = ticks.second;

    switch (record.kind)
    {
    case JointKind::Hinge:
    {
        JPH::HingeConstraint &hinge = static_cast<JPH::HingeConstraint &>(*record.constraint);
        hinge.SetLimits(spec.minLimit, spec.maxLimit);
        hinge.SetLimitsSpringSettings(LimitSpring(spec));
        hinge.SetMaxFrictionTorque(std::max(spec.friction, 0.f));
        SetMotorSpring(hinge.GetMotorSettings());
        hinge.GetMotorSettings().SetTorqueLimit(MotorLimit(spec.motorMax));
        hinge.SetMotorState(MotorStateOf(spec));
        hinge.SetTargetAngularVelocity(spec.motorTarget);
        hinge.SetTargetAngle(spec.motorTarget);
        break;
    }
    case JointKind::Slider:
    {
        JPH::SliderConstraint &slider = static_cast<JPH::SliderConstraint &>(*record.constraint);
        slider.SetLimits(spec.minLimit, spec.maxLimit);
        slider.SetLimitsSpringSettings(LimitSpring(spec));
        slider.SetMaxFrictionForce(std::max(spec.friction, 0.f));
        SetMotorSpring(slider.GetMotorSettings());
        slider.GetMotorSettings().SetForceLimit(MotorLimit(spec.motorMax));
        slider.SetMotorState(MotorStateOf(spec));
        slider.SetTargetVelocity(spec.motorTarget);
        slider.SetTargetPosition(spec.motorTarget);
        break;
    }
    case JointKind::Distance:
    {
        JPH::DistanceConstraint &distance = static_cast<JPH::DistanceConstraint &>(*record.constraint);
        distance.SetDistance(spec.minLimit, spec.maxLimit);
        distance.SetLimitsSpringSettings(LimitSpring(spec));
        break;
    }
    case JointKind::SwingTwist:
    {
        JPH::SwingTwistConstraint &swingTwist = static_cast<JPH::SwingTwistConstraint &>(*record.constraint);
        swingTwist.SetNormalHalfConeAngle(spec.swing);
        swingTwist.SetPlaneHalfConeAngle(spec.swing);
        swingTwist.SetTwistMinAngle(spec.minLimit);
        swingTwist.SetTwistMaxAngle(spec.maxLimit);
        swingTwist.SetMaxFrictionTorque(std::max(spec.friction, 0.f));
        break;
    }
    case JointKind::Fixed:
    case JointKind::Point:
    case JointKind::Count:
        break;
    }
}

JPH::Ref<JPH::TwoBodyConstraintSettings> PhysicsWorld::Impl::MakeJointSettings(JointKind kind, const JointFrame &end1,
                                                                               const JointFrame &end2,
                                                                               const JointSpec &spec)
{
    switch (kind)
    {
    case JointKind::Fixed:
    {
        JPH::FixedConstraintSettings *settings = new JPH::FixedConstraintSettings;
        settings->mAutoDetectPoint = false;
        settings->mPoint1 = ToJolt(end1.point);
        settings->mAxisX1 = ToJoltVector(end1.normal);
        settings->mAxisY1 = ToJoltVector(end1.axis);
        settings->mPoint2 = ToJolt(end2.point);
        settings->mAxisX2 = ToJoltVector(end2.normal);
        settings->mAxisY2 = ToJoltVector(end2.axis);
        return settings;
    }
    case JointKind::Point:
    {
        JPH::PointConstraintSettings *settings = new JPH::PointConstraintSettings;
        settings->mPoint1 = ToJolt(end1.point);
        settings->mPoint2 = ToJolt(end2.point);
        return settings;
    }
    case JointKind::Hinge:
    {
        JPH::HingeConstraintSettings *settings = new JPH::HingeConstraintSettings;
        settings->mPoint1 = ToJolt(end1.point);
        settings->mHingeAxis1 = ToJoltVector(end1.axis);
        settings->mNormalAxis1 = ToJoltVector(end1.normal);
        settings->mPoint2 = ToJolt(end2.point);
        settings->mHingeAxis2 = ToJoltVector(end2.axis);
        settings->mNormalAxis2 = ToJoltVector(end2.normal);
        return settings;
    }
    case JointKind::Slider:
    {
        JPH::SliderConstraintSettings *settings = new JPH::SliderConstraintSettings;
        settings->mAutoDetectPoint = false;
        settings->mPoint1 = ToJolt(end1.point);
        settings->mSliderAxis1 = ToJoltVector(end1.axis);
        settings->mNormalAxis1 = ToJoltVector(end1.normal);
        settings->mPoint2 = ToJolt(end2.point);
        settings->mSliderAxis2 = ToJoltVector(end2.axis);
        settings->mNormalAxis2 = ToJoltVector(end2.normal);
        return settings;
    }
    case JointKind::Distance:
    {
        JPH::DistanceConstraintSettings *settings = new JPH::DistanceConstraintSettings;
        settings->mPoint1 = ToJolt(end1.point);
        settings->mPoint2 = ToJolt(end2.point);
        settings->mMinDistance = spec.minLimit;
        settings->mMaxDistance = spec.maxLimit;
        return settings;
    }
    case JointKind::SwingTwist:
    case JointKind::Count:
        break;
    }
    JPH::SwingTwistConstraintSettings *settings = new JPH::SwingTwistConstraintSettings;
    settings->mPosition1 = ToJolt(end1.point);
    settings->mTwistAxis1 = ToJoltVector(end1.axis);
    settings->mPlaneAxis1 = ToJoltVector(end1.normal);
    settings->mPosition2 = ToJolt(end2.point);
    settings->mTwistAxis2 = ToJoltVector(end2.axis);
    settings->mPlaneAxis2 = ToJoltVector(end2.normal);
    settings->mNormalHalfConeAngle = spec.swing;
    settings->mPlaneHalfConeAngle = spec.swing;
    settings->mTwistMinAngle = spec.minLimit;
    settings->mTwistMaxAngle = spec.maxLimit;
    return settings;
}

// ---------------------------------------------------------------------------
// Taking down
// ---------------------------------------------------------------------------

void PhysicsWorld::Impl::DetachJoint(JointRecord &record)
{
    SetJointedPair(record, false);
    if (record.constraint == nullptr)
    {
        return;
    }
    // Woken while both bodies still exist: a body the joint held still may
    // have fallen asleep, and would hang where it was once let go.
    physicsSystem.GetBodyInterface().ActivateConstraint(record.constraint);
    physicsSystem.RemoveConstraint(record.constraint);
    record.constraint = nullptr;
    record.body1 = JPH::BodyID{};
    record.body2 = JPH::BodyID{};
}

void PhysicsWorld::Impl::DetachJointsTouching(ECS::Entity entity)
{
    if (joints.empty())
    {
        return;
    }
    for (std::pair<const std::uint64_t, JointRecord> &entry : joints)
    {
        JointRecord &record = entry.second;
        if (record.constraint != nullptr && (record.owner == entity || record.otherBody == entity))
        {
            DetachJoint(record);
        }
    }
    jointsDirty = true;
}

void PhysicsWorld::Impl::SetJointedPair(JointRecord &record, bool counted)
{
    ASSISI_ASSERT(!stepping, "PhysicsWorld changed which bodies are joined while the world is stepping");
    if (record.pairCounted == counted || record.otherBody == ECS::NullEntity)
    {
        record.pairCounted = counted && record.otherBody != ECS::NullEntity;
        return;
    }
    const EntityPair pair = PairOf(record.owner, record.otherBody);
    if (counted)
    {
        ++jointedPairs[pair];
    }
    else
    {
        const std::unordered_map<EntityPair, uint32_t, EntityPairHash>::iterator found = jointedPairs.find(pair);
        if (found != jointedPairs.end() && --found->second == 0u)
        {
            jointedPairs.erase(found);
        }
    }
    record.pairCounted = counted;
    RefreshPair(record.owner, record.otherBody);
}

// ---------------------------------------------------------------------------
// Breaking
// ---------------------------------------------------------------------------

std::pair<float, float> PhysicsWorld::Impl::JointLoad(const JointRecord &record, float subStep) const
{
    float force = 0.f;
    float torque = 0.f;
    switch (record.kind)
    {
    case JointKind::Fixed:
    {
        const JPH::FixedConstraint &fixed = static_cast<const JPH::FixedConstraint &>(*record.constraint);
        force = fixed.GetTotalLambdaPosition().Length();
        torque = fixed.GetTotalLambdaRotation().Length();
        break;
    }
    case JointKind::Point:
        force = static_cast<const JPH::PointConstraint &>(*record.constraint).GetTotalLambdaPosition().Length();
        break;
    case JointKind::Hinge:
    {
        // A limit holds the turn the hinge would otherwise make, so it is
        // borne as torque: a door slammed against its stop can break there.
        const JPH::HingeConstraint &hinge = static_cast<const JPH::HingeConstraint &>(*record.constraint);
        const JPH::Vector<2> rotation = hinge.GetTotalLambdaRotation();
        const float limit = hinge.GetTotalLambdaRotationLimits();
        force = hinge.GetTotalLambdaPosition().Length();
        torque = std::sqrt(rotation[0] * rotation[0] + rotation[1] * rotation[1] + limit * limit);
        break;
    }
    case JointKind::Slider:
    {
        const JPH::SliderConstraint &slider = static_cast<const JPH::SliderConstraint &>(*record.constraint);
        const JPH::Vector<2> position = slider.GetTotalLambdaPosition();
        const float limit = slider.GetTotalLambdaPositionLimits();
        force = std::sqrt(position[0] * position[0] + position[1] * position[1] + limit * limit);
        torque = slider.GetTotalLambdaRotation().Length();
        break;
    }
    case JointKind::Distance:
        force = std::abs(static_cast<const JPH::DistanceConstraint &>(*record.constraint).GetTotalLambdaPosition());
        break;
    case JointKind::SwingTwist:
    {
        const JPH::SwingTwistConstraint &swingTwist =
            static_cast<const JPH::SwingTwistConstraint &>(*record.constraint);
        const float twist = swingTwist.GetTotalLambdaTwist();
        const float swingY = swingTwist.GetTotalLambdaSwingY();
        const float swingZ = swingTwist.GetTotalLambdaSwingZ();
        force = swingTwist.GetTotalLambdaPosition().Length();
        torque = std::sqrt(twist * twist + swingY * swingY + swingZ * swingZ);
        break;
    }
    case JointKind::Count:
        break;
    }
    // A lambda is the impulse of one collision step.
    return {force / subStep, torque / subStep};
}

void PhysicsWorld::Impl::BreakJoints(float subStep)
{
    if (joints.empty() || !breaksJoints || subStep <= 0.f)
    {
        return;
    }
    std::vector<std::uint64_t> broken;
    for (std::pair<const std::uint64_t, JointRecord> &entry : joints)
    {
        JointRecord &record = entry.second;
        if (record.constraint == nullptr)
        {
            continue;
        }
        const std::optional<JointSpec> spec = SpecOf(record);
        if (!spec.has_value() || (spec->breakForce <= 0.f && spec->breakTorque <= 0.f))
        {
            continue;
        }
        const std::pair<float, float> load = JointLoad(record, subStep);
        // Compared so that a load that is not a number never breaks anything.
        const bool pulled = spec->breakForce > 0.f && load.first > spec->breakForce;
        const bool twisted = spec->breakTorque > 0.f && load.second > spec->breakTorque;
        if (!pulled && !twisted)
        {
            continue;
        }
        brokenJoints.push_back(JointBroke{.owner = record.owner,
                                          .other = spec->other,
                                          .force = load.first,
                                          .torque = load.second,
                                          .kind = record.kind});
        DetachJoint(record);
        broken.push_back(entry.first);
    }
    for (const std::uint64_t key : broken)
    {
        const std::map<std::uint64_t, JointRecord>::iterator found = joints.find(key);
        RemoveJointComponent(found->second);
        joints.erase(found);
    }
}

void PhysicsWorld::Impl::RemoveJointComponent(const JointRecord &record)
{
    const ECS::Entity owner = record.owner;
    switch (record.kind)
    {
    case JointKind::Fixed:
        (void)scene.Remove<FixedJoint>(owner);
        break;
    case JointKind::Point:
        (void)scene.Remove<PointJoint>(owner);
        break;
    case JointKind::Hinge:
        // The motor requires the hinge, so it goes first.
        (void)scene.Remove<HingeMotor>(owner);
        (void)scene.Remove<HingeJoint>(owner);
        break;
    case JointKind::Slider:
        (void)scene.Remove<SliderMotor>(owner);
        (void)scene.Remove<SliderJoint>(owner);
        break;
    case JointKind::Distance:
        (void)scene.Remove<DistanceJoint>(owner);
        break;
    case JointKind::SwingTwist:
        (void)scene.Remove<SwingTwistJoint>(owner);
        break;
    case JointKind::Count:
        break;
    }
}

// ---------------------------------------------------------------------------
// The public face
// ---------------------------------------------------------------------------

std::span<const JointBroke> PhysicsWorld::BrokenJoints() const
{
    return _impl->brokenJoints;
}

void PhysicsWorld::SetBreaksJoints(bool breaks)
{
    _impl->breaksJoints = breaks;
}

bool PhysicsWorld::BreaksJoints() const
{
    return _impl->breaksJoints;
}

} // namespace Assisi::Physics
