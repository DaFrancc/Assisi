/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file PhysicsInternal.hpp
/// @brief What the pieces of PhysicsWorld share with each other, and with
///        nothing else.
///
/// PhysicsWorld is one class split across several translation units — bodies,
/// contacts, queries, characters, writeback — because one file holding all of it
/// was long enough that finding anything meant scrolling past everything. They
/// share a pimpl and a set of Jolt filter implementations, and this is where
/// those live.
///
/// **Not an installed header.** It sits in src/ and names Jolt types the public
/// headers deliberately hide; anything outside this module that included it
/// would be reaching past the pimpl.

#include <Assisi/Physics/PhysicsWorld.hpp>

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Geometry/CollisionData.hpp>

#include <Jolt/Jolt.h>

// A sanitized build steps physics on one thread — see JoltRuntime.cpp for why.
#if defined(__SANITIZE_THREAD__)
#define ASSISI_PHYSICS_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define ASSISI_PHYSICS_TSAN 1
#endif
#endif

#include <Jolt/Core/JobSystem.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Collision/Shape/MutableCompoundShape.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Physics/Constraints/TwoBodyConstraint.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Assisi::Physics
{

// ---------------------------------------------------------------------------
// Bodies and entities
// ---------------------------------------------------------------------------
//
// Every body this world builds carries its entity in Jolt's per-body user data,
// a character's inner body included (Jolt copies the character's onto it). The
// packed handle is stored inverted so that NullEntity, all ones, stores as 0 —
// which is also what Jolt reports for a body it cannot lock — and so that
// Entity{0, 0}, the first entity of every scene, does not.

inline JPH::uint64 UserDataOf(ECS::Entity entity)
{
    return ~((static_cast<JPH::uint64>(entity.generation) << 32) | static_cast<JPH::uint64>(entity.index));
}

inline ECS::Entity EntityOfUserData(JPH::uint64 userData)
{
    const JPH::uint64 packed = ~userData;
    return ECS::Entity{static_cast<std::uint32_t>(packed & 0xFFFFFFFFull), static_cast<std::uint32_t>(packed >> 32)};
}

/// Two entities, in either order: one IgnoreCollision exception.
struct EntityPair
{
    JPH::uint64 low = 0;
    JPH::uint64 high = 0;

    friend constexpr bool operator==(EntityPair, EntityPair) = default;
};

inline EntityPair PairOf(ECS::Entity a, ECS::Entity b)
{
    const JPH::uint64 first = UserDataOf(a);
    const JPH::uint64 second = UserDataOf(b);
    return first < second ? EntityPair{first, second} : EntityPair{second, first};
}

struct EntityPairHash
{
    std::size_t operator()(EntityPair pair) const noexcept
    {
        return std::hash<JPH::uint64>{}(pair.low) ^ (std::hash<JPH::uint64>{}(pair.high) << 1u);
    }
};

// ---------------------------------------------------------------------------
// Object / broad-phase layers
// ---------------------------------------------------------------------------

/// The rule both bodies and queries are filtered by: each side's mask must admit
/// the other's channel. One-sided agreement is not enough, so either participant
/// can refuse the pair on its own.
inline bool ChannelsAgree(std::uint32_t maskA, std::uint32_t channelBitA, std::uint32_t maskB,
                          std::uint32_t channelBitB)
{
    return (maskA & channelBitB) != 0u && (maskB & channelBitA) != 0u;
}

/// What one ObjectLayer stands for: everything a filter decision needs about a
/// body, read once when the layer is made rather than on every decision.
struct LayerEntry
{
    std::uint32_t mask = 0;       ///< The channels it collides with.
    std::uint32_t channelBit = 0; ///< Its channel, as the one bit a mask tests.
    CollisionChannel channel = CollisionChannel::World;
    BodyMotion motion = BodyMotion::Static;
    bool trigger = false; ///< On the Trigger channel, so a sensor.
};

/// @brief The ObjectLayers a world has handed out, one per distinct filter and
/// motion its bodies use.
///
/// A body's layer is an index here rather than its filter packed into the
/// layer's bits, because 32 bits cannot hold a 32-bit mask and anything else.
/// Jolt's own ObjectLayerPairFilterTable is the same shape.
///
/// Entries are only ever appended, and only while no step runs: Jolt reads this
/// from its worker jobs during a step, and an append could move the storage
/// under them. They are never freed. A filter is a few checkboxes, so a world
/// meets a few hundred combinations at most, and freeing one would mean finding
/// and re-layering every body still on it.
class LayerTable
{
public:
    /// The layer for @p filter on a body that moves as @p motion, made if no
    /// body has used it yet.
    JPH::ObjectLayer LayerFor(CollisionFilter filter, BodyMotion motion);

    [[nodiscard]] const LayerEntry &EntryOf(JPH::ObjectLayer layer) const { return _entries[layer]; }

    /// The filter @p layer was made for.
    [[nodiscard]] CollisionFilter FilterOf(JPH::ObjectLayer layer) const;

private:
    std::vector<LayerEntry> _entries;

    /// Filter, channel and motion in one key, to the layer made for them.
    std::unordered_map<std::uint64_t, JPH::ObjectLayer> _index;
};

/// @brief Identifies one pair of bodies, whichever order they are named in.
///
/// Two BodyIDs' index+sequence numbers packed into one value, the lower first.
/// A type of its own rather than a bare integer because nothing about it is a
/// number: adding to it, comparing it for order, or handing it to anything
/// expecting a count are all meaningless, and a bare uint64_t invites all three.
///
/// Deliberately **not** a Core::StrongId. That marker declares a type's wire
/// form, and this one has none — it is built from live BodyIDs, which mean
/// nothing outside the process that issued them.
struct PairKey
{
    std::uint64_t value = 0;

    friend constexpr bool operator==(PairKey, PairKey) = default;
};

/// Supplied to the map rather than specializing std::hash, which would mean
/// opening namespace std for a type that never leaves this module.
struct PairKeyHash
{
    std::size_t operator()(PairKey key) const noexcept { return std::hash<std::uint64_t>{}(key.value); }
};

namespace BPLayers
{
static constexpr JPH::BroadPhaseLayer Static(0);
static constexpr JPH::BroadPhaseLayer Moving(1);
static constexpr unsigned int Count = 2;
} // namespace BPLayers

// Maps object layers → broad-phase layers. Bodies that never move get their own
// tree, which is the one Jolt does not rebuild as the simulation runs.
class BPLayerInterface final : public JPH::BroadPhaseLayerInterface
{
public:
    explicit BPLayerInterface(const LayerTable &layers) : _layers(layers) {}

    unsigned int GetNumBroadPhaseLayers() const override { return BPLayers::Count; }

    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override
    {
        return _layers.EntryOf(layer).motion == BodyMotion::Static ? BPLayers::Static : BPLayers::Moving;
    }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char *GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override
    {
        return layer == BPLayers::Static ? "Static" : "Moving";
    }
#endif

private:
    const LayerTable &_layers;
};

// Decides whether an object layer should be tested against a broad-phase layer.
class ObjVsBPFilter final : public JPH::ObjectVsBroadPhaseLayerFilter
{
public:
    explicit ObjVsBPFilter(const LayerTable &layers) : _layers(layers) {}

    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer bpLayer) const override
    {
        if (bpLayer != BPLayers::Static)
            return true;

        // Two bodies that both never move can never begin to touch, so walking
        // the static tree for one of them only ever confirms that.
        const LayerEntry &entry = _layers.EntryOf(layer);
        if (entry.motion == BodyMotion::Static)
            return false;

        // A sensor is kinematic, and Jolt generates no kinematic-vs-static
        // contact unless a body asks for one, which none here does. The walk
        // would find candidates and the narrow phase would discard every one.
        return !entry.trigger;
    }

private:
    const LayerTable &_layers;
};

// Decides whether two object layers should collide at all.
class ObjLayerFilter final : public JPH::ObjectLayerPairFilter
{
public:
    explicit ObjLayerFilter(const LayerTable &layers) : _layers(layers) {}

    bool ShouldCollide(JPH::ObjectLayer layerA, JPH::ObjectLayer layerB) const override
    {
        const LayerEntry &a = _layers.EntryOf(layerA);
        const LayerEntry &b = _layers.EntryOf(layerB);
        if (a.motion == BodyMotion::Static && b.motion == BodyMotion::Static)
            return false;

        // One sensor inside another says nothing about the world: neither is
        // solid, so neither entered anything. Reachable because the default
        // sensor is kinematic, and Jolt does pair a kinematic body with a sensor.
        if (a.trigger && b.trigger)
            return false;

        return ChannelsAgree(a.mask, a.channelBit, b.mask, b.channelBit);
    }

private:
    const LayerTable &_layers;
};

