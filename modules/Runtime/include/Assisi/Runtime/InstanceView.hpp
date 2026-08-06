/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file InstanceView.hpp
/// @brief The undefined primary template that generated typed views specialize.
///
/// For blueprints stable enough to be part of the code, a generator reads the
/// file and emits one specialization per opted-in blueprint
/// (docs/blueprint-system-concept.md §7):
///
/// ```cpp
/// if (auto car = SpawnBlueprint<Car>(world, at))
///     world.physics.AddForce(car->body, forward * 500.f);
/// ```
///
/// A member typo does not compile, and renaming a member breaks the build at
/// every call site — which is not a new tax, because member names are *already*
/// stringly coupled through FindMember and through author-time wiring. The
/// codegen moves an existing failure from runtime to compile time.
///
/// Nesting stays visible even though the runtime member list is flat: a nested
/// instance becomes a nested struct, so `car.body` in code and `car/body` at
/// runtime are the same entity reached two ways. Grouping rather than flattening
/// to `car_body` avoids a real collision — a top-level member named `car_body`
/// beside an instance `car` with a member `body` would otherwise produce two
/// fields with one name, and break the build for a reason visible in neither
/// file.
///
/// ## Three rules keep this from becoming the thing it resembles
///
/// - **Receipts, not references.** A view lives in the scope of the call that
///   produced it. Only `instanceId` may outlive it. The entity handles are for
///   right now: a handle goes stale when its entity dies, but the id survives
///   members dying, because FindMember re-resolves and DestroyInstance finds
///   whatever is left. Storing the handles is how the stored-member-list problem
///   comes back in disguise.
/// - **Fields only.** The generator emits no methods, and regeneration clobbers
///   the file — so `car.ApplyHandbrake()` cannot survive a build. Closed by
///   machinery, not by discipline.
/// - **No reflected component may contain one**, checked by reflectgen. A view
///   inside a component is a stored member list plus a serialization hazard.
///
/// It is a plain aggregate of handles: no identity, no invariants, no methods,
/// no polymorphism, and it owns nothing.
///
/// Undefined on purpose, the same idiom as `Core::Reflect::MessageTraits<T>`
/// (MessageMeta.hpp) — so a `T` that was never opted in fails with an incomplete
/// type at the spawn site rather than silently producing nothing. It is the
/// second use of a pattern the engine already has, not a new one.

namespace Assisi::Runtime
{

template <typename T> struct InstanceView;

/// @brief The blueprint an opted-in type names, available at compile time.
///
/// Specialized by the same generated header, and undefined for the same reason:
/// it is what lets `SpawnBlueprint<Car>(world, at)` take no string at all, so
/// the file a view was generated from and the file it spawns cannot drift apart.
template <typename T> struct InstanceViewTraits;

} // namespace Assisi::Runtime
