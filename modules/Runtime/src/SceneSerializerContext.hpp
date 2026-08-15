/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SceneSerializerContext.hpp
/// @brief The thread-local context a save, load or transfer runs inside, and the
///        entity packing its reference hooks use.
///
/// State every serializer translation unit shares — the reference hooks, Save,
/// Load and the instance paths all read and write it — so it lives in one
/// thread-local inside ScopedContext, one copy per thread rather than one per TU.
///
/// Valid only between the entry point that engages it and that call's return:
/// ScopedContext installs it and restores the previous one on every exit path,
/// and outside that window the hooks see no context and resolve every ref to
/// null. Engaging one is that guard's job alone — the storage is private to it,
/// so no entry point can half-write the pattern.
///
/// Contexts stack, so an entry point reached from inside another one no longer
/// destroys it. Whether it may *run* there is a separate question, and the line
/// is what it does to the caller's scene:
///
///   - Save and PrepareBlueprint only read it, so they nest. The outer context
///     comes back untouched.
///   - Load clears it, so it refuses (ScopedContext::Current, then
///     LevelError::ContextBusy): restoring would hand the outer context back its
///     tables naming entities the clear destroyed.
///   - TransferEntities, SaveEntitiesToFile and the blueprint expansions refuse
///     too, each through its own return type. Nesting them is not a thing any
///     caller has wanted.
///
/// The raw-entity context below is a separate axis and does not stack with
/// anything: the hooks check it *first*, so an entry point reached inside one
/// would resolve refs through it rather than through whatever it just installed.
/// Every entry point refuses or reports when it is live.

#include <Assisi/Runtime/SceneSerializer.hpp>

#include <Assisi/Core/Reflect/BinaryCodec.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
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

/// The one way to engage a serialization context.
///
/// Installs @p ctx for the scope and puts the previous one back on every exit
/// path — including a component serialize/deserialize throwing mid-pass, which
/// malformed field data does through j.at(...)/_v[i].get<T>() in generated code.
/// Anything less leaves a stale, half-populated context engaged for the next
/// EntityToRef / RefToEntity to read.
///
/// Restoring rather than blanking is what makes nesting safe; which entry points
/// may nest is the rule in this file's header comment.
class ScopedContext
{
public:
    explicit ScopedContext(SerializationContext ctx = {})
        : _outer(std::exchange(s_current, std::move(ctx)))
    {
    }
    ~ScopedContext() { s_current = std::move(_outer); }

    ScopedContext(const ScopedContext &)            = delete;
    ScopedContext &operator=(const ScopedContext &) = delete;
    ScopedContext(ScopedContext &&)                 = delete;
    ScopedContext &operator=(ScopedContext &&)      = delete;

    /// The *innermost* live context, which is this guard's own only while no
    /// inner one is engaged over it. Every use below is innermost where it runs.
    /// Const on the guard, not on what it hands back: the context lives in
    /// thread-local storage the guard merely owns the lifetime of, so a `const`
    /// guard still writes through to it.
    SerializationContext *operator->() const { return &*s_current; }

    /// The live context, or null when none is engaged — what the reference hooks
    /// ask, since they run under whichever entry point engaged one, and what an
    /// entry point that refuses to nest checks before it starts.
    [[nodiscard]] static SerializationContext *Current() { return s_current ? &*s_current : nullptr; }

private:
    static inline thread_local std::optional<SerializationContext> s_current;

    std::optional<SerializationContext> _outer;
};

// Raw-entity (identity) context — engaged by ScopedRawEntityContext. When set,
// EntityRef fields map through packed (slot, generation) keys against this scene
// instead of the Save/Load remap tables. Mutually exclusive with a serialization
// context and non-reentrant; ScopedRawEntityContext asserts both.
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
/// something it does not contain. What they must not do is null them silently:
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

} // namespace Assisi::Runtime
