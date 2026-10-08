/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Geometry/AnimationImport.hpp>

#include <Assisi/Core/AssetSidecar.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/Assert.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Geometry/AnimationClip.hpp>
#include <Assisi/Geometry/AnimationFile.hpp>

#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <unordered_set>

#include "GltfAnimation.hpp"
#include "GltfSource.hpp"

namespace Assisi::Geometry
{

namespace
{

namespace fs = std::filesystem;

/// A name as a filename component: anything but `[A-Za-z0-9._-]` becomes '_',
/// and nothing becomes "clip".
std::string SafeName(std::string_view name)
{
    std::string safe;
    safe.reserve(name.size());
    for (const char c : name)
    {
        const bool keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
                          c == '_' || c == '-';
        safe.push_back(keep ? c : '_');
    }
    return safe.empty() ? std::string{"clip"} : safe;
}

/// The model's file name without its extension.
std::string StemOf(std::string_view vpath)
{
    return fs::path{std::string{vpath}}.stem().generic_string();
}

bool WriteFile(const fs::path &path, std::span<const std::byte> bytes)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return stream.good();
}

/// Writes an "animation" sidecar for @p clipFile unless one is there already,
/// whose id, and the players that name it, are then kept.
bool EnsureSidecar(const fs::path &clipFile)
{
    fs::path sidecarPath = clipFile;
    sidecarPath += ".aast";
    if (fs::exists(sidecarPath))
    {
        return true;
    }
    Core::AssetSidecar sidecar = Core::AssetSidecar::Leaf(Core::MintAssetId());
    sidecar.uses = {Core::AssetUse{.kind = std::string{kAnimationKindName}}};
    const std::string text = Core::SerializeSidecar(sidecar);
    std::ofstream stream(sidecarPath, std::ios::binary | std::ios::trunc);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    return stream.good();
}

/// The file animation @p index is written to: `<model>_<name>.glb`, or with the
/// animation's position appended when an earlier one took that name.
std::string ClipFileName(std::string_view stem, std::string_view name, std::size_t index,
                         std::unordered_set<std::string> &taken)
{
    std::string file = std::format("{}_{}.glb", stem, SafeName(name));
    if (!taken.insert(file).second)
    {
        file = std::format("{}_{}_{}.glb", stem, SafeName(name), index);
        taken.insert(file);
    }
    return file;
}

} // namespace

std::string_view ToString(AnimationExtractError error) noexcept
{
    switch (error)
    {
    case AnimationExtractError::ReadFailed:
        return "the model, or a buffer beside it, could not be read";
    case AnimationExtractError::NoAnimations:
        return "the model has no animations";
    case AnimationExtractError::WriteFailed:
        return "a clip file could not be written";
    default:
        ASSISI_ASSERT(false, "ToString reached an AnimationExtractError with no description");
        Core::Log::Error("AnimationExtract: no description for this error");
        return "the animations could not be extracted";
    }
}

std::expected<ExtractedAnimations, AnimationExtractError> ExtractGltfAnimations(std::string_view gltfVirtualPath)
{
    const std::expected<fastgltf::Asset, MeshImportError> source = LoadGltfSource(gltfVirtualPath);
    const std::expected<fs::path, Core::AssetError> sourceFile = Core::AssetSystem::Resolve(gltfVirtualPath);
    if (!source || !sourceFile)
    {
        return std::unexpected(AnimationExtractError::ReadFailed);
    }
    if (source->animations.empty())
    {
        return std::unexpected(AnimationExtractError::NoAnimations);
    }

    const fs::path directory = sourceFile->parent_path();
    const std::string stem = StemOf(gltfVirtualPath);
    std::unordered_set<std::string> taken;
    ExtractedAnimations extracted;
    for (std::size_t index = 0; index < source->animations.size(); ++index)
    {
        // Named before it can be skipped, so a file keeps its name whether or
        // not an earlier animation of the same name plays.
        const std::string file = ClipFileName(stem, source->animations[index].name, index, taken);
        const std::expected<GltfClip, AnimationReadError> clip = ReadGltfAnimation(*source, index);
        if (!clip)
        {
            Core::Log::Warn("Extract animations: '{}' animation '{}' is left out: {}.", gltfVirtualPath,
                            std::string_view{source->animations[index].name}, ToString(clip.error()));
            ++extracted.skipped;
            continue;
        }
        const std::vector<std::byte> glb = WriteClipGlb(*clip, *source);
        if (glb.empty() || !WriteFile(directory / file, glb) || !EnsureSidecar(directory / file))
        {
            return std::unexpected(AnimationExtractError::WriteFailed);
        }
        ++extracted.written;
    }
    return extracted;
}

} // namespace Assisi::Geometry
