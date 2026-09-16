/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Reflect/AssetTypeMeta.hpp
/// @brief Runtime descriptor for a reflected standalone asset type (AASSET).
///
/// Slimmer than ComponentMeta: an asset (e.g. a Material saved as .amat) is not
/// scene-bound, so there is no addToScene / iterateEntities / getByEntity. Just
/// a field table (for a reflection-driven editor) plus serialize/deserialize
/// over a caller-owned instance.

#include <functional>
#include <string>
#include <typeindex>
#include <vector>

#include <nlohmann/json.hpp>

#include <Assisi/Core/Reflect/FieldMeta.hpp>

namespace Assisi::Core::Reflect
{

struct AssetTypeMeta
{
    std::string name;
    std::type_index typeIndex;
    std::vector<FieldMeta> fields;

    /// @brief Serialize an asset instance to a JSON object of its fields.
    ///        The .amat envelope (version/type) is added by the load/save
    ///        helper, not here — this is the field payload only.
    /// @param instance_ptr Pointer to a live asset of this type.
    std::function<nlohmann::json(const void *instance_ptr)> serialize;

    /// @brief Deserialize field values from JSON into an existing instance.
    ///
    /// Per-field "if present" application: keys absent from @p j leave the
    /// instance's current value untouched, so old files stay forward-compatible
    /// as fields are added. Pass a default-constructed instance for a clean load.
    ///   j            — JSON object holding the asset's fields.
    ///   instance_ptr — pointer to a mutable asset of this type.
    ///
    /// @return false when a field is present but unreadable, having logged which
    ///         one. Unlike the component path this writes straight into the
    ///         caller's instance, so fields before the bad one may already be
    ///         applied — the caller owns that instance and should drop it.
    ///         Absent keys are not failures.
    std::function<bool(const nlohmann::json &j, void *instance_ptr)> deserialize;

    /// @brief Default-construct an instance on the heap, or null if the
    ///        allocation failed.
    ///
    /// For a caller that knows the type only by the name in a document's
    /// envelope and so cannot name it to the compiler — the cooker above all,
    /// which reads a `.amat` and a config file through the same code. Every
    /// instance from here is freed through `destroy` below; the pair is the only
    /// way to own one, since the type is erased by the time a caller holds it.
    std::function<void *()> construct;

    /// @brief Free an instance `construct` returned. Null is accepted and does
    ///        nothing, so a failed construct needs no guard at the call site.
    std::function<void (void *instance_ptr)> destroy;
};

} // namespace Assisi::Core::Reflect
