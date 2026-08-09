/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SceneSerializerInstances.hpp
/// @brief Staging and committing one blueprint instance.
///
/// Load, PlaceInstance and ReexpandInstance all stage before they commit, so
/// these cross translation units and can no longer be anonymous.

#include <Assisi/Runtime/SceneSerializer.hpp>

#include <Assisi/Core/Assert.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Reflect/BinaryCodec.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/ECS/BlueprintMember.hpp>
#include <Assisi/Runtime/Blueprint.hpp>
#include <Assisi/Runtime/EditorOnly.hpp>
#include <Assisi/Runtime/Hierarchy.hpp>
#include <Assisi/Runtime/NameComponent.hpp>
#include <Assisi/Runtime/Naming.hpp>

#include <cmath>
#include <cstdint>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Assisi::Runtime
{

/// Everything one placed instance produced, kept so a later failure can undo it.
struct StagedInstance
{
    ECS::InstanceId id;

    /// Held, not borrowed. A load stages every instance before committing any of
    /// them, so this outlives the lookup that produced it — and on the async-travel
    /// worker, a blueprint save on the main thread can evict the cache entry in
    /// between.
    std::shared_ptr<const BlueprintDefinition> definition;

    ECS::Transform placement;

    /// Parallel to definition->members, with NullEntity where this instance
    /// removed one. A hole rather than a shorter list because the index *is* the
    /// NetId offset (§9): two instances of one file that removed different members
    /// must still agree about which index names which member.
    std::vector<ECS::Entity> members;

    /// This instance's view of each member, after its own overrides. Parallel to
    /// members; empty entries where there is a hole.
    std::vector<BlueprintMemberDesc> resolved;

    /// The instance's own claims, kept so the commit knows which components it may
    /// take off the fast path. A prepared block is *full state*, so decoding one
    /// over a component an override just set would undo the override.
    nlohmann::json overrides = nlohmann::json::object();
};

/// Which entities a re-expansion may take over instead of creating, keyed by the
/// member name they currently stand for.
///
/// Entries are **erased as they are adopted**, so what is left when staging ends is
/// exactly the set of members the edit deleted. That is the whole diff, and it falls
/// out of the staging walk rather than needing a second comparison that could
/// disagree with it.
struct AdoptionSet
{
    /// The row to keep, instead of allocating a new one. A re-expansion must land on
    /// the same instance id: every BlueprintMember tag in the world names it, and so
    /// does every transaction in the editor's history.
    ECS::InstanceId instanceId;

    std::unordered_map<std::string, ECS::Entity> byName;
};

/// @brief The name a saved entity is written under, before uniquing.
[[nodiscard]] std::string AuthoredName(ECS::Scene &scene, ECS::Entity entity);

/// @brief Make @p base unique against @p used, recording the result.
[[nodiscard]] std::string UniqueName(std::string base, std::unordered_set<std::string> &used);

/// @brief The `instance/member` path a member entity is saved under.
[[nodiscard]] std::string MemberPathName(const InstanceTable &instances, const ECS::BlueprintMember &tag);

/// @brief Build one instance's entities without committing them. See the
/// definition for what the caller owes on failure.
[[nodiscard]] std::expected<void, LevelError> StageInstance(ECS::Scene &scene, InstanceTable &table,
                                                            const LevelInstance &entry,
                                                            int32_t levelInstanceIndex,
                                                            StagedInstance &staged,
                                                            AdoptionSet *adopt = nullptr);

/// @brief Apply a staged instance to the scene.
void CommitInstance(ECS::Scene &scene, const StagedInstance &staged, std::string_view instanceName);

} // namespace Assisi::Runtime
