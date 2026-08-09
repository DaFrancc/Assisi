/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SceneSerializerContext.hpp
/// @brief The thread-local context a save or load runs inside.
///
/// This was an anonymous-namespace block while the serializer was one
/// translation unit. The context is genuinely shared — the reference hooks,
/// Save, Load and every instance path read and write it — so it is `inline`
/// here rather than one copy per TU.

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
    /// Two callers, two vocabularies, and mixing them is the kind of bug that
    /// produces a plausible scene rather than an error — so the mode is explicit.
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
    /// rather than thrown where they are found: RefToEntity runs inside a
    /// component's generated deserialize, which knows neither the file nor the
    /// entity it is speaking for. Load reports them together and refuses the file.
    std::vector<std::string> unresolvedRefNames;
};

inline thread_local std::optional<SerializationContext> s_context;

// Raw-entity (identity) context — engaged by ScopedRawEntityContext. When set,
// EntityRef fields map through raw slot indices against this scene instead of the
// Save/Load remap table. Mutually exclusive with s_context by construction (the
// editor never captures/applies mid-Save/Load), and asserted non-reentrant.
inline thread_local ECS::Scene *s_rawContextScene = nullptr;

inline uint64_t EntityKey(uint32_t idx, uint32_t gen)
{
    return (static_cast<uint64_t>(gen) << 32) | idx;
}

/// A packed EntityRef, the way BinaryCodec spells one: index low, generation high.
/// The same packing EntityKey uses, named separately because it means something
/// different — this one crosses the codec's reference hook.
constexpr uint64_t PackEntity(ECS::Entity entity)
{
    return (static_cast<uint64_t>(entity.generation) << 32) | entity.index;
}

// Tears down the thread-local context on every exit path, including a
// component serialize/deserialize throwing mid-pass (malformed field data
// reaches j.at(...)/_v[i].get<T>() in generated code). Without it the context
// would stay engaged after a throw and a later EntityToIndex / IndexToEntity
// call would resolve against a stale, half-populated context.
struct ScopedContextReset
{
    ~ScopedContextReset() { s_context.reset(); }
};

} // namespace Assisi::Runtime
