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

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Assisi::Runtime
{

struct SerializationContext
{
    /// How an EntityRef field addresses its target while this context is live.
    /// Explicit because mixing the two vocabularies produces a plausible scene
    /// rather than an error.
    enum class RefMode
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
