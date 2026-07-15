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
///   AFIELD()
///   AFIELD(transient)            -- excluded from serialization
///   AFIELD(min=0.0, max=100.0)   -- editor clamp hints
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
