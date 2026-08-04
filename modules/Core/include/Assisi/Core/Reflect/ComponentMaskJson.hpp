/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ComponentMaskJson.hpp
/// @brief JSON (de)serialization for `ComponentMask`, as an array of component
///        type names.
///
/// Split from ComponentMask.hpp for the same reason AssetIdJson is split from
/// AssetId: the mask itself is a trivially-copyable value on a hot pooled
/// component, and nothing that merely *stores* one should pull in the JSON
/// library or the registry.
///
/// The wire form is names rather than bits, and so is the disk form:
///
///     "excluded": ["Bounce", "Name"]
///
/// Bits index a component's *replicable ordinal*, which is not stable across
/// builds — it shifts when any component is added, renamed, or has its
/// capability flipped. See ComponentMask.hpp for why that makes raw bits the
/// wrong thing to persist.

#include <Assisi/Core/Reflect/ComponentMask.hpp>

#include <nlohmann/json_fwd.hpp>

namespace Assisi::Core::Reflect
{

/// @brief Serialize to an array of component names, ascending by ordinal.
///
/// Deterministic ordering so a mask that has not changed produces a
/// byte-identical level file — otherwise a save would show spurious diffs and
/// mark scenes dirty for no reason.
[[nodiscard]] nlohmann::json SerializeComponentMask(const ComponentMask &mask);

/// @brief Read a mask from an array of component names.
///
/// A name that does not resolve to a *replicable* registered component warns and
/// is dropped — there is no bit that could represent it, so it cannot round-trip
/// (ComponentMask.hpp explains why that trade is the right one). A non-array
/// value warns and yields an empty mask, rather than throwing: a malformed level
/// file should lose one field, not fail to load.
[[nodiscard]] ComponentMask DeserializeComponentMask(const nlohmann::json &value);

} // namespace Assisi::Core::Reflect
