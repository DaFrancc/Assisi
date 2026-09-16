/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Runtime/SceneSerializer.hpp>

#include <Assisi/Core/Logger.hpp>

#include <exception>
#include <string>
#include <utility>
#include <vector>

#include "SceneSerializerHeader.hpp"

// ---------------------------------------------------------------------------
// Reading level and blueprint documents through whatever the executable installed.
// ---------------------------------------------------------------------------

namespace Assisi::Runtime
{

namespace
{

/// Installed once, before any level or blueprint loads, and read-only afterwards,
/// so loads on worker threads read it without a lock.
SceneSerializer::DocumentReader &InstalledReader()
{
    static SceneSerializer::DocumentReader reader;
    return reader;
}

} // namespace

SceneSerializer::DocumentReader SceneSerializer::SetDocumentReader(DocumentReader reader)
{
    return std::exchange(InstalledReader(), std::move(reader));
}

std::expected<nlohmann::json, LevelError> SceneSerializer::ReadDocument(std::string_view vpath)
{
    const DocumentReader &reader = InstalledReader();
    if (!reader)
    {
        // No fallback to reading text: a shipped game that quietly opened a loose
        // file here would hide a pak that is missing the level.
        Core::Log::Error("SceneSerializer: no document reader is installed, so '{}' cannot be read.", vpath);
        return std::unexpected(LevelError::FileUnreadable);
    }
    return reader(vpath);
}

std::expected<std::vector<std::string>, LevelError> SceneSerializer::ReadLevelSystems(std::string_view assetPath)
{
    const std::expected<nlohmann::json, LevelError> doc = ReadDocument(assetPath);
    if (!doc)
    {
        return std::unexpected(doc.error());
    }

    // The same reader Load fills the header with — see ParseSystemNames for why
    // that has to be the same reader and not merely the same rule.
    return ParseSystemNames(*doc);
}

LevelResult SceneSerializer::LoadFromFile(ECS::Scene &scene, std::string_view assetPath, const LoadOptions &options)
{
    // Read before Load is called, so an unreadable document leaves the scene alone.
    const std::expected<nlohmann::json, LevelError> doc = ReadDocument(assetPath);
    if (!doc)
    {
        return std::unexpected(LevelFailure{.kind = doc.error()});
    }

    try
    {
        return Load(scene, *doc, options);
    }
    catch (const std::exception &ex)
    {
        // Load reports its own failures by value and clears as it goes; what is left
        // to catch is a throw partway through — out of a component's addToScene hook
        // above all — which leaves the scene half-populated. Clear it, so a failed
        // load yields an empty scene and never a corrupt one. (Load's ScopedContext
        // has already put back whatever context was live before it.)
        //
        // std::exception, not json::exception: those hooks are arbitrary code, and
        // one throwing a bad_alloc or its own container's out_of_range would
        // otherwise escape with the half-populated scene left behind.
        //
        // `sceneReplaced` for the same reason as in LoadFromDisk: the Clear below is
        // ours, so the caller's scene is gone whichever side of Load's own clear the
        // throw came from.
        Core::Log::Error("SceneSerializer: failed to load '{}': {}", assetPath, ex.what());
        scene.Clear();
        return std::unexpected(LevelFailure{.kind = LevelError::MalformedJson, .sceneReplaced = true});
    }
}

} // namespace Assisi::Runtime
