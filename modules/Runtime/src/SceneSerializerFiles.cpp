/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Runtime/SceneSerializer.hpp>

#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/ECS/BlueprintMember.hpp>
#include <Assisi/Runtime/Hierarchy.hpp>
#include <Assisi/Runtime/NameComponent.hpp>

#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#include "SceneSerializerContext.hpp"
#include "SceneSerializerInstances.hpp"

// ---------------------------------------------------------------------------
// Reading and writing levels on disk.
// ---------------------------------------------------------------------------

namespace Assisi::Runtime
{

bool SceneSerializer::SaveEntitiesToFile(ECS::Scene &scene, std::span<const ECS::Entity> entities,
                                         const std::filesystem::path &path, const ECS::Transform &origin)
{
    if (entities.empty())
    {
        Core::Log::Error("Blueprint: nothing selected to save.");
        return false;
    }
    if (s_context || s_rawContextScene != nullptr)
    {
        Core::Log::Error("SaveEntitiesToFile: a serialization context is already active on this thread.");
        return false;
    }

    for (const ECS::Entity entity : entities)
    {
        if (scene.Has<ECS::BlueprintMember>(entity))
        {
            // Nesting is an `instances` entry, not copied entities. Copying them
            // would bake the inner blueprint into the new file and stop a fix to it
            // from reaching this one — the exact thing the format exists to avoid.
            Core::Log::Error("Blueprint: the selection contains a blueprint member. Prune it from its "
                             "instance first, or nest by adding an `instances` entry by hand.");
            return false;
        }
    }

    const auto &registry = Core::Reflect::ComponentRegistry::Instance();

    ScopedContextReset guard;
    s_context = SerializationContext{};

    // Names first, so a reference between two selected entities resolves whichever
    // order they are serialized in.
    std::unordered_set<std::string> usedNames;
    std::vector<std::string>        names;
    names.reserve(entities.size());
    for (const ECS::Entity entity : entities)
    {
        std::string name = UniqueName(AuthoredName(scene, entity), usedNames);
        s_context->entityToName.emplace(EntityKey(entity.index, entity.generation), name);
        names.push_back(std::move(name));
    }

    nlohmann::json doc;
    doc["version"]  = 2;
    doc["entities"] = nlohmann::json::array();

    for (std::size_t i = 0; i < entities.size(); ++i)
    {
        const ECS::Entity entity = entities[i];

        nlohmann::json written;
        written["name"]       = names[i];
        written["components"] = nlohmann::json::object();

        for (const Core::Reflect::ComponentMeta *meta : registry.SerializableComponents())
        {
            if (meta->name == "Name")
                continue; // the entity's `name` key already carries it

            const void *component = meta->getByEntity(&scene, entity.index, entity.generation);
            if (component == nullptr)
                continue;
            written["components"][meta->name] = meta->serialize(component);
        }

        // Around the new file's own origin, so the result is placeable. A parented
        // entity is already relative to its parent and reaches the origin through
        // the chain; dividing it too would divide twice.
        if (!scene.Has<Parent>(entity))
        {
            if (const ECS::Transform *transform = scene.Get<ECS::Transform>(entity))
                written["components"]["Transform"] = TransformToJson(InverseComposeTransform(origin, *transform));
        }

        doc["entities"].push_back(std::move(written));
    }

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::ofstream file(path, std::ios::binary);
    if (!file.is_open())
    {
        Core::Log::Error("Blueprint: cannot open '{}' for writing.", path.string());
        return false;
    }
    file << doc.dump(2);
    if (!file.good())
    {
        Core::Log::Error("Blueprint: write failed for '{}'.", path.string());
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// File I/O helpers
// ---------------------------------------------------------------------------

bool SceneSerializer::SaveToFile(ECS::Scene &scene, const std::filesystem::path &path, const LevelHeader &header,
                                 const InstanceTable *instances)
{
    // Binary, so the newlines written are the newlines that land on disk. A
    // text-mode write expands every '\n' to "\r\n" on Windows and leaves it
    // alone elsewhere, which makes the same level two different files depending
    // on who saved it — noise in every diff, and a refused join for the network
    // session's content-hash check.
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open())
    {
        Core::Log::Error("SceneSerializer: cannot open '{}' for writing", path.string());
        return false;
    }
    f << Save(scene, header, instances).dump(2);
    if (!f.good())
    {
        Core::Log::Error("SceneSerializer: write failed for '{}'", path.string());
        return false;
    }
    return true;
}

LevelResult SceneSerializer::LoadFromDisk(ECS::Scene &scene, const std::filesystem::path &path,
                                   const ProgressFn &onProgress, LevelHeader *header, InstanceTable *instances)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        Core::Log::Error("SceneSerializer: cannot open '{}' for reading", path.string());
        return std::unexpected(LevelError::FileUnreadable);
    }

