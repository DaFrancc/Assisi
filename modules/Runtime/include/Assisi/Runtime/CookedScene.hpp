/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file CookedScene.hpp
/// @brief A level or blueprint as bytes: the binary form of what `.alvl` and
///        `.abp` say in JSON.
///
/// The file format addresses entities by name, because a name survives an edit
/// that inserts an entity above it and a position does not. The cooked form
/// keeps exactly that property and drops the cost: every name in the file is
/// written once into a table, and everything that would have spelled a name —
/// an entity's own, an `EntityRef` field's target, a member path an override
/// addresses — is an index into it.
///
/// **Component blocks are the network codec's**, written through
/// `WriteComponent` with no mask, so a component becomes cookable by being
/// reflected. That reuse is why the blob leads with `ProtocolHash`: the codec
/// identifies a component by a dense `ComponentId` the registry assigns by
/// sorting names, which agrees across one build and nothing more. A cooked blob
/// outlives its build, so the hash is what stops a later one from reading each
/// block into whichever component now holds that id.
///
/// **An override stays a patch.** It is written as a masked block naming only
/// the fields the author actually set, because an override that became full
/// state would freeze the rest of the component at the values the blueprint
/// happened to have on the day it was cooked — and a later fix to the blueprint
/// would stop reaching it, which is the whole property the format exists for.
///
/// A cooked scene loads by turning back into its document (CookedSceneToDocument)
/// and going through the one loader levels have.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Core/AssetProvider.hpp>
#include <Assisi/Core/Reflect/ComponentId.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Runtime/LevelError.hpp>
// LevelHeader and InstanceTable, which the writer takes for the same reason
// SceneSerializer::Save does: a Scene carries neither.
#include <Assisi/Runtime/SceneSerializer.hpp>

namespace Assisi::Runtime
{

/// @brief One component of one entity, as the codec wrote it.
///
/// The block still carries its own `ComponentId` prefix, so a reader hands the
/// whole thing to `ReadComponentId` and then `ReadComponent` exactly as the
/// blueprint fast path does with a prepared block.
struct CookedComponent
{
    std::vector<std::byte> block;
    Core::Reflect::ComponentId id;
};

/// @brief One entity: which name it answers to, and what it carries.
struct CookedEntity
{
    std::vector<CookedComponent> components;

    /// Index into CookedScene::names. Every `EntityRef` inside the blocks above
    /// is one of these too, which is what lets a reference point forward.
    std::uint32_t nameIndex = 0;
};

/// @brief One thing an instance changed about one of its members.
struct CookedOverride
{
    /// A masked component block — only the fields the author set. Empty when
    /// @ref absent is true.
    std::vector<std::byte> block;

    /// The member this addresses, as a path within the instance (`wheel_fl`,
    /// `car/body`). Stored as an index into CookedScene::names.
    std::uint32_t memberNameIndex = 0;

    /// Which component, by index into CookedScene::componentNames.
    std::uint32_t componentNameIndex = 0;

    /// The instance does not have this component at all — the file's `null`.
    /// Distinct from a block with an empty mask, which means "has it, changed
    /// nothing".
    bool absent = false;
};

/// @brief Version of a scene blob's own layout, separate from the envelope's.
///
/// Part of the scene cooker's cache key, so bumping it re-cooks every level.
inline constexpr std::uint8_t kScenePayloadVersion = 2;

/// @brief The id of the blueprint a level names by path, or nil when there is none.
///
/// The cook passes its database lookup, so a level naming a blueprint that does
/// not exist fails the cook instead of shipping an instance nothing can expand.
using BlueprintIdOf = std::function<Core::AssetId(std::string_view source)>;

/// @brief One blueprint instance the level places.
struct CookedInstance
{
    ECS::Transform transform;

    std::string name;

    /// The blueprint's virtual path. A path rather than an id because a path is a
    /// blueprint's identity everywhere a live instance is asked what it is — a
    /// spawn names one, a typed view checks one, replication sends one.
    std::string source;

    /// Member paths this instance does not have.
    std::vector<std::string> removed;
    std::vector<CookedOverride> overrides;
};

/// @brief A decoded cooked scene — the shape a loader would build from.
struct CookedScene
{
    /// Every name the file spells, entity names first and member paths after,
    /// in the order the writer assigned them.
    std::vector<std::string> names;

    /// Component names, so a reader can report what it could not find without
    /// resolving a `ComponentId` it may not have.
    std::vector<std::string> componentNames;

    std::vector<std::string> systems;
    std::vector<CookedEntity> entities;
    std::vector<CookedInstance> instances;

    /// The `ProtocolHash` the blob was written against.
    std::uint64_t protocolHash = 0;
};

/// @brief Encode @p scene and its instances as a cooked blob.
///
/// Mirrors SceneSerializer::Save exactly in what it includes and what it leaves
/// out — entities carrying `EditorOnly` are skipped, and a blueprint member is
/// described by its instance entry rather than written as an entity — so a
/// cooked level and a saved one describe the same world.
///
/// @p idOf turns each instance's blueprint path into the id the blob stores.
///
/// @return the bytes, or why the scene could not be encoded. A component whose
///         fields the codec would drop (see the `norep` rule) is refused rather
///         than written short: a cooked level missing a field is a level that
///         loads and is quietly wrong. An instance whose blueprint @p idOf cannot
///         name is BlueprintUnusable, for the same reason.
[[nodiscard]] std::expected<std::vector<std::byte>, LevelError>
SaveCookedScene(ECS::Scene &scene, const LevelHeader &header, InstanceTable *instances, const BlueprintIdOf &idOf);

/// @brief Parse a cooked blob back into its parts.
///
/// A parser, not a loader: it validates framing and hands back the blocks
/// without touching a scene. What proves the format round-trips is decoding
/// these blocks into components and comparing against the source level.
///
/// @return the decoded scene, or why the bytes are not one. A blob whose
///         `ProtocolHash` is not this build's is refused before any block is
///         read — every one of them would otherwise decode into whichever
///         component now holds its id.
[[nodiscard]] std::expected<CookedScene, LevelError> DecodeCookedScene(std::span<const std::byte> bytes);

/// @brief The level document @p cooked was cooked from: the same JSON
///        SceneSerializer::Save writes for that scene.
///
/// This is how a cooked level loads. The blueprint rules — the override merge,
/// reference qualification, removals — are written once, against the document,
/// and a cooked level that re-implemented them would be a second opinion about
/// what a level means. What cooking removes is the text parse; the blocks decode
/// through the codec.
///
/// Override references were qualified with their instance's name when cooked;
/// they come back in the form an author writes them, a plain name for a member
/// of the instance and a leading `/` for an entity of the level.
///
/// @return the document, or MalformedBlob if a block does not decode against
///         this build's components.
[[nodiscard]] std::expected<nlohmann::json, LevelError> CookedSceneToDocument(const CookedScene &cooked);

/// @brief The level or blueprint document at @p vpath, read from the cooked blob
///        @p provider holds for it: the reader a game reading a pak installs.
///
/// @return the document, FileUnreadable if @p provider has no such path or cannot
///         read it, or why the blob is not a scene this build loads.
[[nodiscard]] std::expected<nlohmann::json, LevelError> ReadCookedDocument(const Core::AssetProvider &provider,
                                                                           std::string_view vpath);

} // namespace Assisi::Runtime
