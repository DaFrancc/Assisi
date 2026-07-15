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
///   AFIELD(min=0.0, max=100.0)   -- editor hints (future use)

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
/// reachable from their namespace. Use a 4-byte underlying type (the default
/// `int`, or `: std::uint32_t`) — the inspector reads the value as a 4-byte int.
///
///   AENUM()
///   enum class ColliderShape : std::uint32_t { Box, Sphere, Capsule };
#define AENUM(...)
