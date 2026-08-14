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
#include "SceneSerializerHeader.hpp"
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
    if (ScopedContext::Current() != nullptr || s_rawContextScene != nullptr)
    {
        Core::Log::Error("SaveEntitiesToFile: a serialization context is already active on this thread.");
        return false;
    }

    for (const ECS::Entity entity : entities)
    {
        if (scene.Has<ECS::BlueprintMember>(entity))
        {
            // Nesting is an `instances` entry, not copied entities: copying would
            // bake the inner blueprint into the new file and stop a fix to it from
            // reaching this one.
            Core::Log::Error("Blueprint: the selection contains a blueprint member. Prune it from its "
                             "instance first, or nest by adding an `instances` entry by hand.");
            return false;
        }
    }

    // Which entities the file will hold, so "is this entity's parent coming with
    // it" has an answer below. Selecting a child without its parent is an ordinary
    // thing to want — the parts of a rig are worth saving as a blueprint without
    // the rig — so it is supported rather than refused.
    std::unordered_set<uint64_t> inSet;
    inSet.reserve(entities.size());
    for (const ECS::Entity entity : entities)
        inSet.insert(EntityKey(entity.index, entity.generation));

    const auto parentComesAlong = [&](ECS::Entity entity)
                                  {
                                      const Parent *parent = scene.Get<Parent>(entity);
                                      return parent != nullptr && parent->parent != ECS::NullEntity &&
                                             inSet.contains(EntityKey(parent->parent.index, parent->parent.generation));
                                  };

    const auto &registry = Core::Reflect::ComponentRegistry::Instance();

    const ScopedContext scoped;

    // Names first, so a reference between two selected entities resolves whichever
    // order they serialize in.
    std::unordered_set<std::string> usedNames;
    std::vector<std::string>        names;
    names.reserve(entities.size());
    for (const ECS::Entity entity : entities)
    {
        std::string name = UniqueName(AuthoredName(scene, entity), usedNames);
        scoped->entityToName.emplace(EntityKey(entity.index, entity.generation), name);
        names.push_back(std::move(name));
    }

    // Every wire leaving the selection, named before it is cut. The file cannot
    // name what it does not contain, so these become null — and "create blueprint
    // from selection" is a gesture made on a subset of a wired-up level, which
    // makes cutting wires the normal case rather than the exceptional one. Named
    // down to the field and by the names the file is being written under, because
    // "some reference was dropped" tells nobody what to re-wire.
    ForEachRefLeavingSet(scene, entities,
                         [&](const Core::Reflect::ComponentMeta &meta, const Core::Reflect::FieldMeta &field,
                             std::size_t owner, ECS::Entity target)
        {
            Core::Log::Warn("Blueprint: {}::{} on '{}' references '{}', which is not in the "
                            "selection — it is null in '{}'.",
                            meta.name, field.name, names[owner],
                            AuthoredName(scene, target), path.string());
        });

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

        // Stored around the new file's own origin, so the result is placeable. An
        // entity whose parent comes along reaches that origin through the chain, so
        // dividing it too would divide twice — its local offset is already right.
        //
        // Every other entity is a root of this file, including one whose parent was
        // left behind. That parent is nulled just above, so what the file holds must
        // be a pose measured from the origin rather than from a parent that is not
        // coming: its own Transform is an offset in a space this file does not have,
        // and writing it raw is what put the copy somewhere the original never stood
        // (round-7 S16). WorldTransformOf resolves the chain that is being cut, so a
        // selection of children saved without their parent lands where it was.
        if (!parentComesAlong(entity))
        {
            if (scene.Has<ECS::Transform>(entity))
                written["components"]["Transform"] =
                    TransformToJson(InverseComposeTransform(origin, WorldTransformOf(scene, entity)));
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
                                 InstanceTable *instances)
{
    // Binary, so the newlines written are the newlines that land on disk. Text mode
    // expands '\n' to "\r\n" on Windows only, which makes the same level two
    // different files depending on who saved it — noise in every diff, and a
    // refused join for the network session's content-hash check.
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
                                          const LoadOptions &options)
{
    // Both refusals below precede the load, so the caller's scene is untouched.
    std::ifstream file(path);
    if (!file.is_open())
    {
        Core::Log::Error("SceneSerializer: cannot open '{}' for reading", path.string());
        return std::unexpected(LevelFailure{.kind = LevelError::FileUnreadable});
    }

    const nlohmann::json doc = nlohmann::json::parse(file, nullptr, /*allow_exceptions=*/ false);
    if (doc.is_discarded())
    {
        Core::Log::Error("SceneSerializer: '{}' is not readable JSON", path.string());
        return std::unexpected(LevelFailure{.kind = LevelError::MalformedJson});
    }

    try
    {
        return Load(scene, doc, options);
    }
    catch (const std::exception &ex)
    {
        // Same contract as LoadFromFile below: a failed load yields an empty scene,
        // never a half-populated one. Load reports its own failures by value —
        // including a `version` or `entities` key of the wrong shape, which it once
        // threw on and now guards. What is left to catch is the component
        // `addToScene` hooks it runs, which are generated code over nlohmann.
        //
        // `sceneReplaced` unconditionally, whichever side of Load's clear the throw
        // came from: the Clear below is this function's own, so by the time this
        // returns the caller's scene is gone either way.
        Core::Log::Error("SceneSerializer: failed to load '{}': {}", path.string(), ex.what());
        scene.Clear();
        return std::unexpected(LevelFailure{.kind = LevelError::MalformedJson, .sceneReplaced = true});
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

    const nlohmann::json doc = nlohmann::json::parse(*text, nullptr, /*allow_exceptions=*/ false);
    if (doc.is_discarded())
    {
        Core::Log::Error("SceneSerializer: cannot parse '{}'", assetPath);
        return std::unexpected(LevelError::MalformedJson);
    }

    // The same reader Load fills the header with — see ParseSystemNames for why
    // that has to be the same reader and not merely the same rule.
    return ParseSystemNames(doc);
}

LevelResult SceneSerializer::LoadFromFile(ECS::Scene &scene, std::string_view assetPath,
                                          const LoadOptions &options)
{
    // As in LoadFromDisk: everything up to the Load call leaves the scene alone.
    const auto text = Core::AssetSystem::ReadText(assetPath);
    if (!text)
    {
        Core::Log::Error("SceneSerializer: cannot read asset '{}'", assetPath);
        return std::unexpected(LevelFailure{.kind = LevelError::FileUnreadable});
    }

    // Parsed before Load is called, so a parse failure leaves the scene untouched.
    const nlohmann::json doc = nlohmann::json::parse(*text, nullptr, /*allow_exceptions=*/ false);
    if (doc.is_discarded())
    {
        Core::Log::Error("SceneSerializer: '{}' is not readable JSON", assetPath);
        return std::unexpected(LevelFailure{.kind = LevelError::MalformedJson});
    }

    try
    {
        return Load(scene, doc, options);
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
