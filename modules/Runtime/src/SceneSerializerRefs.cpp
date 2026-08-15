/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Runtime/SceneSerializer.hpp>

#include <Assisi/Core/Assert.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/Runtime/NameComponent.hpp>
#include <Assisi/Runtime/Naming.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "SceneSerializerContext.hpp"

// ---------------------------------------------------------------------------
// Entity references, and moving entities between scenes.
// ---------------------------------------------------------------------------

namespace Assisi::Runtime
{

std::string_view Describe(LevelError error)
{
    switch (error)
    {
    case LevelError::FileUnreadable:
        return "the file could not be read";
    case LevelError::MalformedJson:
        return "the file is not readable JSON";
    case LevelError::UnsupportedVersion:
        return "the file is a version this build does not read";
    case LevelError::NoInstanceTable:
        return "the file places blueprint instances and this load has nowhere to put them";
    case LevelError::MissingName:
        return "an entity or instance in the file has no name";
    case LevelError::MissingSource:
        return "an instance in the file names no source";
    case LevelError::InvalidName:
        return "the file gives something a name that is not usable as one";
    case LevelError::DuplicateName:
        return "two things in the file answer to one name";
    case LevelError::NonUniformScale:
        return "an instance has a non-uniform scale";
    case LevelError::BlueprintUnusable:
        return "an instance names a blueprint that will not load";
    case LevelError::UnresolvedReference:
        return "a reference names something the file does not declare";
    case LevelError::MalformedComponent:
        return "a component field holds something the engine cannot read";
    case LevelError::ContextBusy:
        return "a serialization context is already active on this thread";
    case LevelError::InstanceNotLive:
        return "no such instance is live";
    case LevelError::NameAlreadyLive:
        return "an instance of that name is already live in this world";
    }
    return "the file cannot be used";
}

// ---------------------------------------------------------------------------
// Public context accessors (called from component serialize/addToScene lambdas)
// ---------------------------------------------------------------------------

nlohmann::json SceneSerializer::EntityToRef(ECS::Entity entity)
{
    // Null before any context check: every mode spells "no target" the same way.
    if (entity == ECS::NullEntity)
        return nullptr;

    // Raw-entity context wins: identity mapping, keyed by slot AND generation.
    // The generation is load-bearing — a bare slot index silently resolves to
    // whatever entity later reused the slot, which is perfectly alive, so no
    // liveness check can tell. Packed in, RefToEntity can reject the stale ref.
    if (s_rawContextScene != nullptr)
        return EntityKey(entity.index, entity.generation);

    const SerializationContext *context = ScopedContext::Current();
    if (context == nullptr)
        return nullptr;

    const uint64_t key = EntityKey(entity.index, entity.generation);

    if (context->mode == SerializationContext::RefMode::SetIndices)
    {
        const auto it = context->entityToIndex.find(key);
        // Out of the moved set: ~0ull, which RefToEntity reads as out of range and
        // nulls. TransferEntities has already warned about each of these.
        return it != context->entityToIndex.end() ? nlohmann::json(it->second) : nlohmann::json(~0ull);
    }

    const auto it = context->entityToName.find(key);
    return it != context->entityToName.end() ? nlohmann::json(it->second) : nlohmann::json(nullptr);
}

ECS::Entity SceneSerializer::RefToEntity(const nlohmann::json &value)
{
    if (value.is_null())
        return ECS::NullEntity;

    // Raw-entity context wins: the value is a packed (slot, generation) key. The
    // paired restore revives entities at their original handle, so the generation
    // matches exactly. A mismatch means the slot was recycled by an unrelated
    // entity: null the ref rather than redirect onto whoever moved in.
    if (s_rawContextScene != nullptr)
    {
        if (!value.is_number_unsigned())
            return ECS::NullEntity;
        const uint64_t key     = value.get<uint64_t>();
        const auto slot    = static_cast<uint32_t>(key & 0xFFFFFFFFull);
        const auto wantGen = static_cast<uint32_t>(key >> 32);
        const ECS::Entity live    = s_rawContextScene->EntityAt(slot);
        return live.generation == wantGen ? live : ECS::NullEntity;
    }

    SerializationContext *const context = ScopedContext::Current();
    if (context == nullptr)
        return ECS::NullEntity;

    if (context->mode == SerializationContext::RefMode::SetIndices)
    {
        if (!value.is_number_unsigned())
            return ECS::NullEntity;
        const uint64_t index = value.get<uint64_t>();
        return index < context->indexToEntity.size() ? context->indexToEntity[static_cast<std::size_t>(index)]
                                                     : ECS::NullEntity;
    }

    // A file ref is a name. Anything else is a v1 file or a hand-edit that meant a
    // position; guessing would resolve it to *some* entity, the failure names exist
    // to remove. Record it and let Load refuse the file.
    if (!value.is_string())
    {
        context->unresolvedRefNames.push_back(value.dump());
        return ECS::NullEntity;
    }

    // A leading slash means "the file that wrote this" — here, this file, so it
    // names what a plain ref names. Inside an instance, Blueprint's QualifyName
    // already applied the prefix; stripping it here makes the two spellings agree.
    auto name = value.get<std::string>();
    if (!name.empty() && name.front() == '/')
        name.erase(0, 1);

    const auto it = context->nameToEntity.find(name);
    if (it == context->nameToEntity.end())
    {
        context->unresolvedRefNames.push_back(std::move(name));
        return ECS::NullEntity;
    }

    // Declared but mapped at nothing: a member a removal took out, either on the
    // placement or in a file it instances. A legitimate thing for a file to say, so
    // the ref nulls with a warning instead of refusing the file the way an unknown
    // name does — and it says the same thing either way, because which of the two
    // removals it was is not something the reference can see (§6).
    if (it->second == ECS::NullEntity)
        Core::Log::Warn("SceneSerializer: a reference names '{}', which was removed — left null.", name);

    return it->second;
}

// ---------------------------------------------------------------------------
// TransferEntities (entity migration)
// ---------------------------------------------------------------------------

std::vector<ECS::Entity> SceneSerializer::TransferEntities(ECS::Scene &src, ECS::Scene &dst,
                                                           std::span<const ECS::Entity> entities)
{
    if (ScopedContext::Current() != nullptr || s_rawContextScene != nullptr)
    {
        Core::Log::Error("TransferEntities: a serialization context is already active on this thread.");
        return {};
    }
    if (entities.empty())
        return {};

    ScopedContext scoped;
    scoped->mode = SerializationContext::RefMode::SetIndices;

    // Source half of the remap: each migrated entity → its index within the set.
    // A ref to any entity NOT in this map serializes as ~0ull and resolves to
    // NullEntity on the destination side — refs that leave the set are nulled.
    for (uint32_t i = 0; i < entities.size(); ++i)
    {
        const ECS::Entity e = entities[i];
        scoped->entityToIndex[EntityKey(e.index, e.generation)] = i;
    }

    const auto &registry = Core::Reflect::ComponentRegistry::Instance();

    // Name the refs about to be dropped, before serialize loses them silently: a
    // non-null EntityRef pointing outside the migrated set becomes null. Entities
    // here have no names to give, so the message spells the handles out.
    ForEachRefLeavingSet(src, entities,
                         [entities](const Core::Reflect::ComponentMeta &meta,
                                    const Core::Reflect::FieldMeta &field, std::size_t owner,
                                    ECS::Entity target)
        {
            Core::Log::Warn("Migrate: {}::{} on entity (index {}, gen {}) references entity "
                            "(index {}, gen {}) outside the migrated set — it will be null "
                            "in the destination.",
                            meta.name, field.name, entities[owner].index,
                            entities[owner].generation, target.index, target.generation);
        });

    // Pass 1: serialize every migrated component while the source map is live, so
    // in-set EntityRefs capture their set index. Held per entity until pass 3.
    struct CapturedComponent
    {
        const Core::Reflect::ComponentMeta *meta;
        nlohmann::json data;
    };
    std::vector<std::vector<CapturedComponent>> captured(entities.size());
    for (std::size_t i = 0; i < entities.size(); ++i)
    {
        const ECS::Entity e = entities[i];
        for (const Core::Reflect::ComponentMeta *meta : registry.SerializableComponents())
        {
            if (const void *comp = meta->getByEntity(&src, e.index, e.generation))
                captured[i].push_back({meta, meta->serialize(comp)});
        }
    }

    // Pass 2: create the destination entities and record the destination half of
    // the remap. All of them first, so a ref to a sibling later in the set still
    // resolves in pass 3 (the same forward-ref handling as Load).
    std::vector<ECS::Entity> created;
    created.reserve(entities.size());
    scoped->indexToEntity.reserve(entities.size());
    for (std::size_t i = 0; i < entities.size(); ++i)
    {
        const ECS::Entity d = dst.Create();
        created.push_back(d);
        scoped->indexToEntity.push_back(d);
    }

    // Pass 3: deserialize each captured component into its destination entity.
    // RefToEntity now maps in-set refs onto the created handles.
    for (std::size_t i = 0; i < entities.size(); ++i)
    {
        for (const CapturedComponent &c : captured[i])
        {
            // The data came out of the source scene a moment ago, so a refusal
            // here is the codec disagreeing with itself, not a bad file. Loud, and
            // the migration continues: the entity has already moved and there is
            // nothing to roll back to.
            if (c.meta->addToScene &&
                !c.meta->addToScene(&dst, created[i].index, created[i].generation, c.data))
            {
                Core::Log::Error("SceneSerializer: migrating '{}' lost a component the source scene had "
                                 "written — this is an engine bug, not a bad file.",
                                 c.meta->name);
            }
        }
    }

    // Pass 4: the arrivals take names the destination is not already using. A Name
    // travels with every other component, so a migration into a world that has its
    // own `crate` would otherwise make two. The traveller is what steps aside;
    // the resident is what that world already addresses by name. `created`
    // is the batch's rebuilding set, or each arrival would be suffixed against the
    // name it just brought with it.
    NameBatch claims{dst, created};
    for (const ECS::Entity entity : created)
    {
        const Name *name = dst.Get<Name>(entity);
        if (name == nullptr || name->value.Empty())
            continue; // an entity that arrived without a name stays without one
        // Copied out first: Give writes its answer back into this same component.
        const std::string arrived{name->value.View()};
        (void)claims.Give(entity, arrived);
    }

    // The originals leave the source scene. Destroy only queues: they survive
    // until the source's next FlushDestroyed, and the caller is still responsible
    // for tearing down their Jolt bodies, which the ECS destroy does not touch.
    for (const ECS::Entity e : entities)
        src.Destroy(e);

    return created;
}

// ---------------------------------------------------------------------------
// ScopedRawEntityContext
// ---------------------------------------------------------------------------

SceneSerializer::ScopedRawEntityContext::ScopedRawEntityContext(ECS::Scene &scene)
{
    // One context per thread. Nesting raw-in-raw or raw-in-Save/Load is a bug: the
    // mappings are incompatible and neither the pointer nor the optional stacks.
    ASSISI_ASSERT((s_rawContextScene == nullptr && ScopedContext::Current() == nullptr),
                  "SceneSerializer: a serialization context is already active on this thread "
                  "(raw-entity and Save/Load contexts are mutually exclusive and non-reentrant).");
    s_rawContextScene = &scene;
}

SceneSerializer::ScopedRawEntityContext::~ScopedRawEntityContext()
{
    s_rawContextScene = nullptr;
}

} // namespace Assisi::Runtime