    const nlohmann::json doc = nlohmann::json::parse(file, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded())
    {
        Core::Log::Error("SceneSerializer: '{}' is not readable JSON", path.string());
        return std::unexpected(LevelError::MalformedJson);
    }

    try
    {
        return Load(scene, doc, onProgress, header, instances);
    }
    catch (const std::exception &ex)
    {
        // Same contract as LoadFromFile below: a failed load yields an empty
        // scene rather than a half-populated one. Load itself no longer throws —
        // this is for the component `addToScene` hooks it runs, which are
        // generated code over nlohmann and can.
        Core::Log::Error("SceneSerializer: failed to load '{}': {}", path.string(), ex.what());
        scene.Clear();
        return std::unexpected(LevelError::MalformedJson);
    }
}

std::expected<std::vector<std::string>, LevelError> SceneSerializer::ReadLevelSystems(
    std::string_view assetPath)
{
    const auto text = Core::AssetSystem::ReadText(assetPath);
    if (!text)
    {
        Core::Log::Error("SceneSerializer: cannot read asset '{}'", assetPath);
        return std::unexpected(LevelError::FileUnreadable);
    }

    const nlohmann::json doc = nlohmann::json::parse(*text, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded())
    {
        Core::Log::Error("SceneSerializer: cannot parse '{}'", assetPath);
        return std::unexpected(LevelError::MalformedJson);
    }

    std::vector<std::string> out;
    if (const auto it = doc.find("systems"); it != doc.end() && it->is_array())
    {
        for (const auto &name : *it)
        {
            if (name.is_string())
                out.push_back(name.get<std::string>());
        }
    }
    return out;
}

LevelResult SceneSerializer::LoadFromFile(ECS::Scene &scene, std::string_view assetPath,
                                          const ProgressFn &onProgress, LevelHeader *header,
                                          InstanceTable *instances)
{
    const auto text = Core::AssetSystem::ReadText(assetPath);
    if (!text)
    {
        Core::Log::Error("SceneSerializer: cannot read asset '{}'", assetPath);
        return std::unexpected(LevelError::FileUnreadable);
    }

    // A parse failure leaves the scene untouched, so it is refused before Load is
    // ever called rather than caught after.
    const nlohmann::json doc = nlohmann::json::parse(*text, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded())
    {
        Core::Log::Error("SceneSerializer: '{}' is not readable JSON", assetPath);
        return std::unexpected(LevelError::MalformedJson);
    }

    try
    {
        return Load(scene, doc, onProgress, header, instances);
    }
    catch (const std::exception &ex)
    {
        // Load reports its own failures by value and clears as it goes; what is
        // left for this to catch is a throw out of a component's addToScene hook
        // partway through, which leaves the scene half-populated. Clear it so a
        // failed load yields an empty scene, never a corrupt one.
        // (ScopedContextReset in Load already freed s_context.)
        //
        // Catches std::exception, not just json::exception: Load runs arbitrary
        // component addToScene hooks, and one throwing anything else (bad_alloc,
        // out_of_range from a hook's own container) would otherwise escape and
        // leave the half-populated scene behind.
        Core::Log::Error("SceneSerializer: failed to load '{}': {}", assetPath, ex.what());
        scene.Clear();
        return std::unexpected(LevelError::MalformedJson);
    }
}

} // namespace Assisi::Runtime
