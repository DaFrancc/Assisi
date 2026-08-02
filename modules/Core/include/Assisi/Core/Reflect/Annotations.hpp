/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Reflect/Annotations.hpp
/// @brief Marker macros consumed by reflectgen and compiled away at build time.
///
/// ACOMP(...)  — marks a struct as a reflected component.
/// AFIELD(...) — marks a field for reflection and serialization.
///
/// Both accept a comma-separated list of flags and key=value pairs:
///   ACOMP()
///   ACOMP(tracked)               -- opts into ECS change detection
///   ACOMP(transient)             -- id-only registration, never serialized
///   ACOMP(replicable)            -- *can* travel over the network (implies tracked)
///   AFIELD()
///   AFIELD(transient)            -- excluded from serialization
///   AFIELD(norep)                -- saved to disk, never sent over the network
///   AFIELD(min=0.0, max=100.0)   -- editor clamp hints
///
/// ── Replication: five gates, three mechanisms ───────────────────────────────
///
/// A field value crosses the wire only if it passes *all* of these. Each lives
/// at the scope that owns the decision, which is the point: an engine module can
/// touch G1 and G5 and nothing else, so it cannot set a game's network policy by
/// editing one of its own headers.
///
///   G1  capability   component type   ACOMP(replicable)              opt-in
///   G2  game policy  game             game.json networking.neverReplicate
///   G3  entity gate  entity instance  the NetSync::Replicated marker opt-in
///   G4  instance     entity instance  Replicated::excluded (a mask)  opt-out
///   G5  field gate   field            AFIELD(norep)                  opt-out
///
/// Plus the two dynamic gates that always applied: the entity has the component,
/// and the acked baseline says the client does not already hold this value.
///
/// **`replicable` grants a capability, not a policy.** It says this type *has* a
/// wire form; whether a given entity actually sends it is G2 and G4. The retired
/// spelling `replicated` fused the two, and reflectgen rejects it by name rather
/// than ignoring it — an unknown flag would parse as nothing and silently
/// un-replicate a component that used to travel.
///
/// **Polarity is deliberate in both directions.** G1 is opt-in because a type
/// should not acquire a wire form by accident (SpatialOS migrated to opt-in
/// after blanket schema generation failed to scale). G4 is opt-out because
/// Transform, Name, MeshRenderer and RigidBodyDescriptor are wanted on
/// essentially every replicated entity, and making each level author restate
/// that would manufacture boilerplate and silent under-replication.
///
/// **`replicable` implies `tracked`**, because an untracked component reports
/// change tick 0 forever — it would replicate once at spawn and then go silent.
/// Writing both is legal and *not* redundant: there is one change-tick lane with
/// two readers, so the implication serves replication while an explicit
/// `tracked` records that a local system needs the ticks too — which is what
/// keeps them if `replicable` is ever removed. `ECS::Transform` is the live
/// example (PropagateTransforms predates the network by a long way). Nothing is
/// emitted at build time to say this, deliberately: a note printed forever on
/// correct code teaches people to skim build output.
///
/// A type that simply does not replicate says so by *not* being marked. Where
/// that silence is a decision rather than an oversight — `Runtime::Camera`,
/// whose mirrored `isActive` would hand a client a view it did not choose — the
/// reason belongs in that type's header comment, where every other design
/// rationale in this codebase lives.
///
/// reflectgen hard-fails on ACOMP(replicable, transient) (nothing to encode), on
/// AASSET(replicable) (assets are not entities), on AFIELD(transient, norep)
/// (redundant), and on AFIELD(norep) in a component that is not replicable (the
/// annotation would mean nothing).
///
/// Full rationale, alternatives weighed, and the survey of how other ECS engines
/// answer this: docs/replication-optin-plan-v1.md and
/// docs/replication-research-ecs-survey.md.
///
/// Radio (declarative editor visibility driven by a sibling enum's value):
///   AFIELD(radioBroadcast)       -- marks an AENUM enum field as a broadcaster
///   AFIELD(radioListen = { source = shape, value = Box, behavior = vanish })
///   AFIELD(radioListen = { source = shape, value = {Sphere, Capsule}, behavior = grey })
/// A listener field is active only while the named source enum equals one of the
/// listed value(s); otherwise the inspector greys it out (behavior = grey) or
/// hides it (behavior = vanish).
///
/// A field may be BOTH a broadcaster and a listener, so a source can itself
/// follow another broadcaster (a chain). The rule is recursive: while a source
/// is inactive, all of its listeners hide unconditionally (regardless of their
/// own value/behavior); once it becomes active, its listeners resolve normally
/// against its current value.
///
///   AFIELD(radioBroadcast) Foo myFoo;
///   AFIELD(radioBroadcast, radioListen = { source = myFoo, value = A, behavior = vanish }) Bar myBar;
///   AFIELD(radioListen = { source = myBar, value = X, behavior = grey }) float f;
///
/// reflectgen hard-fails the build if radioBroadcast is on a non-enum field, if
/// `source` names a field that is not an AENUM enum marked AFIELD(radioBroadcast)
/// in the same struct, if any `value` is not an enumerator of that enum, if
/// `behavior` is neither grey nor vanish, or if the chain contains a cycle.

#define ACOMP(...)
#define AFIELD(...)

/// AASSET(...) — marks a struct as a reflected standalone asset type (e.g. a
/// Material saved as a .amat file). Like ACOMP but registers into the
/// AssetTypeRegistry instead of ComponentRegistry: the generated code has no
/// scene/entity hooks — just serialize(const T&) -> json and
/// deserialize(json, T&). Fields are marked with AFIELD, same as components.
/// Asset types must not contain AFIELD EntityRef fields (there is no scene to
/// resolve them against).
#define AASSET(...)

/// AEVENT() — marks a struct as an event type.
/// Compiles to nothing today; reserved for future reflectgen support
/// (serialization, network replication interception).
#define AEVENT(...)

/// AENUM() — marks an `enum class` so reflectgen records its enumerators, making
/// it usable as an AFIELD type (serialized by value, edited as a dropdown). The
/// enum must be defined in the same header as the component(s) that use it, and
/// reachable from their namespace. Any fixed-width underlying type works (the
/// default `int`, or an explicit `: std::uint8_t` / `: std::int16_t` / etc.):
/// reflectgen records the width so the inspector reads/writes the field at its
/// true size. Platform-dependent `long` / `unsigned long` are rejected — spell
/// the width explicitly.
///
///   AENUM()
///   enum class ColliderShape : std::uint8_t { Box, Sphere, Capsule };
#define AENUM(...)
