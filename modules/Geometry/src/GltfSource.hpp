/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file GltfSource.hpp
/// @brief Reads a glTF from the asset tree and parses it, with its external
///        buffers loaded — what every importer of a `.gltf` or `.glb` starts from.
///
/// Internal to Geometry: the parsed asset is fastgltf's, which no public header
/// exposes.

#include <expected>
#include <string>
#include <string_view>

#include <fastgltf/types.hpp>

#include <Assisi/Geometry/MeshImporter.hpp>

namespace Assisi::Geometry
{

/// @brief Whether @p path names a `.gltf` or `.glb`, ignoring case.
[[nodiscard]] bool IsGltfPath(std::string_view path) noexcept;

/// @brief The virtual-path directory holding @p vpath, or "" if it has none.
///        A glTF's sibling buffers and images are named relative to it.
[[nodiscard]] std::string ParentDir(std::string_view vpath);

/// @brief Reads @p virtualPath through AssetSystem and parses it, then reads
///        every sibling buffer it names, also through AssetSystem.
///
/// The parser never touches the filesystem itself, so a glTF cannot name a
/// buffer outside the asset root. Every KHR_materials_* extension is accepted,
/// whether or not anything reads it, so a file that marks one required still
/// parses.
[[nodiscard]] std::expected<fastgltf::Asset, MeshImportError> LoadGltfSource(std::string_view virtualPath);

} // namespace Assisi::Geometry
