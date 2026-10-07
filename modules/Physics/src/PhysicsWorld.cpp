/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file PhysicsWorld.cpp
/// @brief The world itself: bringing it up, stepping it, and reading and
///        writing one entity's body.
///
/// The rest of PhysicsWorld lives beside this file rather than in it. Keeping
/// bodies in step with the scene is in PhysicsReconcile.cpp, contacts in
/// PhysicsContacts.cpp, scene queries in PhysicsQueries.cpp, characters in
/// PhysicsCharacters.cpp, and the per-step writeback in
/// PhysicsWriteback.cpp; what they share is PhysicsInternal.hpp.

#include "PhysicsInternal.hpp"

#include <Assisi/Core/Assert.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/ECS/Scene.hpp>

#include <Jolt/Core/FPFlushDenormals.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/CompoundShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/ScaleHelpers.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSettings.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Assisi::Physics
{

namespace
{
/* Jolt's BoxShape requires each half extent to be at least its convex radius
   (cDefaultConvexRadius) and asserts below that — reachable from an ordinary
   inspector drag. Clamp silently: a warning here would fire once per drag
   tick. */
JPH::Vec3 ClampedBoxHalfExtents(glm::vec3 halfExtents)
{
    const glm::vec3 clamped = glm::max(halfExtents, glm::vec3(JPH::cDefaultConvexRadius));
    return {clamped.x, clamped.y, clamped.z};
}

/// Closer to 1 than this on every axis, a scale is no scale: wrapping the shape
/// would cost a level of indirection on every collision test for nothing.
constexpr float kUnitScaleTolerance = 1e-6f;

/// The least density a shape is built at (kg/m³). A dynamic body of no mass
/// cannot be simulated, and a density of zero is one inspector drag away.
constexpr float kMinDensity = 1e-3f;

/// The least size, on each axis, of the box a shape with no volume of its own
/// is weighed as (m): a flat triangle mesh has a zero-thickness bounds, and a
/// box of no volume would weigh nothing again.
constexpr float kMinMassBoxSize = 0.1f;

JPH::Ref<JPH::ConvexShape> MakePrimitive(const Collider &collider)
{
    const float radius = glm::max(collider.radius, JPH::cDefaultConvexRadius);
    const float halfHeight = glm::max(collider.halfHeight, JPH::cDefaultConvexRadius);
    switch (collider.shape)
    {
    case ColliderShape::Sphere:
        return new JPH::SphereShape(radius);
    case ColliderShape::Capsule:
        return new JPH::CapsuleShape(halfHeight, radius);
    case ColliderShape::Cylinder:
        return new JPH::CylinderShape(halfHeight, radius);
    case ColliderShape::Box:
    case ColliderShape::Convex:
    case ColliderShape::Mesh:
    case ColliderShape::Count_:
        break;
    }
    return new JPH::BoxShape(ClampedBoxHalfExtents(collider.halfExtents));
}
} // namespace

JPH::ShapeRefC MakeShape(const Collider &collider, ECS::Entity entity)
{
    const JPH::Ref<JPH::ConvexShape> shape = MakePrimitive(collider);
    shape->SetDensity(glm::max(collider.density, kMinDensity));
    shape->SetUserData(UserDataOf(entity));
    return JPH::ShapeRefC(shape.GetPtr());
}

bool SameShape(const Collider &a, const Collider &b)
{
    return a.shape == b.shape && a.halfExtents == b.halfExtents && a.radius == b.radius &&
           a.halfHeight == b.halfHeight && a.offsetPosition == b.offsetPosition &&
           a.offsetRotation == b.offsetRotation && a.density == b.density && a.collisionAsset == b.collisionAsset &&
           a.collisionPiece == b.collisionPiece;
}

glm::vec3 ClampedShapeScale(ColliderShape shape, glm::vec3 scale)
{
    // The rules each Jolt primitive's MakeScaleValid applies, so the scale this
    // reports is the one the shape is built at.
    const JPH::Vec3 nonZero = JPH::ScaleHelpers::MakeNonZeroScale(JPH::Vec3(scale.x, scale.y, scale.z));
    JPH::Vec3 valid = nonZero;
    switch (shape)
    {
    case ColliderShape::Sphere:
    case ColliderShape::Capsule:
        valid = nonZero.GetSign() * JPH::ScaleHelpers::MakeUniformScale(nonZero.Abs());
        break;
    case ColliderShape::Cylinder:
        valid = nonZero.GetSign() * JPH::ScaleHelpers::MakeUniformScaleXZ(nonZero.Abs());
        break;
    case ColliderShape::Box:
    case ColliderShape::Convex:
    case ColliderShape::Mesh:
    case ColliderShape::Count_:
        break;
    }
    return glm::vec3(valid.GetX(), valid.GetY(), valid.GetZ());
}

JPH::ShapeRefC PlaceShape(const JPH::ShapeRefC &base, const Collider &collider, glm::vec3 scale, glm::vec3 valid)
{
    JPH::ShapeRefC scaled = base;
    if (!glm::all(glm::lessThan(glm::abs(valid - glm::vec3(1.f)), glm::vec3(kUnitScaleTolerance))))
    {
        scaled = new JPH::ScaledShape(base, JPH::Vec3(valid.x, valid.y, valid.z));
    }
    const bool offset = collider.offsetPosition != glm::vec3(0.f) ||
                        collider.offsetRotation != glm::quat(1.f, 0.f, 0.f, 0.f);
    if (!offset)
    {
        return scaled;
    }
    const glm::vec3 position = collider.offsetPosition * scale;
    const glm::quat rotation = glm::normalize(collider.offsetRotation);
    return JPH::RotatedTranslatedShapeSettings(JPH::Vec3(position.x, position.y, position.z),
                                               JPH::Quat(rotation.x, rotation.y, rotation.z, rotation.w), scaled)
           .Create()
           .Get();
}

JPH::MassProperties MassOfShape(const JPH::Shape &shape)
{
    JPH::MassProperties mass = shape.GetMassProperties();
    if (mass.mMass > 0.f)
    {
        return mass;
    }
    const JPH::Vec3 size = JPH::Vec3::sMax(shape.GetLocalBounds().GetSize(), JPH::Vec3::sReplicate(kMinMassBoxSize));
    mass.SetMassAndInertiaOfSolidBox(size, kWaterDensity);
    return mass;
}

std::size_t CookedShapeKeyHash::operator()(const CookedShapeKey &key) const noexcept
{
    std::size_t hash = std::hash<Core::AssetId>{}(key.asset);
    // Boost's combine: the golden-ratio constant spreads each field across the bits.
    constexpr std::size_t kCombine = 0x9E3779B9u;
    for (const std::size_t field : {std::hash<float>{}(key.density), std::hash<std::int32_t>{}(key.piece),
                                    static_cast<std::size_t>(key.shape)})
    {
        hash ^= field + kCombine + (hash << 6u) + (hash >> 2u);
    }
    return hash;
}

JPH::ShapeRefC PhysicsWorld::Impl::MakeColliderShape(const Collider &collider, glm::vec3 scale,
                                                     ECS::Entity entity) const
{
    const JPH::ShapeRefC base = IsModelShape(collider.shape) ? CookedShapeFor(collider) : MakeShape(collider, entity);
    if (base == nullptr)
    {
        return nullptr;
    }
    return PlaceShape(base, collider, scale, ClampedScale(collider, scale));
}

glm::vec3 PhysicsWorld::Impl::ClampedScale(const Collider &collider, glm::vec3 scale) const
{
    if (!IsModelShape(collider.shape))
    {
        return ClampedShapeScale(collider.shape, scale);
    }
    const JPH::ShapeRefC shape = CookedShapeFor(collider);
    if (shape == nullptr)
    {
        return scale;
    }
    // A model's pieces may be round, which take only some scales; the shape
    // knows which.
    const JPH::Vec3 valid = shape->MakeScaleValid(JPH::Vec3(scale.x, scale.y, scale.z));
    return glm::vec3(valid.GetX(), valid.GetY(), valid.GetZ());
}

Collider PhysicsWorld::Impl::AsBuilt(ECS::Entity entity, const Collider &collider, BodyMotion motion) const
{
    if (collider.shape != ColliderShape::Mesh || motion != BodyMotion::Dynamic)
    {
        return collider;
    }
    Core::Log::Error("PhysicsWorld: entity {} (gen {}) has a Mesh collider on a dynamic body; a triangle mesh "
                     "cannot move under forces, so it is built as Convex. Use Convex, or make the body Kinematic.",
                     entity.index, entity.generation);
    Collider built = collider;
    built.shape = ColliderShape::Convex;
    return built;
}

const Collider *PhysicsWorld::Impl::UsableCollider(ECS::Entity entity) const
{
    const Collider *collider = EnabledCollider(scene, entity);
    if (collider == nullptr || (IsModelShape(collider->shape) && CookedShapeFor(*collider) == nullptr))
    {
        return nullptr;
    }
    return collider;
}

ECS::Entity PhysicsWorld::Impl::PieceOf(const JPH::Body &body, const JPH::SubShapeID &subShape) const
{
    const JPH::Shape *root = body.GetShape();
    if (root->GetType() == JPH::EShapeType::Compound)
    {
        JPH::SubShapeID remainder;
        const JPH::CompoundShape *compound = static_cast<const JPH::CompoundShape *>(root);
        const std::uint32_t child = compound->GetSubShapeIndexFromID(subShape, remainder);
        const std::uint32_t named = child < compound->GetNumSubShapes() ? compound->GetCompoundUserData(child) : 0u;
        if (named != 0u && named - 1u < slots.size())
        {
            return ECS::Entity{named - 1u, slots[named - 1u].generation};
        }
    }
    const ECS::Entity piece = EntityOfUserData(root->GetSubShapeUserData(subShape));
    return piece != ECS::NullEntity ? piece : EntityOfUserData(body.GetUserData());
}

// Contact-solver tuning (see the constructor). Rather than brute-forcing high
// step rates, we lean on the same cheap mechanism Unity/Unreal use: speculative
// contacts (a predictive margin that stops a body at a surface within one solve)
// plus a small allowed overlap that resolves gently. Fast free-fallers that
// still slip past the fixed margin are handled per-body via CCD (RigidBody::ccd).
// Jolt defaults: 0.02 m slop, 0.2 Baumgarte, 0.02 m speculative distance, 0.75
// linear-cast threshold.
constexpr float kPenetrationSlop = 0.01f; ///< Allowed resting overlap (meters) — Unity-like contact offset.
constexpr float kSpeculativeContactDist =
    0.05f; ///< Predictive contact margin (meters); catches moderate impacts in one solve.
// CCD (LinearCast) engages once a body moves more than this * its shape's inner
// radius in a step. Below Jolt's 0.75 default so CCD-enabled bodies stop sinking
// at lower speeds (no "floaty" landings), but not so low that they sweep on
// nearly every step: 0.3 keeps sweeps to genuinely fast motion. Only costs CPU
// for bodies with CCD on (RigidBody::ccd), so the perf downside is bounded. For a 1 m
// box (inner radius 0.5) this triggers at ~9 m/s / a ~4 m drop.
constexpr float kLinearCastThreshold = 0.3f;

PhysicsWorld::PhysicsWorld(ECS::Scene &scene, const CollisionSource &collision, uint32_t maxBodies)
{
    /* Impl's first member acquires the shared Jolt runtime, so the library is up
       (allocator/Factory/types) before any of its other members construct. */
    _impl = std::make_unique<Impl>(scene, collision);
    _impl->maxBodies = maxBodies;
    _impl->clearEpoch = scene.ClearEpoch();

    _impl->physicsSystem.Init(maxBodies, 0u, Impl::kMaxBodyPairs, Impl::kMaxContactConstraints,
                              _impl->bpLayerInterface, _impl->objVsBPFilter, _impl->objLayerFilter);

    // Prevent impact penetration the cheap way (see the constant block above):
    // a wider speculative-contact margin lets the solver stop a body at a surface
    // within a single step, and a small allowed overlap keeps resting contacts
    // from jittering. Baumgarte and solver iteration counts stay at Jolt's
    // defaults — a gentle correction is less visible than an aggressive one.
    JPH::PhysicsSettings settings = _impl->physicsSystem.GetPhysicsSettings();
    settings.mPenetrationSlop = kPenetrationSlop;
    settings.mSpeculativeContactDistance = kSpeculativeContactDist;
    settings.mLinearCastThreshold = kLinearCastThreshold;
    _impl->physicsSystem.SetPhysicsSettings(settings);

    /* Gravity: 9.81 m/s² downward (−Y). */
    _impl->physicsSystem.SetGravity(JPH::Vec3(0.f, -9.81f, 0.f));

    // Installed for the world's whole life rather than switched on by whoever
    // wants contacts. Trigger volumes are authored in a level, and a switch that
    // had to be flipped somewhere else to make one work is a switch that gets
    // forgotten, leaving a volume that silently does nothing.
    _impl->physicsSystem.SetContactListener(&_impl->collector);
    _impl->physicsSystem.SetBodyActivationListener(&_impl->activationCollector);

    Assisi::Core::Log::Info("PhysicsWorld: initialized (Jolt).");
}

PhysicsWorld::~PhysicsWorld()
{
    // Characters own inner bodies they destroy themselves, so everything goes
    // before the PhysicsSystem those bodies live in. Impl's members are then
    // destroyed in reverse declaration order, releasing the shared runtime last.
    _impl->DestroyAll();
    _impl.reset();
}

// ---------------------------------------------------------------------------
// Layers
// ---------------------------------------------------------------------------

namespace
{
/// Where the channel and the motion sit in a LayerTable key, above the mask.
constexpr std::uint32_t kLayerKeyChannelShift = 32;
constexpr std::uint32_t kLayerKeyMotionShift = 40;
} // namespace

JPH::ObjectLayer LayerTable::LayerFor(CollisionFilter filter, BodyMotion motion)
{
    const std::uint64_t key = static_cast<std::uint64_t>(filter.collidesWith.bits) |
                              (static_cast<std::uint64_t>(filter.channel) << kLayerKeyChannelShift) |
                              (static_cast<std::uint64_t>(motion) << kLayerKeyMotionShift);
    const std::unordered_map<std::uint64_t, JPH::ObjectLayer>::const_iterator found = _index.find(key);
    if (found != _index.end())
    {
        return found->second;
    }

    const JPH::ObjectLayer layer = static_cast<JPH::ObjectLayer>(_entries.size());
    _entries.push_back(LayerEntry{.mask = filter.collidesWith.bits,
                                  .channelBit = Core::Bitmask<CollisionChannel, std::uint32_t>::Of(filter.channel).bits,
                                  .channel = filter.channel,
                                  .motion = motion,
                                  .trigger = filter.channel == CollisionChannel::Trigger});
    _index.emplace(key, layer);
    return layer;
}

CollisionFilter LayerTable::FilterOf(JPH::ObjectLayer layer) const
{
    const LayerEntry &entry = EntryOf(layer);
    return CollisionFilter{Core::Bitmask<CollisionChannel, std::uint32_t>{entry.mask}, entry.channel};
}

JPH::ObjectLayer PhysicsWorld::Impl::LayerFor(CollisionFilter filter, BodyMotion motion)
{
    ASSISI_ASSERT(!stepping, "PhysicsWorld made a layer while the world is stepping");
    return layers.LayerFor(filter, motion);
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

PhysicsWorld::Impl::BodySlot *PhysicsWorld::Impl::SlotFor(ECS::Entity entity)
{
    if (entity.index >= slots.size())
    {
        return nullptr;
    }
    BodySlot &slot = slots[entity.index];
    if (slot.kind == SlotKind::Empty || slot.generation != entity.generation)
    {
        return nullptr;
    }
    return &slot;
}

const PhysicsWorld::Impl::BodySlot *PhysicsWorld::Impl::SlotFor(ECS::Entity entity) const
{
    if (entity.index >= slots.size())
    {
        return nullptr;
    }
    const BodySlot &slot = slots[entity.index];
    if (slot.kind == SlotKind::Empty || slot.generation != entity.generation)
    {
        return nullptr;
    }
    return &slot;
}

PhysicsWorld::Impl::BodySlot &PhysicsWorld::Impl::SlotAt(ECS::Entity entity)
{
    if (entity.index >= slots.size())
    {
        slots.resize(static_cast<std::size_t>(entity.index) + 1u);
    }
    return slots[entity.index];
}

ECS::Entity PhysicsWorld::Impl::EntityFor(const JPH::BodyID &id) const
{
    if (id.IsInvalid())
    {
        return ECS::NullEntity;
    }
    return EntityOfUserData(physicsSystem.GetBodyInterface().GetUserData(id));
}

JPH::BodyID PhysicsWorld::Impl::BodyFor(ECS::Entity entity) const
{
    const BodySlot *slot = OwnerSlotFor(entity);
    return slot == nullptr ? JPH::BodyID{} : slot->body;
}

void PhysicsWorld::Impl::Follow(std::uint32_t index)
{
    BodySlot &slot = slots[index];
    if (!slot.followed)
    {
        slot.followed = true;
        awake.push_back(index);
    }
}

// ---------------------------------------------------------------------------
// Stepping
// ---------------------------------------------------------------------------

void PhysicsWorld::Reconcile()
{
    ASSISI_ASSERT(!_impl->stepping, "PhysicsWorld::Reconcile called while the world is stepping");
    _impl->ReconcileScene(0.f);
}

void PhysicsWorld::Rebuild()
{
    ASSISI_ASSERT(!_impl->stepping, "PhysicsWorld::Rebuild called while the world is stepping");
    _impl->DestroyAll();
    // From the start of the scene's history, so the reconcile below finds every
    // Collider and Character it holds and builds each one new.
    _impl->changeCursor = 0;
    _impl->ReconcileScene(0.f);
}

void PhysicsWorld::Update(float deltaTime)
{
    ASSISI_ASSERT(!_impl->stepping, "PhysicsWorld::Update called while the world is stepping");

    // The events describe the step about to run, not the one before it — clearing
    // here is what guarantees a consumer sees each one exactly once. Safe without
    // the mutex: no Jolt worker is inside a callback at this point.
    _impl->events.clear();
    _impl->brokenJoints.clear();

    // First, so the step simulates the scene as it is now: bodies for what was
    // added, none for what was destroyed, and every Transform written since the
    // last step already pushed. The Exits of what it destroys join those below.
    _impl->ReconcileScene(deltaTime);

    // Exits recorded when a body was destroyed. They belong to this step: the pair
    // ended when the body went away, and there was no step in between.
    _impl->events.swap(_impl->pendingExits);
    _impl->pendingExits.clear();

    // After the reconcile, so every body a request names is built and placed.
    _impl->ApplyRequests(deltaTime);

    // Everything woken by now is awake for the whole step about to run. No Jolt
    // job is running, so the mutex is not needed.
    _impl->activatedAtStart.swap(_impl->activated);
    std::sort(_impl->activatedAtStart.begin(), _impl->activatedAtStart.end());
    _impl->activatedAtStart.erase(std::unique(_impl->activatedAtStart.begin(), _impl->activatedAtStart.end()),
                                  _impl->activatedAtStart.end());

    ++_impl->step;

    _impl->stepping = true;
    {
        // Denormals flushed to zero on this thread for the step, as the workers
        // have them for their whole lives: the solver divides by values that can
        // otherwise come out denormal rather than zero. See InitWorkerThread.
        const JPH::FPFlushDenormals flushDenormals;

        // Characters first. They are swept rather than solved, so they react to
        // the world as the step found it; whatever they push then has the rest
        // of this step to respond, instead of waiting for the next one.
        _impl->StepCharacters(deltaTime);

        /* This world's own scratch allocator, and the shared thread pool. Both
           are Update() arguments; the pool is shared (one set of workers), the
           allocator is per-world so two worlds' steps never touch the same
           scratch stack (see JoltRuntime). */
        _impl->physicsSystem.Update(deltaTime, _impl->collisionSteps, &_impl->tempAlloc, &_impl->jolt.JobSystem());
    }
    _impl->stepping = false;

    // Before the writeback, so a broken joint's bodies are reported as the
    // step left them and its component is gone by the time anything reads.
    _impl->BreakJoints(deltaTime / static_cast<float>(_impl->collisionSteps));

    // After the solve has moved each base, and before the writeback, so a rider
    // is written where its base now is rather than a step behind it.
    _impl->CarryRiders();

    // Before the contact events, so a system reacting to them reads the
    // Transforms this step left. The followers after the writeback, since
    // their pose is composed from their owners'.
    _impl->WriteBack();
    _impl->PlaceFollowers();
    _impl->ResolveContactEvents();
}

void PhysicsWorld::SetCollisionSteps(int32_t steps)
{
    _impl->collisionSteps = std::clamp(steps, 1, Impl::kMaxCollisionSteps);
}

int32_t PhysicsWorld::GetCollisionSteps() const
{
    return _impl->collisionSteps;
}

// ---------------------------------------------------------------------------
// One entity's body
// ---------------------------------------------------------------------------

bool PhysicsWorld::HasBody(ECS::Entity entity) const
{
    const Impl::BodySlot *slot = _impl->SlotFor(entity);
    return slot != nullptr && (slot->kind == Impl::SlotKind::Body || slot->kind == Impl::SlotKind::Character);
}

void PhysicsWorld::Teleport(ECS::Entity entity, const Pose &pose)
{
    ASSISI_ASSERT(!_impl->stepping, "PhysicsWorld::Teleport called while the world is stepping");
    if (!IsFinite(pose))
    {
        Core::Log::Warn("PhysicsWorld: entity {} (gen {}) was teleported to a pose that is not a number; it stays "
                        "where it is.",
                        entity.index, entity.generation);
        return;
    }

    Impl::BodySlot *slot = _impl->OwnBodySlot(entity);
    if (slot == nullptr)
    {
        return;
    }

    const JPH::Quat rotation =
        JPH::Quat(pose.rotation.x, pose.rotation.y, pose.rotation.z, pose.rotation.w).Normalized();
    const JPH::RVec3 position(pose.position.x, pose.position.y, pose.position.z);
    JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();

    if (slot->kind == Impl::SlotKind::Character)
    {
        Impl::CharacterRecord *record = _impl->FindCharacter(entity);
        record->character->SetPosition(position);
        record->character->SetRotation(rotation);
        record->character->SetLinearVelocity(JPH::Vec3::sZero());
        bodies.SetPositionAndRotation(slot->body, position, rotation, JPH::EActivation::Activate);

        // What it was touching is about to be wrong. Re-finding it here rather
        // than waiting a step keeps the ground state honest for anything that
        // reads it between the teleport and the next Update.
        const FilterLayerFilter layerFilter{_impl->layers, record->queryFilter};
        const Impl::OwnerBodiesFilter bodyFilter{*_impl, entity, /*exceptions=*/ true};
        record->character->RefreshContacts({}, layerFilter, bodyFilter, {}, _impl->tempAlloc);
    }
    else
    {
        const bool isStatic = slot->motion == BodyMotion::Static;
        bodies.SetPositionAndRotation(slot->body, position, rotation,
                                      isStatic ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
        if (!isStatic)
        {
            bodies.SetLinearVelocity(slot->body, JPH::Vec3::sZero());
            bodies.SetAngularVelocity(slot->body, JPH::Vec3::sZero());
        }
    }

    // The Transform is written now rather than left to the writeback, which
    // follows only bodies that are awake: a static or sleeping one would stay
    // where it was. Stamped as this world's own, so it is not pushed back, and
    // written over any write still waiting to be pushed, which this replaces.
    // Then snapped, so it is drawn there at once rather than slid to.
    _impl->WritePose(entity, pose, slot->kind != Impl::SlotKind::Character);
    ECS::SnapTransform(_impl->scene, entity);
}

void PhysicsWorld::ActiveBodies(std::vector<ECS::Entity> &out) const
{
    out.clear();

    JPH::BodyIDVector active;
    _impl->physicsSystem.GetActiveBodies(JPH::EBodyType::RigidBody, active);
    out.reserve(active.size());
    for (const JPH::BodyID &id : active)
    {
        // A follower's body is its owner's business, and the owner's own body
        // is what reports for it.
        const ECS::Entity entity = _impl->EntityFor(id);
        if (entity != ECS::NullEntity && HasBody(entity))
        {
            out.push_back(entity);
        }
    }
}

bool PhysicsWorld::IsBodyActive(ECS::Entity entity) const
{
    const JPH::BodyID id = _impl->BodyFor(entity);
    return !id.IsInvalid() && _impl->physicsSystem.GetBodyInterface().IsActive(id);
}

ECS::Entity PhysicsWorld::BodyOf(ECS::Entity piece) const
{
    return _impl->OwnerSlotFor(piece) != nullptr ? _impl->OwnerOf(piece) : ECS::NullEntity;
}

float PhysicsWorld::Mass(ECS::Entity entity) const
{
    const Impl::BodySlot *slot = _impl->OwnerSlotFor(entity);
    if (slot == nullptr)
    {
        return 0.f;
    }
    if (slot->kind == Impl::SlotKind::Character)
    {
        return _impl->FindCharacter(_impl->OwnerOf(entity))->mass;
    }
    if (slot->motion != BodyMotion::Dynamic)
    {
        return 0.f;
    }
    JPH::BodyLockRead lock(_impl->physicsSystem.GetBodyLockInterface(), slot->body);
    if (!lock.Succeeded())
    {
        return 0.f;
    }
    const float inverseMass = lock.GetBody().GetMotionProperties()->GetInverseMass();
    return inverseMass > 0.f ? 1.f / inverseMass : 0.f;
}

// ---------------------------------------------------------------------------
// Requests
// ---------------------------------------------------------------------------

void PhysicsWorld::Impl::Request(const BodyRequest &request)
{
    ASSISI_ASSERT(!stepping, "PhysicsWorld request made while the world is stepping");
    if (!IsFinite(request.value) || !IsFinite(request.point))
    {
        Core::Log::Warn("PhysicsWorld: a push on entity {} (gen {}) is not a number and is ignored.",
                        request.entity.index, request.entity.generation);
        return;
    }
    requests.push_back(request);
}

void PhysicsWorld::Impl::ApplyRequests(float deltaTime)
{
    for (const BodyRequest &request : requests)
    {
        // A push on a piece or a follower is a push on the body it belongs to.
        const ECS::Entity owner = OwnerOf(request.entity);
        const BodySlot *slot = OwnBodySlot(owner);
        if (slot == nullptr)
        {
            continue;
        }
        if (slot->kind == SlotKind::Character)
        {
            ApplyCharacterRequest(*FindCharacter(owner), request, deltaTime);
        }
        else
        {
            ApplyBodyRequest(owner, *slot, request);
        }
    }
    requests.clear();
}

void PhysicsWorld::Impl::ApplyBodyRequest(ECS::Entity owner, const BodySlot &slot, const BodyRequest &request)
{
    // Only a dynamic body responds to a push, and Jolt asserts on one given to
    // anything else. A static body has no sleep to change.
    if (slot.motion == BodyMotion::Static ||
        (slot.motion != BodyMotion::Dynamic && request.kind != RequestKind::Wake && request.kind != RequestKind::Sleep))
    {
        return;
    }

    JPH::BodyInterface &bodies = physicsSystem.GetBodyInterface();
    const JPH::Vec3 value(request.value.x, request.value.y, request.value.z);
    const JPH::RVec3 point(request.point.x, request.point.y, request.point.z);
    switch (request.kind)
    {
    case RequestKind::Force:
        bodies.AddForce(slot.body, value);
        break;
    case RequestKind::ForceAt:
        bodies.AddForce(slot.body, value, point);
        break;
    case RequestKind::Impulse:
        bodies.AddImpulse(slot.body, value);
        break;
    case RequestKind::ImpulseAt:
        bodies.AddImpulse(slot.body, value, point);
        break;
    case RequestKind::Torque:
        bodies.AddTorque(slot.body, value);
        break;
    case RequestKind::AngularImpulse:
        bodies.AddAngularImpulse(slot.body, value);
        break;
    case RequestKind::Wake:
        bodies.ActivateBody(slot.body);
        break;
    case RequestKind::Sleep:
        bodies.DeactivateBody(slot.body);
        break;
    case RequestKind::Count_:
        return;
    }

    // Followed, so the writeback reports what the request did: the motion it
    // started, or the rest it put the body at.
    Follow(owner.index);
}

void PhysicsWorld::Impl::ApplyCharacterRequest(CharacterRecord &record, const BodyRequest &request,
                                               float deltaTime) const
{
    if (record.mass <= 0.f)
    {
        return;
    }
    const JPH::Vec3 value(request.value.x, request.value.y, request.value.z);
    if (request.kind == RequestKind::Impulse)
    {
        record.push += value / record.mass;
    }
    else if (request.kind == RequestKind::Force)
    {
        record.push += value * (deltaTime / record.mass);
    }
}

void PhysicsWorld::AddForce(ECS::Entity entity, glm::vec3 force)
{
    _impl->Request(Impl::BodyRequest{.value = force, .entity = entity, .kind = Impl::RequestKind::Force});
}

void PhysicsWorld::AddForceAt(ECS::Entity entity, glm::vec3 force, glm::vec3 point)
{
    _impl->Request(
        Impl::BodyRequest{.value = force, .point = point, .entity = entity, .kind = Impl::RequestKind::ForceAt});
}

void PhysicsWorld::AddImpulse(ECS::Entity entity, glm::vec3 impulse)
{
    _impl->Request(Impl::BodyRequest{.value = impulse, .entity = entity, .kind = Impl::RequestKind::Impulse});
}

void PhysicsWorld::AddImpulseAt(ECS::Entity entity, glm::vec3 impulse, glm::vec3 point)
{
    _impl->Request(
        Impl::BodyRequest{.value = impulse, .point = point, .entity = entity, .kind = Impl::RequestKind::ImpulseAt});
}

void PhysicsWorld::AddTorque(ECS::Entity entity, glm::vec3 torque)
{
    _impl->Request(Impl::BodyRequest{.value = torque, .entity = entity, .kind = Impl::RequestKind::Torque});
}

void PhysicsWorld::AddAngularImpulse(ECS::Entity entity, glm::vec3 angularImpulse)
{
    _impl->Request(
        Impl::BodyRequest{.value = angularImpulse, .entity = entity, .kind = Impl::RequestKind::AngularImpulse});
}

void PhysicsWorld::Wake(ECS::Entity entity)
{
    _impl->Request(Impl::BodyRequest{.entity = entity, .kind = Impl::RequestKind::Wake});
}

void PhysicsWorld::Sleep(ECS::Entity entity)
{
    _impl->Request(Impl::BodyRequest{.entity = entity, .kind = Impl::RequestKind::Sleep});
}

void PhysicsWorld::ApplyCorrection(ECS::Entity entity, const Pose &pose, const BodyState &state)
{
    ASSISI_ASSERT(!_impl->stepping, "PhysicsWorld::ApplyCorrection called while the world is stepping");
    if (!IsFinite(pose) || !IsFinite(state.linearVelocity) || !IsFinite(state.angularVelocity))
    {
        Core::Log::Warn("PhysicsWorld: a correction for entity {} (gen {}) is not a number and is ignored.",
                        entity.index, entity.generation);
        return;
    }
    const glm::vec3 linearVelocity = state.linearVelocity;
    const glm::vec3 angularVelocity = state.angularVelocity;
    const bool activate = !state.asleep;

    Impl::BodySlot *slot = _impl->OwnBodySlot(entity);
    if (slot == nullptr)
        return;

    JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();

    if (slot->kind == Impl::SlotKind::Character)
    {
        Impl::CharacterRecord &record = *_impl->FindCharacter(entity);
        const JPH::RVec3 position(pose.position.x, pose.position.y, pose.position.z);
        record.character->SetPosition(position);
        record.character->SetLinearVelocity(JPH::Vec3(linearVelocity.x, linearVelocity.y, linearVelocity.z));
        bodies.SetPosition(slot->body, position, JPH::EActivation::Activate);
        const FilterLayerFilter layerFilter{_impl->layers, record.queryFilter};
        const Impl::OwnerBodiesFilter bodyFilter{*_impl, entity, /*exceptions=*/ true};
        record.character->RefreshContacts({}, layerFilter, bodyFilter, {}, _impl->tempAlloc);
        _impl->WritePose(entity, pose, /*writeRotation=*/ false);
        ECS::SnapTransform(_impl->scene, entity);
        return;
    }

    // A static body can be *placed*, it just has no motion to place it with.
    // Refusing the whole call for one would be a trap: a correction for a body
    // the two ends disagree about the motion type of would silently do nothing,
    // which is the worst available outcome for a peer that is trying to tell us
    // where something is.
    const bool isStatic = slot->motion == BodyMotion::Static;

    // Normalized: a quaternion that crossed a wire is often a hair off unit
    // length, and Jolt asserts IsNormalized() when it rotates with one.
    const JPH::Quat rotation =
        JPH::Quat(pose.rotation.x, pose.rotation.y, pose.rotation.z, pose.rotation.w).Normalized();
    bodies.SetPositionAndRotation(slot->body, JPH::RVec3(pose.position.x, pose.position.y, pose.position.z),
                                  rotation,
                                  (activate && !isStatic) ? JPH::EActivation::Activate
                                                          : JPH::EActivation::DontActivate);

    if (!isStatic)
    {
        // Before the deactivate below, not after: Jolt ignores velocity written
        // to a sleeping body, so zeroing an about-to-sleep body has to happen
        // while it is still awake.
        bodies.SetLinearVelocity(slot->body, JPH::Vec3(linearVelocity.x, linearVelocity.y, linearVelocity.z));
        bodies.SetAngularVelocity(slot->body, JPH::Vec3(angularVelocity.x, angularVelocity.y, angularVelocity.z));

        if (!activate)
            bodies.DeactivateBody(slot->body);
    }

    // Written and snapped now, as Teleport is: a body left asleep would never
    // reach the writeback, and the replication view offset assumes the drawn
    // pose jumps with the correction rather than sliding after it.
    _impl->WritePose(entity, pose, /*writeRotation=*/ true);
    ECS::SnapTransform(_impl->scene, entity);
}

Pose PhysicsWorld::GetBodyPose(ECS::Entity entity) const
{
    const JPH::BodyID id = _impl->BodyFor(entity);
    if (id.IsInvalid())
    {
        return Pose{};
    }
    const JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();
    const JPH::RVec3 pos = bodies.GetPosition(id);
    const JPH::Quat rot = bodies.GetRotation(id);
    return Pose{glm::quat(rot.GetW(), rot.GetX(), rot.GetY(), rot.GetZ()), glm::vec3(pos.GetX(), pos.GetY(), pos.GetZ())};
}

BodyState PhysicsWorld::GetBodyState(ECS::Entity entity) const
{
    const Impl::BodySlot *slot = _impl->OwnerSlotFor(entity);
    if (slot == nullptr || slot->motion == BodyMotion::Static)
    {
        return BodyState{};
    }

    if (slot->kind == Impl::SlotKind::Character)
    {
        const JPH::Vec3 velocity = _impl->FindCharacter(_impl->OwnerOf(entity))->character->GetLinearVelocity();
        return BodyState{.linearVelocity = glm::vec3(velocity.GetX(), velocity.GetY(), velocity.GetZ()),
                         .angularVelocity = glm::vec3(0.f),
                         .asleep = false};
    }

    const JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();
    const JPH::Vec3 lin = bodies.GetLinearVelocity(slot->body);
    const JPH::Vec3 ang = bodies.GetAngularVelocity(slot->body);
    return BodyState{.linearVelocity = glm::vec3(lin.GetX(), lin.GetY(), lin.GetZ()),
                     .angularVelocity = glm::vec3(ang.GetX(), ang.GetY(), ang.GetZ()),
                     .asleep = !bodies.IsActive(slot->body)};
}

bool PhysicsWorld::IsBodyCCDEnabled(ECS::Entity entity) const
{
    const Impl::BodySlot *slot = _impl->OwnerSlotFor(entity);
    if (slot == nullptr || slot->motion == BodyMotion::Static)
    {
        return false;
    }
    return _impl->physicsSystem.GetBodyInterface().GetMotionQuality(slot->body) == JPH::EMotionQuality::LinearCast;
}

glm::vec3 PhysicsWorld::GetColliderScale(ECS::Entity entity) const
{
    // Answered per Collider: a piece or a follower is built at its own scale,
    // not its owner's.
    const Impl::BodySlot *slot = _impl->SlotFor(entity);
    if (slot == nullptr || _impl->MaterialOf(entity) == nullptr)
    {
        return glm::vec3(1.f);
    }
    return _impl->ClampedScale(slot->collider, slot->worldScale);
}

CollisionFilter PhysicsWorld::GetBodyCollisionFilter(ECS::Entity entity) const
{
    // A follower collides as itself, not as its owner.
    const Impl::BodySlot *own = _impl->SlotFor(entity);
    const JPH::BodyID id = own != nullptr && own->kind == Impl::SlotKind::Follower ? own->body : _impl->BodyFor(entity);
    if (id.IsInvalid())
        return CollisionFilter{};
    return _impl->FilterOf(id);
}

void PhysicsWorld::SetGravity(glm::vec3 gravity)
{
    _impl->physicsSystem.SetGravity(JPH::Vec3(gravity.x, gravity.y, gravity.z));

    /* Wake everything that moves so it responds to the new gravity immediately.
       A kinematic body among them ignores gravity and is simply woken for
       nothing, which is cheaper than keeping a second list to spare it. */
    JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();
    for (const Impl::BodySlot &slot : _impl->slots)
    {
        if (slot.kind == Impl::SlotKind::Body && slot.motion != BodyMotion::Static)
        {
            bodies.ActivateBody(slot.body);
        }
    }
}

glm::vec3 PhysicsWorld::GetGravity() const
{
    const JPH::Vec3 g = _impl->physicsSystem.GetGravity();
    return glm::vec3(g.GetX(), g.GetY(), g.GetZ());
}

} // namespace Assisi::Physics