/// Applies the two-way channel rule between one participant and each body a
/// query reaches. A query is filtered exactly as a body is: it finds something
/// only if it admits that thing's channel and that thing admits its channel.
///
/// Unlike ObjLayerFilter this ignores motion entirely. A query is not a body: it
/// has no motion type, and the reasons two static bodies are never paired — they
/// cannot begin to touch — say nothing about whether a ray may hit a wall.
class FilterLayerFilter final : public JPH::ObjectLayerFilter
{
public:
    FilterLayerFilter(const LayerTable &layers, CollisionFilter filter)
        : _layers(layers), _mask(filter.collidesWith.bits),
        _channelBit(Core::Bitmask<CollisionChannel, std::uint32_t>::Of(filter.channel).bits)
    {
    }

    bool ShouldCollide(JPH::ObjectLayer layer) const override
    {
        const LayerEntry &entry = _layers.EntryOf(layer);
        return ChannelsAgree(_mask, _channelBit, entry.mask, entry.channelBit);
    }

private:
    const LayerTable &_layers;
    std::uint32_t _mask;
    std::uint32_t _channelBit;
};

// ---------------------------------------------------------------------------
// Shared Jolt runtime
// ---------------------------------------------------------------------------

/// @brief RAII handle to the process-wide Jolt runtime. The first one
/// constructed brings Jolt up; the last one destroyed tears it down.
///
/// See JoltRuntime.cpp for what "the runtime" is and why it is shared across
/// every PhysicsWorld rather than built per world.
class JoltRuntimeRef
{
public:
    JoltRuntimeRef();
    ~JoltRuntimeRef();

    JoltRuntimeRef(const JoltRuntimeRef &) = delete;
    JoltRuntimeRef &operator=(const JoltRuntimeRef &) = delete;

    // The base type, so the tsan build's single-threaded job system substitutes
    // without every caller caring which one it got.
    [[nodiscard]] JPH::JobSystem &JobSystem() const;
};

// ---------------------------------------------------------------------------
// Character tuning
// ---------------------------------------------------------------------------
//
// Solver tolerances, not game feel. Everything an author tunes is a field on
// Character; these are the numbers only someone debugging the sweep
// itself would touch, so each says what it prevents.

/// The world axis a character stands along. Characters do not model a rotating
/// gravity: slope angles, step heights and jump direction are all measured
/// against this one vector, and a world whose gravity pointed elsewhere would
/// have its characters walking on walls.
inline const JPH::Vec3 kCharacterUp = JPH::Vec3::sAxisY();

/// How far beyond the capsule contacts are predicted (meters). Too small and a
/// wall is discovered only once the capsule is inside it, with no surface left
/// to slide along; too large and the character slides along walls it has not
/// reached.
constexpr float kCharacterPredictiveContactDistance = 0.1f;

/// Fraction of an overlap pushed out per step. At 1 a character that spawns
/// inside geometry is ejected in one step rather than seeping out over several.
constexpr float kCharacterPenetrationRecoverySpeed = 1.f;

/// How far the sweep stays off geometry (meters). Keeps the capsule from coming
/// to rest exactly on a surface, where a contact flickers in and out between
/// steps and the character buzzes.
constexpr float kCharacterPadding = 0.02f;

/// A capsule sliding across two boxes laid edge to edge otherwise catches on the
/// seam: the shared edge is interior to neither box, so it reports a normal
/// facing into the direction of travel. Costs extra work per contact.
constexpr bool kCharacterEnhancedInternalEdgeRemoval = true;

/// Above this rising speed relative to its ground, a character reported as
/// standing has in fact just jumped and not yet left (m/s). Adopting the
/// ground's velocity there would cancel the jump on the very next step.
constexpr float kMaxRisingSpeedWhileGrounded = 0.1f;

/// How much overlap a stance change tolerates, as a multiple of the solver's
/// resting slop. Exactly zero would refuse to stand up while the head is within
/// the tolerance the solver itself allows, so standing would fail in the open.
constexpr float kStanceChangePenetrationSlopFactor = 1.5f;

/// A step taller than this fraction of a character's standing height is a wall,
/// whatever a Character says. Left unclamped, the stair sweep vaults the
/// character up surfaces shorter than itself — over railings, onto tables —
/// which reads as the collision simply not working.
constexpr float kMaxStepHeightFraction = 0.5f;

/// Builds a character's capsule with the **bottom of the shape at the origin**,
/// which is what puts an entity's Transform at its feet: an author stands a
/// character on the floor rather than guessing where its middle is. Jolt's
/// capsule is centred on its origin, so it is lifted by its own half-height.
///
/// Dimensions are clamped like a collider's, so a zeroed field from an inspector
/// drag cannot build a degenerate shape that asserts.
JPH::RefConst<JPH::Shape> MakeCharacterShape(float radius, float halfHeight);

/// Distance from a character's feet to the middle of the capsule
/// @ref MakeCharacterShape builds.
float CharacterHalfHeight(float radius, float halfHeight);

/// Below this horizontal speed friction stops the character outright (m/s).
/// Dividing by a speed this small to keep the direction would amplify rounding
/// into a velocity that wanders.
constexpr float kFrictionMinSpeed = 0.002f;

/// Below this requested speed there is no direction to accelerate along (m/s):
/// normalizing it would divide by nearly zero.
constexpr float kMinWishSpeed = 1.0e-4f;

/// With BunnyHopPolicy::Boost, the share of its forward request a standing
/// character's jump adds, and equally how far over walkSpeed the jump may leave
/// it. Half-Life 2's value for a player who is not sprinting.
constexpr float kJumpBoostStanding = 0.5f;

/// The same for a crouched character. The smaller margin is why back hopping
/// gains more while crouched: more of the speed counts as over the limit.
constexpr float kJumpBoostCrouching = 0.1f;

/// What ground friction leaves of a horizontal @p velocity after @p deltaTime:
/// the speed drops by `max(speed, stopSpeed) * friction * deltaTime` and never
/// below zero, so the direction is kept and the character cannot be pushed
/// backwards by its own friction.
JPH::Vec3 ApplyFriction(JPH::Vec3Arg velocity, float friction, float stopSpeed, float deltaTime);

/// Adds speed along the unit @p wishDirection until the velocity's component
/// along it reaches @p targetSpeed, by at most @p maxGain.
///
/// Only that one component is measured. A velocity already faster than
/// @p targetSpeed in another direction still gains along this one, which is
/// where both the slide of a ground turn and the speed of an air strafe come
/// from; one already at @p targetSpeed along it is returned unchanged, never
/// slowed.
JPH::Vec3 Accelerate(JPH::Vec3Arg velocity, JPH::Vec3Arg wishDirection, float targetSpeed, float maxGain);

/// Scales a horizontal @p velocity down to @p maxSpeed if it is faster, keeping
/// its direction.
JPH::Vec3 LimitSpeed(JPH::Vec3Arg velocity, float maxSpeed);

