/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SceneSerializerContext.hpp
/// @brief The thread-local context a save, load or transfer runs inside, and the
///        entity packing its reference hooks use.
///
/// State every serializer translation unit shares — the reference hooks, Save,
/// Load and the instance paths all read and write it — so it is `inline` here,
/// one copy per thread rather than one per TU.
///
/// Valid only between the entry point that engages it and that call's return: a
/// scope guard (ScopedContextReset, ScopedRawEntityContext) clears it on every
/// exit path, and outside that window the hooks see no context and resolve every
/// ref to null. One at a time — the guards do not stack, so the entry points that
/// can be reached re-entrantly (SaveEntitiesToFile, TransferEntities, the
/// blueprint expansions) refuse to start while another context is live.

#include <Assisi/Runtime/SceneSerializer.hpp>

#include <Assisi/Core/Reflect/BinaryCodec.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Assisi::Runtime
{

struct SerializationContext
{
    /// How an EntityRef field addresses its target while this context is live.
    /// Explicit because mixing the two vocabularies produces a plausible scene
    /// rather than an error.
    enum class RefMode : std::uint8_t
    {
        Names,      ///< Save/Load of a file: a ref is the target entity's name.
        SetIndices, ///< TransferEntities: a ref is an index within the moved set.
    };
    RefMode mode = RefMode::Names;

    // SetIndices. Save side: entity key (gen<<32|idx) → index within the set.
    // Load side: index → live Entity.
    std::unordered_map<uint64_t, uint32_t> entityToIndex;
    std::vector<ECS::Entity>               indexToEntity;

    // Names. Save side: entity key → the unique name it is being written under.
    // Load side: name → the live Entity created for it.
    std::unordered_map<uint64_t, std::string>    entityToName;
    std::unordered_map<std::string, ECS::Entity> nameToEntity;

    /// Names an EntityRef asked for that the file never declared. Collected here
    /// rather than reported where they are found: RefToEntity runs inside a
    /// component's generated deserialize, which knows neither the file nor the
    /// entity it speaks for. Load reports them together and refuses the file.
    std::vector<std::string> unresolvedRefNames;
};

inline thread_local std::optional<SerializationContext> s_context;

// Raw-entity (identity) context — engaged by ScopedRawEntityContext. When set,
// EntityRef fields map through packed (slot, generation) keys against this scene
// instead of the Save/Load remap tables. Mutually exclusive with s_context and
// non-reentrant; ScopedRawEntityContext asserts both.
inline thread_local ECS::Scene *s_rawContextScene = nullptr;

inline uint64_t EntityKey(uint32_t idx, uint32_t gen)
{
    return (static_cast<uint64_t>(gen) << 32) | idx;
}

/// A packed EntityRef the way BinaryCodec spells one: index low, generation high.
/// The same packing EntityKey uses, named apart because it means something
/// different — this one crosses the codec's reference hook.
constexpr uint64_t PackEntity(ECS::Entity entity)
{
    return (static_cast<uint64_t>(entity.generation) << 32) | entity.index;
}

/// Calls @p report for every non-null EntityRef on @p entities whose target is not
/// itself in @p entities.
///
/// Both writers that take a *subset* of a scene — migration and "create blueprint
/// from selection" — null those references, because neither destination can name
/// something it does not contain. Silently is the one thing they must not do it:
/// the gesture is made on a subset of a wired-up level, so cutting wires is the
/// normal case rather than the exceptional one, and nothing about the result shows
/// which ones were cut. Found here rather than in the reference hook, which runs
/// inside a generated serialize and knows neither the field nor the entity it
/// speaks for.
///
/// @p report takes (component, field, owner's index within @p entities, target).
/// The index rather than the handle because each site has already built something
/// per-entity — a name, a set index — that the message wants and the handle alone
/// would have to look back up. Each site words its own message: the two say the
/// same thing but not about the same destination.
template <typename Fn>
void ForEachRefLeavingSet(ECS::Scene &scene, std::span<const ECS::Entity> entities, Fn &&report)
{
    std::unordered_set<uint64_t> inSet;
    inSet.reserve(entities.size());
    for (const ECS::Entity entity : entities)
        inSet.insert(EntityKey(entity.index, entity.generation));

    const auto &registry = Core::Reflect::ComponentRegistry::Instance();
    for (std::size_t i = 0; i < entities.size(); ++i)
    {
        const ECS::Entity entity = entities[i];
        for (const Core::Reflect::ComponentMeta *meta : registry.SerializableComponents())
        {
            const void *component = meta->getByEntity(&scene, entity.index, entity.generation);
            if (component == nullptr)
                continue;

            for (const Core::Reflect::FieldMeta &field : meta->fields)
            {
                // A transient field is not written at all, so it loses nothing here.
                if (field.type != Core::Reflect::FieldType::EntityRef || field.transient)
                    continue;

                const auto target = *reinterpret_cast<const ECS::Entity *>(
                    static_cast<const char *>(component) + field.offset);
                if (target == ECS::NullEntity || inSet.contains(EntityKey(target.index, target.generation)))
                    continue;

                report(*meta, field, i, target);
            }
        }
    }
}

// Clears s_context on every exit path, including a component
// serialize/deserialize throwing mid-pass (malformed field data reaches
// j.at(...)/_v[i].get<T>() in generated code). Without it a throw leaves the
// context engaged and the next EntityToRef / RefToEntity resolves against a
// stale, half-populated one.
struct ScopedContextReset
{
    ~ScopedContextReset() { s_context.reset(); }
};

} // namespace Assisi::Runtime
