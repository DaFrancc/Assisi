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