/// Whether every component is a number. Nothing that fails this is handed to
/// Jolt or written to a component: one NaN in a body spreads to everything it
/// touches within a step.
inline bool IsFinite(glm::vec3 value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

inline bool IsFinite(glm::quat value)
{
    return std::isfinite(value.w) && IsFinite(glm::vec3(value.x, value.y, value.z));
}

inline bool IsFinite(const Pose &pose)
{
    return IsFinite(pose.position) && IsFinite(pose.rotation);
}

inline JPH::RVec3 ToJolt(glm::vec3 position)
{
    return JPH::RVec3(position.x, position.y, position.z);
}

inline JPH::Vec3 ToJoltVector(glm::vec3 vector)
{
    return JPH::Vec3(vector.x, vector.y, vector.z);
}

/// Normalized: a hand-authored or imported rotation is often a hair off unit
/// length, and Jolt asserts IsNormalized() when it rotates with one.
inline JPH::Quat ToJolt(glm::quat rotation)
{
    return JPH::Quat(rotation.x, rotation.y, rotation.z, rotation.w).Normalized();
}

/// Every degree of freedom, as LockedAxis bits.
constexpr uint32_t kAllAxes = (1u << static_cast<uint32_t>(LockedAxis::Count_)) - 1u;

/// The motion a body is built with: static without a RigidBody, and kinematic
/// when every axis is locked, since Jolt cannot simulate a body with no freedom
/// left and one that may not move is moved only by its Transform.
inline BodyMotion MotionOf(const RigidBody *rigidBody)
{
    if (rigidBody == nullptr)
    {
        return BodyMotion::Static;
    }
    if (rigidBody->motion == MotionType::Kinematic || (rigidBody->lockedAxes.bits & kAllAxes) == kAllAxes)
    {
        return BodyMotion::Kinematic;
    }
    return BodyMotion::Dynamic;
}

/// Whether two colliders build the same shape, mass included.
bool SameShape(const Collider &a, const Collider &b);

/// Whether @p shape is built from a model rather than being a primitive.
inline bool IsModelShape(ColliderShape shape)
{
    return shape == ColliderShape::Convex || shape == ColliderShape::Mesh;
}

/// The world matrix @p entity's Transform is relative to, composed from the
/// local poses up its Parent chain: the simulation pose, not the drawn one the
/// propagated world matrices hold. Null for a root.
std::optional<glm::mat4> SimulationParentMatrix(const ECS::Scene &scene, ECS::Entity entity);

/// Builds the Jolt collision shape for a Collider's primitive, unscaled and
/// without its offset, at the collider's density and carrying @p entity in its
/// user data — NullEntity for a shape built for a query. Radii and half-heights
/// are clamped to the convex radius, so a zeroed dimension field — reachable
/// from an inspector drag — cannot create a degenerate, asserting shape.
///
/// Never shared between colliders: the leaf names its own.
JPH::ShapeRefC MakeShape(const Collider &collider, ECS::Entity entity);

/// The scale a primitive is built at for a requested @p scale: as given for a
/// box, one scale on every axis for a sphere or a capsule, and one across the
/// round axes for a cylinder.
glm::vec3 ClampedShapeScale(ColliderShape shape, glm::vec3 scale);

/// How far a clamped scale may differ from the one asked for before the clamp
/// is worth a warning: rounding in a composed parent scale is not a mistake.
constexpr float kScaleClampTolerance = 1e-4f;

/// Places @p base by @p collider's offset at @p scale, wrapping it only in what
/// differs from no scale and no offset.
JPH::ShapeRefC PlaceShape(const JPH::ShapeRefC &base, const Collider &collider, glm::vec3 scale, glm::vec3 valid);

/// The mass a moving body built of @p shape is given: the shape's own, or for
/// a shape with no volume of its own — a triangle mesh on a kinematic body — a
/// solid box of its bounds at the density of water, since Jolt refuses a moving
/// body of no mass.
JPH::MassProperties MassOfShape(const JPH::Shape &shape);

/// The Collider @p entity carries if it is present and enabled.
const Collider *EnabledCollider(const ECS::Scene &scene, ECS::Entity entity);

/// What a world keeps about one shape it built from a model.
struct CookedShapeKey
{
    Core::AssetId asset;
    float density = 0.f;
    std::int32_t piece = kAllCollisionPieces;
    ColliderShape shape = ColliderShape::Convex;

    friend bool operator==(const CookedShapeKey &, const CookedShapeKey &) = default;
};

struct CookedShapeKeyHash
{
    std::size_t operator()(const CookedShapeKey &key) const noexcept;
};

/// A point and two perpendicular directions: one end of a joint.
struct JointFrame
{
    glm::vec3 point{0.f};
    glm::vec3 axis{0.f, 1.f, 0.f};
    glm::vec3 normal{1.f, 0.f, 0.f};
};

/// Everything a joint component says, whichever kind it is. Angles in
/// radians, distances in metres.
struct JointSpec
{
    glm::vec3 anchor{0.f};
    glm::vec3 axis{0.f, 1.f, 0.f};
    glm::vec3 otherAnchor{0.f};
    ECS::Entity other{ECS::NullEntity};
    float minLimit = 0.f;
    float maxLimit = 0.f;
    float swing = 0.f;
    float springFrequency = 0.f;
    float springDamping = 0.f;
    float friction = 0.f;
    float motorTarget = 0.f;
    float motorMax = 0.f;
    float breakForce = 0.f;
    float breakTorque = 0.f;
    MotorMode motorMode = MotorMode::Velocity;
    bool hasMotor = false;
    bool collideConnected = false;
};

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct PhysicsWorld::Impl
{
    /* First member: brings the shared Jolt runtime up before any other member's
       constructor allocates through Jolt, and releases it after they are gone. */
    JoltRuntimeRef jolt;

    Impl(ECS::Scene &owner, const CollisionSource &source) : scene(owner), collision(source) {}

    static constexpr uint32_t kMaxBodyPairs = 65536;
    static constexpr uint32_t kMaxContactConstraints = 10240;

    // Collision substeps per Update(); runtime-adjustable via SetCollisionSteps.
    // Defaults to 1 (a single solve, like Unity/Unreal at their fixed rate);
    // raise it to trade CPU for shallower impact penetration.
    static constexpr int32_t kDefaultCollisionSteps = 1;
    static constexpr int32_t kMaxCollisionSteps = 16;

    /// Before the three filters below, which read it from the step's jobs.
    LayerTable layers;

    BPLayerInterface bpLayerInterface{layers};
    ObjVsBPFilter objVsBPFilter{layers};
    ObjLayerFilter objLayerFilter{layers};

    /// The layer a body on @p filter moving as @p motion is put on. Only
    /// between steps: see LayerTable.
    JPH::ObjectLayer LayerFor(CollisionFilter filter, BodyMotion motion);

    // Per-world scratch for this world's Update() (see JoltRuntime for why it is
    // not shared). Constructed after `jolt`, so the Jolt allocator is installed.
    JPH::TempAllocatorImpl tempAlloc{10u * 1024u * 1024u}; // 10 MiB

    JPH::PhysicsSystem physicsSystem;

    ECS::Scene &scene;
    const CollisionSource &collision;

    // --- Shapes (PhysicsWorld.cpp, and from models PhysicsCook.cpp) ------------
    //
    // A shape built from a model is shared: by every collider naming the same
    // model, piece, kind and density, at any scale, which a wrapper of each
    // collider's own carries. Shared means it cannot name an entity in its
    // leaves as a primitive does; a part built of one is named by its child of
    // the owner's compound instead (see PieceOf).
    //
    // The caches are filled on first use and only read afterwards, between
    // steps, so a const query may fill them.

    /// Every model read so far, or nullopt for one that could not be.
    mutable std::unordered_map<Core::AssetId, std::optional<Geometry::CollisionModel>> models;

    /// Every shape built from a model so far, or null for one that could not be.
    mutable std::unordered_map<CookedShapeKey, JPH::ShapeRefC, CookedShapeKeyHash> cooked;

    /// The edges of each shape built from a model that has been drawn, as
    /// CollisionAssetEdges gives them.
    mutable std::unordered_map<CookedShapeKey, std::vector<glm::vec3>, CookedShapeKeyHash> outlines;

    /// The key @p collider's model shape is cached under.
    static CookedShapeKey CookedKeyOf(const Collider &collider);

    /// The model @p asset names, read on first use, or null.
    const Geometry::CollisionModel *ModelOf(Core::AssetId asset) const;

    /// The shared shape @p collider builds from its model, unscaled and without
    /// its offset, built on first use. Null when it cannot be built; the reason
    /// is logged once.
    JPH::ShapeRefC CookedShapeFor(const Collider &collider) const;

    /// The whole shape @p collider builds at @p scale: its primitive or its
    /// model's shape at the scale it can take, moved and turned by its offset,
    /// the offset scaled with it. A primitive's leaf names @p entity. Null when
    /// a model's shape cannot be built.
    JPH::ShapeRefC MakeColliderShape(const Collider &collider, glm::vec3 scale, ECS::Entity entity) const;

    /// The scale @p collider's shape is built at for a requested @p scale.
    glm::vec3 ClampedScale(const Collider &collider, glm::vec3 scale) const;

    /// @p collider as a body moving as @p motion builds it: a Mesh on a dynamic
    /// body becomes Convex, with an error naming @p entity, since a triangle
    /// mesh has no volume to move under forces with.
    Collider AsBuilt(ECS::Entity entity, const Collider &collider, BodyMotion motion) const;

    /// The Collider @p entity carries if it takes part in the simulation:
    /// present, enabled, and, for a model's shape, buildable.
    const Collider *UsableCollider(ECS::Entity entity) const;

    /// The entity whose Collider made the part @p subShape names on @p body.
    ///
    /// A child of an owner's compound carries the index of its entity, plus
    /// one, in its compound user data, since the 32 bits there cannot hold a
    /// whole handle; the slot at that index gives the generation. Any other
    /// leaf names its entity in its own user data, and a leaf that names none —
    /// a character's capsule, a shared model's piece, a query's shape — is the
    /// body's own. Safe from Jolt's jobs: the slots are not resized during a
    /// step.
    ECS::Entity PieceOf(const JPH::Body &body, const JPH::SubShapeID &subShape) const;

    /// The Transform values this world last wrote to an entity, or found there
    /// when it last pushed them to the body, and the change tick they carried.
    ///
    /// A Transform whose tick differs was written by something else since.
    /// Which fields differ from these says what that write changed: a changed
    /// scale rebuilds the shape, and a character is only placed when its
    /// position changed, since the look systems turn it every frame.
    struct TransformStamp
    {
        glm::quat rotation{1.f, 0.f, 0.f, 0.f};
        glm::vec3 position{0.f};
        glm::vec3 scale{1.f};
        uint64_t tick = 0;
    };

    /// What an entity's slot holds.
    enum class SlotKind : std::uint8_t
    {
        Empty,

        /// A body of the entity's own: a static collider, or a RigidBody built
        /// from its own Collider and its pieces.
        Body,
        Character,

        /// A Collider that is part of its owner's body. It has no body of its
        /// own; its shape is a child of the owner's compound.
        Piece,

        /// A Collider with a kinematic body of its own, put at its entity's
        /// pose after every step. Answered for by its owner.
        Follower,
        Count_,
    };

    /// Everything this world keeps about one entity, at that entity's index.
    ///
    /// Indexed rather than hashed: the reconcile and the writeback visit slots by
    /// entity many times a frame, and entity indices are dense. The generation
    /// beside it says which life of the index the slot belongs to.
    struct BodySlot
    {
        TransformStamp stamp;

        /// The Collider and RigidBody values the body was built or last
        /// retuned from, so an edit can be told apart from a write that changed
        /// nothing physical. The RigidBody is default for a static body. For an
        /// owner with no Collider of its own, the collider its layer and
        /// material were taken from: its first piece's.
        Collider collider;
        RigidBody rigidBody;

        /// The pairs this body is in, as keys into `pairs`. Kept in step with
        /// it by InsertPair and ErasePair alone.
        std::vector<PairKey> pairKeys;

        /// An owner's pieces, in the order they sit in its compound after its
        /// own collider.
        std::vector<ECS::Entity> pieces;

        /// The entity indices up the Parent chain of a Collider's entity, under
        /// which it is listed in `partsBelow`.
        std::vector<std::uint32_t> ancestors;

        /// An owner's shape when it has pieces, kept to move one in place.
        JPH::Ref<JPH::MutableCompoundShape> compound;

        /// The BodyState change tick this world last wrote or pushed. A
        /// different one was written by something else since.
        uint64_t stateTick = 0;

        /// The RigidBody or Character a Piece or a Follower belongs to.
        ECS::Entity owner{ECS::NullEntity};

        /// Where a Follower's body was last put, in world space.
        Pose placed;

        /// The world-space scale the shape was built at.
        glm::vec3 worldScale{1.f};

        /// The body, or a character's inner body. None for a Piece.
        JPH::BodyID body;

        CollisionFilter filter;
        uint32_t generation = 0;

        /// A Piece's index among its owner's compound children.
        uint32_t subShape = 0;
        BodyMotion motion = BodyMotion::Static;
        SlotKind kind = SlotKind::Empty;

        /// Followed by the writeback: woken in some step and not yet written at
        /// rest. See `awake`.
        bool followed = false;

        /// Whether a Body's own Collider is part of its shape. False for an
        /// owner built from its pieces alone.
        bool ownCollider = false;
    };

    std::vector<BodySlot> slots;

    /// Entity indices of the bodies the writeback still follows. A body joins
    /// when Jolt reports it awake after a step and leaves once its Transform
    /// holds its resting pose, so a settled world costs the writeback nothing.
    std::vector<std::uint32_t> awake;

    /// Entity indices of the kinematic bodies swept to a Transform write in the
    /// step about to run, and in the one before. A sweep's velocity carries the
    /// body to its target in one step; one not swept again is stopped there.
    std::vector<std::uint32_t> sweptThisStep;
    std::vector<std::uint32_t> sweptLastStep;

    /// Change tick the next reconcile reads the scene's logs from.
    uint64_t changeCursor = 0;

    /// The scene's clear epoch when this world last reconciled. A different one
    /// means every entity handle in `slots` may name an unrelated entity.
    uint32_t clearEpoch = 0;

    uint32_t maxBodies = 0;
    int32_t collisionSteps = kDefaultCollisionSteps;

    /// True while Jolt is stepping. Nothing may call into the world then: the
    /// bodies are locked by the step, and the contact callbacks are reading them.
    bool stepping = false;

    /// Scratch for the reconcile, kept so a frame that changes nothing allocates
    /// nothing.
    std::vector<ECS::Entity> scratchChanged;
    std::vector<ECS::Entity> scratchRemoved;

    // --- Parts (PhysicsParts.cpp) -----------------------------------------------
    //
    // A Collider below a RigidBody or a Character is a part of it: a Piece of
    // its shape or a Follower riding along. These tables are only changed
    // between steps; Jolt's jobs read them during one to name the part a
    // contact touched and to skip the pairs that must not collide.

    /// Every Collider slot below an entity, by that entity's index: what an edit
    /// to its Parent, its RigidBody, its Character or its Transform reaches.
    /// No child index exists in the scene, and a walk over it per edit would
    /// cost a scan of every Parent.
    std::unordered_map<std::uint32_t, std::vector<ECS::Entity>> partsBelow;

    /// Indices of every Follower slot, which PlaceFollowers visits.
    std::vector<std::uint32_t> followers;

    /// Owners whose body is built, retuned or destroyed at the end of this
    /// reconcile, once every part of theirs has been brought in line; and those
    /// among them whose shape changed and must be built again.
    std::vector<ECS::Entity> ownersToSync;
    std::vector<ECS::Entity> ownersToReshape;

    /// The IgnoreCollision exceptions, by owner.
    std::unordered_set<EntityPair, EntityPairHash> ignoredPairs;

    /// The RigidBody or Character @p entity's slot belongs to, or @p entity
    /// itself for anything else. Safe from Jolt's jobs.
    ECS::Entity OwnerOf(ECS::Entity entity) const;

    /// The slot of the body @p entity answers through: its own Body or
    /// Character, or its owner's for a Piece or a Follower.
    BodySlot *OwnerSlotFor(ECS::Entity entity);
    const BodySlot *OwnerSlotFor(ECS::Entity entity) const;

    /// The slot of @p entity's own Body or Character, or null.
    BodySlot *OwnBodySlot(ECS::Entity entity);

    /// Whether the bodies of @p a and @p b must not touch: the same owner's, or
    /// an IgnoreCollision exception. Takes the bodies' own entities. Safe from
    /// Jolt's jobs.
    bool IgnoresPair(ECS::Entity a, ECS::Entity b) const;

    /// The Collider a contact on @p piece is made of, or null for a character.
    /// Safe from Jolt's jobs.
    const Collider *MaterialOf(ECS::Entity piece) const;

    /// Combines the two touching pieces' friction and restitution into
    /// @p settings: friction by geometric mean, restitution by the larger.
    void CombineMaterials(const JPH::Body &body1, const JPH::Body &body2, const JPH::ContactManifold &manifold,
                          JPH::ContactSettings &settings) const;

    /// Builds, retunes or destroys @p owner's body from its own Collider and its
    /// pieces. @p reshape builds its shape again even when its parts did not
    /// change, for a part whose own shape did.
    void SyncOwner(ECS::Entity owner, bool reshape);

    /// The shape of @p owner's body at @p scale: its own Collider alone, or a
    /// compound of it and every piece, which is kept in @p compound.
    JPH::ShapeRefC MakeOwnerShape(ECS::Entity owner, const Collider *own, std::span<const ECS::Entity> pieces,
                                  glm::vec3 scale, JPH::Ref<JPH::MutableCompoundShape> &compound);

    /// Whether @p own or any of @p pieces is a Mesh, whose shape depends on
    /// whether its body is dynamic.
    bool HoldsMesh(const Collider *own, std::span<const ECS::Entity> pieces) const;

    /// A Piece's `subShape` when its shape could not be built, so it has no
    /// child of the compound to move.
    static constexpr uint32_t kNoChild = ~0u;

    /// The Piece slots below @p owner that belong to it, in entity order.
    void PiecesOf(ECS::Entity owner, std::vector<ECS::Entity> &out) const;

    /// Moves @p piece's child of its owner's compound to where its Transform
    /// and the ones above it now put it, and recomputes the owner's mass.
    void RePlacePiece(ECS::Entity piece);

    /// RePlacePiece for every Piece below @p entity, unless @p entity is an
    /// owner, whose pieces move with it.
    void RePlacePiecesBelow(ECS::Entity entity);

    void CreatePiece(ECS::Entity entity, const Collider &collider, ECS::Entity owner);
    void EditPiece(ECS::Entity entity, const Collider &collider);

    void CreateFollower(ECS::Entity entity, const Collider &collider, ECS::Entity owner);
    void EditFollower(ECS::Entity entity, const Collider &collider);

    /// Puts every Follower whose composed pose or scale changed there.
    void PlaceFollowers();

    /// Lists @p entity under every entity above it in `partsBelow`, replacing
    /// what it was listed under before.
    void RegisterAncestors(ECS::Entity entity);
    void UnregisterAncestors(ECS::Entity entity, BodySlot &slot);

    /// Adds every Collider slot listed below each of @p entities to the end of
    /// @p entities.
    void AppendPartsBelow(std::vector<ECS::Entity> &entities) const;

    /// Drops every IgnoreCollision exception naming @p entity.
    void ForgetIgnoredPairs(ECS::Entity entity);

    /// Every body that answers for @p owner: its own, and its followers'.
    void BodiesOf(ECS::Entity owner, std::vector<JPH::BodyID> &out) const;

    /// Hides every body one owner answers through: its own, and its
    /// followers'. What a query told to ignore an entity skips, and what a
    /// character's own sweep skips, together with the bodies IgnoreCollision
    /// excepted from it.
    ///
    /// Answers on the locked half of the filter, where the body's user data can
    /// be read, so it costs nothing to set up and allocates nothing.
    class OwnerBodiesFilter final : public JPH::BodyFilter
    {
public:
        /// @p exceptions also hides the bodies IgnoreCollision excepted from
        /// @p owner, which is a collision rule and so binds a sweep but not a
        /// query.
        OwnerBodiesFilter(const Impl &impl, ECS::Entity owner, bool exceptions)
            : _impl(impl), _owner(owner), _exceptions(exceptions)
        {
        }

        bool ShouldCollideLocked(const JPH::Body &body) const override
        {
            if (_owner == ECS::NullEntity)
            {
                return true;
            }
            const ECS::Entity entity = EntityOfUserData(body.GetUserData());
            if (_impl.OwnerOf(entity) == _owner)
            {
                return false;
            }
            return !_exceptions || !_impl.IgnoresPair(_owner, entity);
        }

private:
        const Impl &_impl;
        ECS::Entity _owner;
        bool _exceptions;
    };

    // --- Joints (PhysicsJoints.cpp) -------------------------------------------
    //
    // One record per joint component, built into a Jolt constraint once both
    // its bodies exist. The other body is Jolt's first, the owner its second,
    // so an angle or a slide is the owner's, measured against the other.

    struct JointRecord
    {
        JPH::Ref<JPH::TwoBodyConstraint> constraint;

        /// The other end as first built, in the other body's own space, or in
        /// the world's when there is no other body. Kept across rebuilds, so a
        /// body built again does not move the joint's zero; taken again only
        /// when the anchor, the axis or the other body is edited.
        JointFrame otherFrame;

        /// The authored values otherFrame was taken from.
        glm::vec3 capturedAnchor{0.f};
        glm::vec3 capturedAxis{0.f};
        glm::vec3 capturedOtherAnchor{0.f};

        /// Each body's centre of mass in its own space when the constraint was
        /// built. Jolt holds a constraint's ends relative to it, so one that
        /// moves since means the constraint must be built again.
        glm::vec3 centre1{0.f};
        glm::vec3 centre2{0.f};

        ECS::Entity owner{ECS::NullEntity};

        /// The body the other end is on, NullEntity for the world.
        ECS::Entity otherBody{ECS::NullEntity};
        ECS::Entity capturedOther{ECS::NullEntity};

        JPH::BodyID body1;
        JPH::BodyID body2;

        /// The component's and its motor's change ticks when last applied.
        uint64_t tick = 0;
        uint64_t motorTick = 0;

        JointKind kind = JointKind::Fixed;
        bool captured = false;

        /// Whether `jointedPairs` counts this joint's two bodies.
        bool pairCounted = false;

        /// Whether why it cannot be built has been logged, so it is said once.
        bool refusalLogged = false;
    };

    /// By owner index and then kind, so joints are visited in a fixed order.
    std::map<std::uint64_t, JointRecord> joints;

    /// Set when something a joint depends on may have changed: a joint
    /// component or a motor, or a body built, rebuilt, reshaped or destroyed.
    /// A settled scene leaves it clear, and the joints cost nothing.
    bool jointsDirty = false;

    /// The pairs of bodies a joint joins that do not collide, by owner, with
    /// how many joints join each.
    std::unordered_map<EntityPair, uint32_t, EntityPairHash> jointedPairs;

    /// The joints the last Update() broke.
    std::vector<JointBroke> brokenJoints;

    /// See PhysicsWorld::SetBreaksJoints.
    bool breaksJoints = true;

    void MarkJointsDirty() { jointsDirty = jointsDirty || !joints.empty(); }

    /// Adds, refreshes and drops records from the joint components changed and
    /// removed since the last reconcile, or from every one when @p complete is
    /// false.
    void RegisterJoints(bool complete);

    /// Builds, retunes or takes down every joint, when anything it depends on
    /// may have changed.
    void SyncJoints();

    void SyncJoint(JointRecord &record);

    /// What @p record's component says now, or nullopt once it is gone.
    std::optional<JointSpec> SpecOf(const JointRecord &record) const;

    /// The body @p spec's other end is on, or why there is none yet. False
    /// when the joint cannot be built now.
    bool ResolveJointBodies(JointRecord &record, const JointSpec &spec);

    /// Builds @p record's constraint from @p spec, taking its other frame first
    /// if it has none for these authored values.
    void BuildJoint(JointRecord &record, const JointSpec &spec);

    /// Applies @p spec's limits, springs, friction and motor to the built
    /// constraint.
    void TuneJoint(JointRecord &record, const JointSpec &spec);

    /// The Jolt settings for a @p kind joint from @p end1 on the other body to
    /// @p end2 on the owner, both in world space.
    static JPH::Ref<JPH::TwoBodyConstraintSettings> MakeJointSettings(JointKind kind, const JointFrame &end1,
                                                                      const JointFrame &end2, const JointSpec &spec);

    /// @p body's centre of mass in its own space, or zero for the world.
    glm::vec3 CentreOf(const JPH::BodyID &body) const;

    /// The change ticks of @p record's component and of its motor.
    std::pair<uint64_t, uint64_t> TicksOf(const JointRecord &record) const;

    /// Takes @p record's constraint out of the simulation, keeping the record.
    void DetachJoint(JointRecord &record);

    /// DetachJoint for every joint either of whose bodies is @p entity's.
    /// Before the body goes: Jolt holds the bodies of a constraint by pointer.
    void DetachJointsTouching(ECS::Entity entity);

    /// Counts or uncounts @p record's bodies in `jointedPairs`.
    void SetJointedPair(JointRecord &record, bool counted);

    /// Breaks every joint the step just pulled or twisted past its limit.
    /// @p subStep is the length of one collision step.
    void BreakJoints(float subStep);

    /// The force and torque @p record's constraint held its bodies with in the
    /// last collision step of @p subStep.
    std::pair<float, float> JointLoad(const JointRecord &record, float subStep) const;

    /// Removes @p record's component, and its motor first.
    void RemoveJointComponent(const JointRecord &record);

    /// Tells the simulation that whether @p a and @p b may collide changed:
    /// drops what it remembered about the pair and wakes them.
    void RefreshPair(ECS::Entity a, ECS::Entity b);

    // --- Requests (PhysicsWorld.cpp) -------------------------------------------

    struct CharacterRecord;

    /// What a request asks of a body.
    enum class RequestKind : std::uint8_t
    {
        Force,
        ForceAt,
        Impulse,
        ImpulseAt,
        Torque,
        AngularImpulse,
        Wake,
        Sleep,
        Count_,
    };

    /// One gameplay request, waiting for the next step. `point` is read only
    /// by the kinds applied at a point.
    struct BodyRequest
    {
        glm::vec3 value{0.f};
        glm::vec3 point{0.f};
        ECS::Entity entity{ECS::NullEntity};
        RequestKind kind = RequestKind::Force;
    };

    /// In the order they were made, which is the order they are applied in.
    std::vector<BodyRequest> requests;

    /// Queues @p request for the next step.
    void Request(const BodyRequest &request);

    /// Applies every queued request to the bodies the reconcile just brought in
    /// line, and empties the queue. @p deltaTime is the step about to run, over
    /// which a force on a character is turned into a change of velocity.
    void ApplyRequests(float deltaTime);

    /// Applies one request to @p owner's body, held in @p slot.
    void ApplyBodyRequest(ECS::Entity owner, const BodySlot &slot, const BodyRequest &request);

    /// Applies one request to a character: a force or an impulse becomes a
    /// change of velocity, and anything else is ignored.
    void ApplyCharacterRequest(CharacterRecord &record, const BodyRequest &request, float deltaTime) const;

    /// The slot @p entity's index maps to, if it holds that entity.
    BodySlot *SlotFor(ECS::Entity entity);
    const BodySlot *SlotFor(ECS::Entity entity) const;

    /// The slot at @p entity's index, created if the table is short.
    BodySlot &SlotAt(ECS::Entity entity);

    /// The entity a body was built for, or NullEntity for a body this world
    /// does not know.
    ECS::Entity EntityFor(const JPH::BodyID &id) const;

    /// The body @p entity owns — a character's inner body for a character — or
    /// an invalid id if it has none.
    JPH::BodyID BodyFor(ECS::Entity entity) const;

    // --- Reconcile (PhysicsReconcile.cpp) --------------------------------------

    /// See PhysicsWorld::Reconcile. @p stepTime is the step about to run, or 0
    /// when none is, which places kinematic bodies rather than sweeping them.
    void ReconcileScene(float stepTime);

    /// Builds, retunes or destroys @p entity's body, character or part to match
    /// its components and what is above it.
    void SyncEntity(ECS::Entity entity);

    /// What an entity's slot should hold, and for a Piece or a Follower, whose.
    struct WantedSlot
    {
        ECS::Entity owner{ECS::NullEntity};
        SlotKind kind = SlotKind::Empty;
    };

    WantedSlot WantedFor(ECS::Entity entity) const;

    /// SyncOwner for every owner a part of this reconcile asked for, once each.
    void SyncOwners();

    /// Pushes a Transform written by something else to @p entity's body.
    void PushTransform(ECS::Entity entity, float stepTime);

    /// Pushes a BodyState velocity written by something else to @p entity's
    /// body, waking it.
    void PushBodyState(ECS::Entity entity);

    /// Builds @p entity's body as @p shape, on @p face's channel and material,
    /// moving by @p rigidBody unless that is null.
    void CreateBody(ECS::Entity entity, const JPH::ShapeRefC &shape, const Collider &face, const RigidBody *rigidBody);

    /// Applies edits to @p entity's live body: @p shape when not null, @p face's
    /// channel and material, and @p rigidBody. Gaining or losing the RigidBody
    /// builds it again, since a static body has no motion to change, so @p shape
    /// must not be null then.
    void EditBody(ECS::Entity entity, const JPH::ShapeRefC &shape, const Collider &face, const RigidBody *rigidBody);

    /// Builds or retunes a static collider's body.
    void SyncStatic(ECS::Entity entity, const Collider &collider);

    /// Puts the body in @p slot on @p filter moving as @p motion, and makes it a
    /// sensor when the filter is on Trigger.
    void SetBodyFilter(BodySlot &slot, CollisionFilter filter, BodyMotion motion);

    /// Sets a moving body's mass from @p rigidBody: overridden when it names
    /// one, from the shape's volume otherwise.
    void ApplyMass(const JPH::BodyID &body, const RigidBody &rigidBody);

    /// Logs that @p entity's @p collider cannot take @p scale and is built at
    /// the nearest scale it can. Called where a scale is applied, not every step.
    void WarnOnClampedScale(ECS::Entity entity, const Collider &collider, glm::vec3 scale) const;

    void CreateCharacter(ECS::Entity entity, const Character &character);

    /// Retunes @p entity's character from an edited Character, keeping its
    /// velocity, timers and stance. The capsule is rebuilt only when its size
    /// changed, since the solver bakes that in.
    void EditCharacter(ECS::Entity entity, const Character &character);

    /// Destroys whatever the slot at @p index holds, queuing the contact Exits
    /// its departure causes.
    void DestroySlot(std::uint32_t index);

    /// Puts @p entity's BodyState back at rest, so a body built for it later
    /// starts from a standstill rather than from the motion of the one destroyed.
    void ForgetBodyState(ECS::Entity entity);

    /// Destroys every body and character without reporting Exits: what was
    /// touching no longer exists to hear it.
    void DestroyAll();

    /// Records @p entity's current Transform as this world's own: what it last
    /// wrote, or what it has just pushed to the body.
    void StampTransform(ECS::Entity entity);

    /// Starts following the slot at @p index in the writeback.
    void Follow(std::uint32_t index);

    // --- Contact events ------------------------------------------------------

    /// One body pair seen touching during the step Jolt is running.
    ///
    /// Deliberately not an event: the callbacks cannot tell Enter from Stay
    /// without the previous step's pairs, and cannot see Exit at all. They record
    /// what touched; `pairs` turns that into phases once the step is over.
    struct TouchRecord
    {
        glm::vec3 normal{0.f};    ///< Away from body1's surface, as Jolt reports it.
        glm::vec3 point{0.f};     ///< The middle of the contact, in world space.
        glm::vec3 velocity1{0.f}; ///< Pre-solve, so an impact's speed survives the solver.
        glm::vec3 velocity2{0.f};

        /// The entities whose colliders touched, on each body.
        ECS::Entity piece1{ECS::NullEntity};
        ECS::Entity piece2{ECS::NullEntity};
        JPH::BodyID id1;
        JPH::BodyID id2;

        /// How far the two overlap. Of several touches between one pair in one
        /// step, the deepest names the pieces and the point.
        float depth = 0.f;
        bool sensor = false;
    };

    /// What is known about a pair that was touching as of some step.
    ///
    /// `stamp` is the step it was last seen in; the sweep after each step treats
    /// anything older as gone. The velocities are the last ones observed, which is
    /// all an Exit can carry — nothing measured the pair on the step it ended.
    /// The entities are captured when the pair is seen, so an Exit for a body
    /// destroyed since still names who it was.
    struct PairState
    {
        std::uint64_t stamp = 0;
        glm::vec3 normal{0.f};
        glm::vec3 point{0.f};
        glm::vec3 velocity1{0.f};
        glm::vec3 velocity2{0.f};

        /// The entities the two bodies were built for, whose slots list the pair.
        ECS::Entity entity1{ECS::NullEntity};
        ECS::Entity entity2{ECS::NullEntity};

        /// Who each side answers for, and which of its parts touched.
        ECS::Entity owner1{ECS::NullEntity};
        ECS::Entity owner2{ECS::NullEntity};
        ECS::Entity piece1{ECS::NullEntity};
        ECS::Entity piece2{ECS::NullEntity};
        JPH::BodyID id1;
        JPH::BodyID id2;
        float depth = 0.f;
        bool sensor = false;
    };

    /// Written from Jolt's worker jobs during Update(), drained on the main thread
    /// after it. The mutex only guards the append: contacts are rare relative to
    /// the collision work that produced them, so this never becomes the
    /// bottleneck, and per-thread buffers would cost more to merge than they save.
    std::mutex touchMutex;
    std::vector<TouchRecord> touchedThisStep;

    /// Pairs Jolt stopped reporting during the step: they separated, or a body
    /// fell asleep. Guarded by `touchMutex`, written the same way.
    std::vector<PairKey> removedThisStep;

    /// Bodies woken since the last step began, by the step or by anything
    /// else. Guarded by `touchMutex`: Jolt wakes bodies from its jobs.
    std::vector<JPH::BodyID> activated;

    /// The bodies `activated` held when the current step began. Each was awake
    /// for the whole step, so every contact it still has was reported in it.
    std::vector<JPH::BodyID> activatedAtStart;

    /// Every pair currently touching, keyed by both bodies. Survives across steps
    /// — it is the memory that makes Enter, Stay and Exit distinguishable.
    std::unordered_map<PairKey, PairState, PairKeyHash> pairs;

    /// See PhysicsWorld::SetStayEventsReported.
    bool stayReported = false;

    /// Adds a pair to `pairs` and to both its bodies' slots, or finds the one
    /// already there. True when it was added.
    std::pair<PairState *, bool> InsertPair(PairKey key, const TouchRecord &touch);

    /// Removes a pair from `pairs` and from both its bodies' slots.
    void ErasePair(PairKey key);

    /// Emits an Exit for the pair at @p key and forgets it.
    void EndPair(PairKey key);

    /// Ends every pair of @p id's that this step did not see. @p id is a body
    /// that was awake for the whole step.
    void EndUnseenPairsOf(const JPH::BodyID &id);

    /// Exits for pairs whose body was destroyed before the next step could notice.
    std::vector<ContactEvent> pendingExits;

    std::vector<ContactEvent> events;

    /// Counts Update() calls, so a pair's `stamp` says which step last saw it.
    std::uint64_t step = 0;

    /// Orders the two ids so a pair has one key whichever way Jolt reports it.
    static PairKey KeyFor(const JPH::BodyID &a, const JPH::BodyID &b)
    {
        const std::uint32_t ka = a.GetIndexAndSequenceNumber();
        const std::uint32_t kb = b.GetIndexAndSequenceNumber();
        const std::uint32_t lo = ka < kb ? ka : kb;
        const std::uint32_t hi = ka < kb ? kb : ka;
        return PairKey{(static_cast<std::uint64_t>(hi) << 32) | static_cast<std::uint64_t>(lo)};
    }

    /// Records that a pair touched. Called from Jolt's narrow phase — i.e.
    /// *before* the solver runs, which is the whole reason the velocities are
    /// captured here rather than read back afterwards.
    void RecordTouch(const JPH::Body &body1, const JPH::Body &body2, const JPH::ContactManifold &manifold);

    /// Turns the step's touches into events, and ages out the pairs that ended.
    void ResolveContactEvents();

    /// @p touch with its two sides in the order @p state keeps them in.
    static TouchRecord OrientedAs(const TouchRecord &touch, const PairState &state);

    /// Whether @p touch, oriented as @p state, should name the pair's pieces
    /// and point over what @p state already holds from this step: it is
    /// deeper, or as deep with pieces that come first.
    static bool DeeperTouch(const TouchRecord &touch, const PairState &state);

    /// Wakes every body overlapping @p bounds that @p filter would interact with.
    ///
    /// A sleeping body reports no contacts, so a static sensor cannot discover one
    /// that settled before the sensor arrived — there is no step on which the pair
    /// is ever tested — and a body resting on a wall that moved would float
    /// where the wall was. Waking the bodies is what creates that step. Costs one
    /// broad-phase query and a brief loss of rest for whatever was inside.
    void WakeInside(const JPH::AABox &bounds, CollisionFilter filter);

    /// The same, for a body already in the simulation: wakes what its own bounds
    /// enclose, under its own filter.
    void WakeInside(const JPH::BodyID &id);

    /// The world-space bounds of a body, or an empty box if it cannot be read.
    JPH::AABox BoundsOf(const JPH::BodyID &id) const;

    /// The filter a body was created with, read back out of its packed layer.
    CollisionFilter FilterOf(const JPH::BodyID &id) const;

    // --- Queries (PhysicsQueries.cpp) ------------------------------------------

    /// One ray hit, for a ray from @p origin along @p sweep.
    QueryHit RayHit(glm::vec3 origin, glm::vec3 sweep, const JPH::RayCastResult &result) const;

    /// One shape-cast hit, for a cast along @p sweep.
    QueryHit ShapeHit(glm::vec3 sweep, const JPH::ShapeCastResult &result) const;

    /// One overlap hit; its distance is the depth.
    QueryHit OverlapHit(const JPH::CollideShapeResult &result) const;

    /// The piece @p subShape names on @p body, read under the body's lock.
    ECS::Entity PieceStruck(const JPH::BodyID &body, const JPH::SubShapeID &subShape) const;

    /// Sweeps @p collider from @p start along @p sweep into @p hits.
    /// The shape a query with @p collider uses, unscaled, or null when its
    /// model cannot be built. A Mesh is used as its Convex shape.
    JPH::ShapeRefC QueryShapeOf(const Collider &collider) const;

    void CollectShapeCast(const Collider &collider, const Pose &start, glm::vec3 sweep, ECS::Entity ignore,
                          JPH::CastShapeCollector &hits) const;

    /// What @p collider held at @p at overlaps, into @p hits.
    void CollectOverlap(const Collider &collider, const Pose &at, ECS::Entity ignore,
                        JPH::CollideShapeCollector &hits) const;

    /// Appends to @p out one event per side of a pair that has an entity.
    static void EmitPair(const PairState &state, ContactPhase phase, std::vector<ContactEvent> &out);

    /// Emits an Exit for every pair naming @p id and forgets those pairs.
    void EmitExitsFor(const JPH::BodyID &id, std::vector<ContactEvent> &out);

    /// Installed for the world's whole life. A trigger volume is authored data,
    /// and a switch that had to be flipped to make one work is a switch somebody
    /// forgets, leaving a volume that silently does nothing.
    class ContactCollector final : public JPH::ContactListener
    {
public:
        explicit ContactCollector(Impl &owner) : _owner(owner) {}

        // The parts of one owner never touch each other, nor does a pair that
        // IgnoreCollision excepted. Asked once per new pair; a cached pair is
        // not asked again, which is why an exception invalidates the cache.
        JPH::ValidateResult OnContactValidate(const JPH::Body &body1, const JPH::Body &body2, JPH::RVec3Arg baseOffset,
                                              const JPH::CollideShapeResult &result) override
        {
            (void)baseOffset;
            (void)result;
            return _owner.IgnoresPair(EntityOfUserData(body1.GetUserData()), EntityOfUserData(body2.GetUserData()))
                       ? JPH::ValidateResult::RejectAllContactsForThisBodyPair
                       : JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
        }

        void OnContactAdded(const JPH::Body &body1, const JPH::Body &body2, const JPH::ContactManifold &manifold,
                            JPH::ContactSettings &settings) override
        {
            _owner.CombineMaterials(body1, body2, manifold, settings);
            _owner.RecordTouch(body1, body2, manifold);
        }

        // Persisted contacts are recorded exactly like new ones. Which of the two
        // Jolt called says only whether it saw the pair last step, and the pair
        // table already knows that — and knows it in the cases Jolt gets wrong,
        // where a body woke and Jolt reports a contact it never stopped having.
        void OnContactPersisted(const JPH::Body &body1, const JPH::Body &body2, const JPH::ContactManifold &manifold,
                                JPH::ContactSettings &settings) override
        {
            _owner.CombineMaterials(body1, body2, manifold, settings);
            _owner.RecordTouch(body1, body2, manifold);
        }

        // Only says which pair to look at after the step. Jolt calls this when a
        // body falls asleep too, which is not the pair ending, and forbids
        // reading either body because one may already be destroyed, so whether
        // the pair ended is decided after the step, where both can be read.
        void OnContactRemoved(const JPH::SubShapeIDPair &pair) override
        {
            const PairKey key = KeyFor(pair.GetBody1ID(), pair.GetBody2ID());
            const std::lock_guard<std::mutex> lock(_owner.touchMutex);
            _owner.removedThisStep.push_back(key);
        }

private:
        Impl &_owner;
    };

    ContactCollector collector{*this};

    /// Records which bodies wake. A body woken has its contacts tested afresh
    /// by the step after, which is what can tell that one it slept with ended.
    class ActivationCollector final : public JPH::BodyActivationListener
    {
public:
        explicit ActivationCollector(Impl &owner) : _owner(owner) {}

        void OnBodyActivated(const JPH::BodyID &id, JPH::uint64 userData) override
        {
            (void)userData;
            const std::lock_guard<std::mutex> lock(_owner.touchMutex);
            _owner.activated.push_back(id);
        }

        void OnBodyDeactivated(const JPH::BodyID &id, JPH::uint64 userData) override
        {
            (void)id;
            (void)userData;
        }

private:
        Impl &_owner;
    };

    ActivationCollector activationCollector{*this};

    // --- Characters ----------------------------------------------------------

    /// One character: the solver object, the two shapes it switches between,
    /// and the authored numbers its step consults.
    ///
    /// The Character's values are copied in rather than read back from the
    /// scene each step; the reconcile copies them again when it is edited.
    struct CharacterRecord
    {
        /// The change of velocity the forces and impulses asked of it this step
        /// add up to, taken by its next step and then cleared.
        JPH::Vec3 push = JPH::Vec3::sZero();

        /// While riding, the feet in the base's frame, taken after the
        /// character's own sweep and before the bodies are solved.
        JPH::Vec3 localFeet = JPH::Vec3::sZero();

        /// World velocity of the ground the character last stood on, so a
        /// carrier's grip can take up only the speed the carrier has over it.
        JPH::Vec3 lastGroundVelocity = JPH::Vec3::sZero();

        /// The base's rotation when localFeet was taken; the carry turns the
        /// character by how far the base turned since.
        JPH::Quat baseRotation = JPH::Quat::sIdentity();

        JPH::Ref<JPH::CharacterVirtual> character;
        JPH::RefConst<JPH::Shape> standingShape;
        JPH::RefConst<JPH::Shape> crouchingShape;

        /// The way it is looking, from its Transform at the start of the step.
        /// The capsule itself is never turned; only BunnyHopPolicy::Boost reads
        /// this.
        glm::vec3 facing{0.f, 0.f, -1.f};

        ECS::Entity entity{ECS::NullEntity};

        /// The carrier being ridden, or NullEntity. While it is set the
        /// character's velocity is relative to the base, and its sweep sees the
        /// world in the base's frame; see CharacterContacts.
        ECS::Entity base{ECS::NullEntity};

        /// The base's body, so the sweep's callbacks need no lookup.
        JPH::BodyID baseBody;

        /// What the character's sweep may be stopped by. The Character's mask
        /// minus Trigger: a sensor reports a character but must not block it, and
        /// a sweep that stopped at one would be an invisible wall. The inner body
        /// keeps the full mask, which is what lets the sensor see it.
        CollisionFilter queryFilter;

        float jumpSpeed = 0.f;
        float walkSpeed = 0.f;
        float crouchSpeedScale = 0.f;
        float friction = 0.f;
        float stopSpeed = 0.f;
        float groundAcceleration = 0.f;
        float airAcceleration = 0.f;
        float airWishSpeedCap = 0.f;
        float bunnyHopSpeedCap = 0.f;
        float standingEyeHeight = 0.f;
        float crouchEyeHeight = 0.f;
        float eyeSpeed = 0.f;
        float gravityScale = 1.f;
        float coyoteTime = 0.f;
        float jumpBufferTime = 0.f;
        float maxStepHeight = 0.f;
        float radius = 0.f;
        float standingHalfHeight = 0.f;
        float crouchHalfHeight = 0.f;
        float mass = 0.f;

        /// How much taller the standing capsule is than the crouching one (m),
        /// which is how far the feet move when the stance changes in the air.
        float stanceLift = 0.f;

        /// Where the eye is above the feet right now; see
        /// CharacterState::eyeHeight.
        float eyeHeight = 0.f;

        /// Seconds since last standing on walkable ground; what coyote time is
        /// measured against.
        float timeSinceGrounded = 0.f;

        /// Seconds a pending jump request has left before it is forgotten.
        float jumpBufferRemaining = 0.f;

        /// While riding, seconds left before a rider its carrier no longer
        /// holds is let go; see Carrier::graceTime.
        float graceRemaining = 0.f;

        Stance stance = Stance::Standing;

        BunnyHopPolicy bunnyHop = BunnyHopPolicy::Cap;

        /// Set when a jump fires, cleared on landing. Without it the jump buffer
        /// would fire a second jump in the same flight the moment the first one
        /// left the coyote window open.
        bool jumpedSinceGrounded = false;

        /// Set by a riding character's sweep that jumped or ended off its base:
        /// it is carried one last time with this step's motion, then let go.
        bool leavingBase = false;

        /// Riding the body CharacterIntent::base names, which no carrier rule
        /// lets go of.
        bool basedByIntent = false;

        Core::Bitmask<CharacterOption, std::uint8_t> options = AllCharacterOptions;

        /// Copies the tuning a step reads from @p character, leaving the
        /// character's motion, timers and stance as they are.
        void Retune(const Character &character);

        /// The horizontal velocity, relative to the ground, that one step of
        /// intent turns @p velocity into. @p wish is the horizontal request;
        /// @p grounded is standing on walkable ground and not jumping this step.
        JPH::Vec3 Steer(JPH::Vec3Arg velocity, JPH::Vec3Arg wish, bool grounded, float deltaTime) const;

        /// The most horizontal speed a jump may leave the ground with.
        float TakeOffSpeedLimit() const;

        /// What a jump under BunnyHopPolicy::Boost turns the horizontal
        /// @p velocity into, given the horizontal request @p wish.
        JPH::Vec3 BoostTakeOff(JPH::Vec3Arg velocity, JPH::Vec3Arg wish) const;
    };

    /// Keyed by entity index, and ordered rather than hashed: characters are
    /// stepped in this order, and one that depended on the iteration order of an
    /// unordered_map would simulate differently between two runs of the same
    /// level.
    std::map<std::uint32_t, CharacterRecord> characters;

    /// What every character collides against every other one through. A
    /// character is not in the broad phase, so without this registry two of them
    /// would each sweep through a world the other is not part of and walk
    /// straight through each other.
    JPH::CharacterVsCharacterCollisionSimple characterVsCharacter;

    CharacterRecord *FindCharacter(ECS::Entity entity);
    const CharacterRecord *FindCharacter(ECS::Entity entity) const;

    /// Builds the CharacterVirtual for @p record from @p character at a
    /// world-space @p pose. False when the simulation is full.
    bool BuildCharacterVirtual(CharacterRecord &record, const Character &character, const Pose &pose);

    /// Reads every character's CharacterIntent and sweeps it by one step,
    /// before the bodies are solved. See PhysicsWorld::Update.
    void StepCharacters(float deltaTime);

    /// The carrier under @p record that it may ride: the owner of its ground
    /// body, when that is a body with a Carrier and the character
    /// rides bases. NullEntity otherwise.
    ECS::Entity CarrierUnder(const CharacterRecord &record) const;

    /// Whether @p record is inside one of @p carrier's CarrierVolume triggers,
    /// as the last solve left the overlaps.
    bool InsideVolumeOf(const CharacterRecord &record, ECS::Entity carrier) const;

    /// The carrier with the highest priority that would take @p record now,
    /// the lower entity breaking a tie, or NullEntity: the one it stands on if
    /// @p standing, and any it is inside the volume of that joins by volume.
    /// One joined by volume and held by touch would let go at once of a rider
    /// standing on other ground, so it does not take one.
    ECS::Entity BestCarrierFor(const CharacterRecord &record, bool standing) const;

    /// The priority of @p carrier's Carrier; 0 when it has none.
    uint8_t PriorityOf(ECS::Entity carrier) const;

    /// Boards @p record onto the best carrier that would take it, when it
    /// rides none or that carrier outranks the one it rides. @p velocity and
    /// @p groundVelocity, as the step read them, are made relative to the
    /// carrier boarded.
    void BoardBestCarrier(CharacterRecord &record, bool standing, JPH::Vec3 &velocity, JPH::Vec3 &groundVelocity);

    /// Rides @p record on @p wanted, the body its CharacterIntent names, as
    /// BoardBestCarrier boards a carrier. A body that is gone or static is let
    /// go and cleared from the intent; a cleared intent hands the rider back to
    /// its carriers. @return whether the intent decides the base this step.
    bool RideIntentBase(CharacterRecord &record, ECS::Entity wanted, JPH::Vec3 &velocity, JPH::Vec3 &groundVelocity);

    /// Whether @p record's base still carries it: alive, still the body it
    /// rode, still a carrier, and the character still rides bases.
    bool StillCarried(const CharacterRecord &record) const;

    /// Starts @p record riding @p carrier and makes @p velocity, the world
    /// velocity it is about to steer from, relative to the carrier — taking up
    /// the carrier's speed over the last ground as far as its grip says.
    void AttachRider(CharacterRecord &record, ECS::Entity carrier, JPH::Vec3 &velocity);

    /// Stops @p record riding. Its velocity becomes world velocity again, with
    /// the base's at its feet added while the base's body still exists.
    void ReleaseRider(CharacterRecord &record);

    /// ReleaseRider, from inside a step that has already read @p velocity and
    /// @p groundVelocity relative to the base: both go back to the world.
    void ReleaseMidStep(CharacterRecord &record, JPH::Vec3 &velocity, JPH::Vec3 &groundVelocity);

    /// The world velocity of @p record's base at its feet; zero when it rides
    /// nothing or the base's body is gone.
    JPH::Vec3 BaseVelocityAt(const CharacterRecord &record) const;

    /// After a riding character's sweep: takes its feet into the base's frame
    /// as the base stands before the solve, and marks it leaving when it
    /// stands on other ground, or its carrier stopped holding it more than the
    /// grace time ago. A jump leaves the deck.
    void RecordRiderPose(CharacterRecord &record, bool jumping, float deltaTime);

    /// Moves every riding character with its base by the motion the solve just
    /// gave the base, turning its facing with the base's yaw unless it opted
    /// out, and lets go of each one that is leaving.
    void CarryRiders();

    /// Changes @p record's capsule to @p stance's. False when it does not fit,
    /// which in practice means standing up under something too low; nothing
    /// changes then. On the ground the feet stay put and the head moves; in the
    /// air the head stays put and the feet move, and the Transform follows.
    bool ApplyStance(CharacterRecord &record, Stance stance);

    /// Stops every kinematic body swept last step and not this one, so it rests
    /// where its Transform put it rather than carrying on at the sweep's speed.
    void StopFinishedSweeps();

    /// Changes every character whose CharacterIntent asks for another stance,
    /// where it fits. See ApplyStance.
    void ApplyAskedStances();

    /// What @p record's last step left behind.
    CharacterState BuildCharacterState(const CharacterRecord &record) const;

    /// Writes each moving body's and every character's pose from the step
    /// that just ran into its Transform, and its velocities into its BodyState
    /// or CharacterState. A body that fell asleep gets one last write, at rest,
    /// and is then left alone.
    void WriteBack();

    /// Where @p entity's Transform puts it in world space.
    Pose TransformPose(ECS::Entity entity) const;

    /// Puts a body the step left non-finite back at its Transform, at rest.
    /// The Transform still holds the last pose that was a number: nothing
    /// non-finite is ever written to it.
    void RestoreBody(ECS::Entity entity);

    /// The same for a character, at its Transform's feet.
    void RestoreCharacter(CharacterRecord &record);

    /// Writes @p state into @p entity's BodyState, skipping a write that would
    /// change nothing, and stamps it as this world's own.
    void WriteBodyState(ECS::Entity entity, const BodyState &state);

    /// Writes @p state into @p entity's CharacterState, skipping a write that
    /// would change nothing.
    void WriteCharacterState(ECS::Entity entity, const CharacterState &state);

    /// Writes a world-space pose into @p entity's Transform, converting out of a
    /// parent's space and skipping a write that would change nothing, then
    /// stamps it as this world's own write.
    ///
    /// @p writeRotation is false for characters: the capsule is symmetric about
    /// its up axis, so the simulation has no opinion on facing and overwriting it
    /// would snap a turning character back to forward every frame. The one
    /// exception is a carry, which turns the facing gameplay left by the base's
    /// yaw.
    void WritePose(ECS::Entity entity, Pose pose, bool writeRotation);

    /// Records that a character touched a body, from inside the character's own
    /// sweep. The body-vs-body listener cannot see these: a character's inner
    /// body is kinematic, and against static geometry that pair generates no
    /// contact at all.
    void RecordCharacterTouch(const JPH::CharacterVirtual &character, const JPH::BodyID &other, ECS::Entity otherPiece,
                              JPH::RVec3Arg position, JPH::Vec3Arg normal);

    /// Answers the character sweep's questions about what it may push and be
    /// pushed by, and records what it touched.
    ///
    /// Jolt asks per contact rather than per character, so these cannot be plain
    /// settings on the character — each answer is looked up through the user data
    /// the character was created with.
    class CharacterContacts final : public JPH::CharacterContactListener
    {
public:
        explicit CharacterContacts(Impl &owner) : _owner(owner) {}

        void OnContactAdded(const JPH::CharacterVirtual *character, const JPH::BodyID &bodyId,
                            const JPH::SubShapeID &subShapeId, JPH::RVec3Arg contactPosition,
                            JPH::Vec3Arg contactNormal, JPH::CharacterContactSettings &settings) override;

        void OnCharacterContactAdded(const JPH::CharacterVirtual *character, const JPH::CharacterVirtual *other,
                                     const JPH::SubShapeID &subShapeId, JPH::RVec3Arg contactPosition,
                                     JPH::Vec3Arg contactNormal, JPH::CharacterContactSettings &settings) override;

        /// For a riding character, every body's velocity as seen from its base:
        /// the base itself is still, so friction holds the character on the deck
        /// and the base is not counted twice by the sweep and the carry.
        void OnAdjustBodyVelocity(const JPH::CharacterVirtual *character, const JPH::Body &body,
                                  JPH::Vec3 &linearVelocity, JPH::Vec3 &angularVelocity) override;

private:
        Impl &_owner;
    };

    CharacterContacts characterContacts{*this};
};

} // namespace Assisi::Physics
