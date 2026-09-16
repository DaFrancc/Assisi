/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Runtime/CookedScene.hpp>

#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/CookedBlob.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Reflect/BinaryCodec.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/ECS/BlueprintMember.hpp>
#include <Assisi/Runtime/Blueprint.hpp>
#include <Assisi/Runtime/EditorOnly.hpp>
#include <Assisi/Runtime/NameComponent.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "SceneSerializerContext.hpp"
#include "SceneSerializerHeader.hpp"
#include "SceneSerializerInstances.hpp"

// ---------------------------------------------------------------------------
// The binary form of a level or a blueprint.
// ---------------------------------------------------------------------------

namespace Assisi::Runtime
{

namespace
{

/// The component the entity's own name already carries; writing it again would
/// put the same string in two places with no rule about which wins. The JSON
/// path skips it for the same reason.
constexpr std::string_view kNameComponent = "Name";

/// Stands for a null `EntityRef` in the name table's index space.
///
/// One past the largest index a table can hold, rather than zero: zero is the
/// first real name, and a null that collided with it would wire every dangling
/// reference to whichever entity happened to be written first.
inline constexpr std::uint32_t kNullNameIndex = 0xFFFFFFFFu;

/// Whether a component survives the codec with everything a *file* holds.
///
/// `norep` means "to disk but never over the network", so the wire codec skips
/// such a field — right for replication and wrong here, where the destination is
/// disk. Nothing in the engine declares one; if one appears, cooking refuses it
/// rather than writing a level that loads and is quietly missing a value.
bool IsCodecLossless(const Core::Reflect::ComponentMeta &meta)
{
    for (const Core::Reflect::FieldMeta &field : meta.fields)
    {
        if (field.norep && !field.transient)
        {
            return false;
        }
    }
    return true;
}

/// Assigns each distinct name an index, in insertion order.
class NameTable
{
public:
    std::uint32_t Intern(const std::string &name)
    {
        const auto [slot, inserted] = _indices.try_emplace(name, static_cast<std::uint32_t>(_names.size()));
        if (inserted)
        {
            _names.push_back(name);
        }
        return slot->second;
    }

    [[nodiscard]] std::uint32_t IndexOf(const std::string &name) const
    {
        const auto found = _indices.find(name);
        return found == _indices.end() ? kNullNameIndex : found->second;
    }

    [[nodiscard]] const std::vector<std::string> &Names() const { return _names; }

private:
    std::vector<std::string> _names;
    std::unordered_map<std::string, std::uint32_t> _indices;
};

void WriteStringList(Core::BitWriter &writer, const std::vector<std::string> &values)
{
    writer.WriteVarUInt32(static_cast<std::uint32_t>(values.size()));
    for (const std::string &value : values)
    {
        writer.WriteString(value);
    }
}

/// Appends a component block with its byte length in front.
///
/// The prefix is what lets a parser hand back the block without decoding it: a
/// block's own framing is `[id][mask][payloads]`, whose length is only knowable
/// by walking the field table, so slicing one out otherwise means resolving the
/// component first. The message codec length-prefixes for the same reason.
///
/// It also byte-aligns each block, which is what makes the bytes handed back a
/// standalone buffer a reader can pass straight to ReadComponent.
void WriteBlock(Core::BitWriter &writer, const Core::BitWriter &block)
{
    const std::span<const std::byte> bytes = block.Data();
    writer.WriteVarUInt32(static_cast<std::uint32_t>(bytes.size()));
    writer.WriteBytes(bytes);
}

void WriteTransform(Core::BitWriter &writer, const ECS::Transform &transform)
{
    // The three reflected fields, in declaration order. `worldMatrix` is derived
    // by PropagateTransforms and is not one of them.
    writer.WriteFloat(transform.position.x);
    writer.WriteFloat(transform.position.y);
    writer.WriteFloat(transform.position.z);
    writer.WriteFloat(transform.rotation.w);
    writer.WriteFloat(transform.rotation.x);
    writer.WriteFloat(transform.rotation.y);
    writer.WriteFloat(transform.rotation.z);
    writer.WriteFloat(transform.scale.x);
    writer.WriteFloat(transform.scale.y);
    writer.WriteFloat(transform.scale.z);
}

/// The mask naming exactly the fields @p claim spells, so an override stays a
/// patch. A key the component does not have is refused rather than ignored: it
/// is an override that silently does nothing, which reads as a fix that did not
/// take.
std::expected<Core::Reflect::FieldMask, LevelError> MaskForClaim(const Core::Reflect::ComponentMeta &meta,
                                                                 const nlohmann::json &claim)
{
    Core::Reflect::FieldMask mask = 0;
    for (const auto &[key, value] : claim.items())
    {
        std::size_t codecIndex = 0;
        bool found             = false;
        for (const Core::Reflect::FieldMeta &field : meta.fields)
        {
            if (!Core::Reflect::IsWireField(field))
            {
                continue;
            }
            if (field.name == key)
            {
                mask |= Core::Reflect::FieldMaskBit(codecIndex);
                found = true;
                break;
            }
            ++codecIndex;
        }
        if (!found)
        {
            Core::Log::Error("CookedScene: an override names '{}::{}', which this build's component does not have.",
                             meta.name, key);
            return std::unexpected(LevelError::MalformedComponent);
        }
    }
    return mask;
}

} // namespace

std::expected<std::vector<std::byte>, LevelError> SaveCookedScene(ECS::Scene &scene, const LevelHeader &header,
                                                                  InstanceTable *instances, const BlueprintIdOf &idOf)
{
    if (s_rawContextScene != nullptr)
    {
        Core::Log::Error("CookedScene: cooking while a raw-entity context is active on this thread.");
        return std::unexpected(LevelError::ContextBusy);
    }

    auto &registry = Core::Reflect::ComponentRegistry::Instance();

    // Pass 1: the same collection Save does, and for the same reasons — a sorted
    // map so the order is a property of the scene rather than of the pool
    // layout, members excluded because their instance entry describes them, and
    // editor scaffolding skipped entirely.
    std::map<std::uint64_t, std::uint32_t> entityOrder; // key -> name index, filled in pass 2
    std::map<std::uint64_t, std::string> memberNames;

    for (const auto *meta : registry.SerializableComponents())
    {
        meta->iterateEntities(&scene, [&](std::uint32_t idx, std::uint32_t gen, const void *)
            {
                const ECS::Entity entity{idx, gen};
                if (scene.Has<EditorOnly>(entity))
                {
                    return;
                }
                if (const ECS::BlueprintMember *tag = scene.Get<ECS::BlueprintMember>(entity))
                {
                    if (instances != nullptr)
                    {
                        memberNames.emplace(EntityKey(idx, gen), MemberPathName(*instances, *tag));
                    }
                    return;
                }
                entityOrder.emplace(EntityKey(idx, gen), 0u);
            });
    }

    // Pass 2: name every entity before anything is encoded, because a reference
    // may point at one written later in the file.
    NameTable names;
    std::unordered_set<std::string> usedNames;
    usedNames.reserve(entityOrder.size());
    for (auto &[key, nameIndex] : entityOrder)
    {
        const ECS::Entity entity{static_cast<std::uint32_t>(key & 0xFFFFFFFFull),
                                 static_cast<std::uint32_t>(key >> 32)};
        nameIndex = names.Intern(UniqueName(AuthoredName(scene, entity), usedNames));
    }
    // Members after the entities, so a level entity never loses its own name to
    // one. They are addressed as `car_3/wheel_fl`, which no entity name can be.
    for (const auto &[key, path] : memberNames)
    {
        if (!path.empty())
        {
            names.Intern(path);
        }
    }

    // Every EntityRef becomes the index of the name its target answers to. The
    // codec's own hook does it during the write, so nothing walks an encoded
    // component afterwards looking for references to patch.
    std::unordered_map<std::uint64_t, std::uint32_t> keyToNameIndex;
    keyToNameIndex.reserve(entityOrder.size() + memberNames.size());
    for (const auto &[key, nameIndex] : entityOrder)
    {
        keyToNameIndex.emplace(key, nameIndex);
    }
    for (const auto &[key, path] : memberNames)
    {
        if (!path.empty())
        {
            keyToNameIndex.emplace(key, names.IndexOf(path));
        }
    }

    Core::Reflect::CodecContext codec;
    codec.entityToWire = [&keyToNameIndex](std::uint64_t packed) -> std::uint64_t
                         {
                             const auto found = keyToNameIndex.find(packed);
                             return found == keyToNameIndex.end() ? kNullNameIndex : found->second;
                         };

    // Pass 3: the entities themselves. Component names are interned too, so a
    // reader can say which component it could not find without first resolving
    // an id this build may have assigned to something else.
    NameTable componentNames;
    Core::BitWriter body;

    body.WriteVarUInt32(static_cast<std::uint32_t>(entityOrder.size()));
    for (const auto &[key, nameIndex] : entityOrder)
    {
        const ECS::Entity entity{static_cast<std::uint32_t>(key & 0xFFFFFFFFull),
                                 static_cast<std::uint32_t>(key >> 32)};

        // Collected before any of it is written, because the count goes first
        // and a component that refuses to encode must not leave a half-written
        // entity behind it.
        std::vector<std::pair<const Core::Reflect::ComponentMeta *, const void *>> present;
        for (const Core::Reflect::ComponentMeta *meta : registry.SerializableComponents())
        {
            if (meta->name == kNameComponent)
            {
                continue;
            }
            const void *component = meta->getByEntity(&scene, entity.index, entity.generation);
            if (component == nullptr)
            {
                continue;
            }
            if (!IsCodecLossless(*meta))
            {
                Core::Log::Error("CookedScene: '{}' holds a field the codec would not carry to disk.", meta->name);
                return std::unexpected(LevelError::CodecRefused);
            }
            present.emplace_back(meta, component);
        }

        body.WriteVarUInt32(nameIndex);
        body.WriteVarUInt32(static_cast<std::uint32_t>(present.size()));
        for (const auto &[meta, component] : present)
        {
            componentNames.Intern(meta->name);
            Core::BitWriter block;
            if (!Core::Reflect::WriteComponent(*meta, component, block, Core::Reflect::kAllFields, &codec))
            {
                Core::Log::Error("CookedScene: could not encode '{}'.", meta->name);
                return std::unexpected(LevelError::CodecRefused);
            }
            WriteBlock(body, block);
        }
    }

    // Pass 4: the instances, from the live table rather than from `header` — the
    // editor moves an instance by writing its row, and the file follows.
    std::vector<LevelInstance> placed;
    if (instances != nullptr)
    {
        placed = InstancesForSave(*instances);
    }

    body.WriteVarUInt32(static_cast<std::uint32_t>(placed.size()));
    for (const LevelInstance &entry : placed)
    {
        const Core::AssetId source = idOf ? idOf(entry.source) : Core::AssetId{};
        if (source.IsNil())
        {
            Core::Log::Error("CookedScene: instance '{}' names '{}', which has no asset id to cook it under.",
                             entry.name, entry.source);
            return std::unexpected(LevelError::BlueprintUnusable);
        }

        body.WriteString(entry.name);
        Core::WriteAssetId(body, source);
        WriteTransform(body, entry.transform);
        WriteStringList(body, entry.removed);

        // An override addresses `member -> component -> fields`. Flattened to one
        // list because the nesting carries nothing a reader needs: each row names
        // both halves of its own address.
        std::vector<std::pair<std::string, std::string>> claims;
        if (entry.overrides.is_object())
        {
            for (const auto &[memberPath, byComponent] : entry.overrides.items())
            {
                if (!byComponent.is_object())
                {
                    continue;
                }
                for (const auto &[componentName, claim] : byComponent.items())
                {
                    claims.emplace_back(memberPath, componentName);
                }
            }
        }

        body.WriteVarUInt32(static_cast<std::uint32_t>(claims.size()));
        for (const auto &[memberPath, componentName] : claims)
        {
            const nlohmann::json &claim = entry.overrides.at(memberPath).at(componentName);

            body.WriteVarUInt32(names.Intern(memberPath));
            body.WriteVarUInt32(componentNames.Intern(componentName));

            // `null` is the file's way of saying this instance does not have the
            // component at all, which a block with an empty mask does not mean.
            if (claim.is_null())
            {
                body.WriteBool(true);
                continue;
            }
            body.WriteBool(false);

            const Core::Reflect::ComponentMeta *meta = registry.Find(componentName);
            if (meta == nullptr || !meta->serializable)
            {
                Core::Log::Error("CookedScene: instance '{}' overrides '{}', which this build does not have.",
                                 entry.name, componentName);
                return std::unexpected(LevelError::MalformedComponent);
            }

            const std::expected<Core::Reflect::FieldMask, LevelError> mask = MaskForClaim(*meta, claim);
            if (!mask)
            {
                return std::unexpected(mask.error());
            }

            // The values themselves have to pass through a live component, since
            // the codec writes memory and the override is text. A scratch scene
            // whose entity indices *are* name-table indices makes the reference
            // hook an identity: a ref resolved against it already holds the index
            // the file wants.
            ECS::Scene scratch;
            SerializationContext scratchCtx;
            for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(names.Names().size()); ++i)
            {
                scratchCtx.nameToEntity.emplace(names.Names()[i], ECS::Entity{i, 0});
            }
            const ScopedContext scoped(std::move(scratchCtx));

            // Created up to the index this claim needs, so the handle the hook
            // packs is the index itself.
            ECS::Entity target;
            for (std::uint32_t i = 0; i <= static_cast<std::uint32_t>(names.Names().size()); ++i)
            {
                target = scratch.Create();
            }

            nlohmann::json wrapper{{componentName, claim}};
            QualifyInstanceReferences(wrapper, entry.name.empty() ? std::string{} : entry.name + "/");
            if (!meta->addToScene(&scratch, target.index, target.generation, wrapper.at(componentName)))
            {
                Core::Log::Error("CookedScene: instance '{}' overrides '{}' with a value that does not read.",
                                 entry.name, componentName);
                return std::unexpected(LevelError::MalformedComponent);
            }

            const void *component = meta->getByEntity(&scratch, target.index, target.generation);
            if (component == nullptr)
            {
                return std::unexpected(LevelError::MalformedComponent);
            }

            Core::Reflect::CodecContext identity;
            identity.entityToWire = [](std::uint64_t packed) -> std::uint64_t
                                    {
                                        // The scratch entity's index is already the
                                        // name index; its generation is zero.
                                        return packed & 0xFFFFFFFFull;
                                    };
            Core::BitWriter block;
            if (!Core::Reflect::WriteComponent(*meta, component, block, *mask, &identity))
            {
                return std::unexpected(LevelError::CodecRefused);
            }
            WriteBlock(body, block);
        }
    }

    // The envelope last, so the tables the body interned are complete. Framing
    // puts them ahead of it on the wire, which is what a reader needs.
    Core::BitWriter out;
    Core::WriteCookedHeader(out, Core::CookedKind::Scene);
    out.WriteUInt8(kScenePayloadVersion);
    out.WriteUInt64(Core::Reflect::ProtocolHash());
    WriteStringList(out, names.Names());
    WriteStringList(out, componentNames.Names());
    WriteStringList(out, header.systems);
    out.WriteBytes(body.Data());

    const std::span<const std::byte> bytes = out.Data();
    return std::vector<std::byte>{bytes.begin(), bytes.end()};
}

// ---------------------------------------------------------------------------
// Reading one back.
// ---------------------------------------------------------------------------

namespace
{

/// Reads a length-prefixed block into its own buffer.
///
/// Bounded by what is left in the reader before a single byte is allocated: the
/// length is attacker-controlled in the same sense the network's is, and a
/// cooked tree is as forgeable as any other file beside an executable.
bool ReadBlock(Core::BitReader &reader, std::vector<std::byte> &out)
{
    const std::uint32_t byteCount = reader.ReadVarUInt32();
    if (reader.Failed() || byteCount > reader.BitsRemaining() / 8u)
    {
        return false;
    }
    out.resize(byteCount);
    reader.ReadBytes(out);
    return !reader.Failed();
}

bool ReadStringList(Core::BitReader &reader, std::vector<std::string> &out)
{
    const std::uint32_t count = reader.ReadVarUInt32();
    // One character is at least a byte, so a count past the bits left cannot be
    // honest — checked before reserving, so a forged count allocates nothing.
    if (reader.Failed() || count > reader.BitsRemaining() / 8u)
    {
        return false;
    }
    out.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i)
    {
        out.push_back(reader.ReadString());
        if (reader.Failed())
        {
            return false;
        }
    }
    return true;
}

ECS::Transform ReadTransform(Core::BitReader &reader)
{
    ECS::Transform transform;
    transform.position.x = reader.ReadFloat();
    transform.position.y = reader.ReadFloat();
    transform.position.z = reader.ReadFloat();
    transform.rotation.w = reader.ReadFloat();
    transform.rotation.x = reader.ReadFloat();
    transform.rotation.y = reader.ReadFloat();
    transform.rotation.z = reader.ReadFloat();
    transform.scale.x    = reader.ReadFloat();
    transform.scale.y    = reader.ReadFloat();
    transform.scale.z    = reader.ReadFloat();
    return transform;
}

} // namespace

std::expected<CookedScene, LevelError> DecodeCookedScene(std::span<const std::byte> bytes)
{
    Core::BitReader reader{bytes};

    const std::expected<Core::CookedKind, Core::CookedBlobError> kind = Core::ReadCookedHeader(reader);
    if (!kind)
    {
        Core::Log::Error("CookedScene: {}.", Core::ToString(kind.error()));
        return std::unexpected(LevelError::MalformedBlob);
    }
    if (*kind != Core::CookedKind::Scene)
    {
        Core::Log::Error("CookedScene: these bytes are a {} blob, not a scene.", Core::ToString(*kind));
        return std::unexpected(LevelError::MalformedBlob);
    }

    const std::uint8_t version = reader.ReadUInt8();
    if (reader.Failed())
    {
        return std::unexpected(LevelError::MalformedBlob);
    }
    if (version != kScenePayloadVersion)
    {
        Core::Log::Error("CookedScene: layout version {}, but this build reads {}.", version, kScenePayloadVersion);
        return std::unexpected(LevelError::UnsupportedVersion);
    }

    CookedScene scene;
    scene.protocolHash = reader.ReadBits64(64);
    if (reader.Failed())
    {
        return std::unexpected(LevelError::MalformedBlob);
    }

    // Before a single block is read. The codec names a component by a dense id
    // the registry assigns by sorting names, so a build whose component set has
    // moved would read every block into whichever component now holds its id —
    // values that are wrong rather than missing.
    if (const std::uint64_t expected = Core::Reflect::ProtocolHash(); scene.protocolHash != expected)
    {
        Core::Log::Error("CookedScene: written against protocol {:016x}, but this build's is {:016x}.",
                         scene.protocolHash, expected);
        return std::unexpected(LevelError::ProtocolMismatch);
    }

    if (!ReadStringList(reader, scene.names) || !ReadStringList(reader, scene.componentNames) ||
        !ReadStringList(reader, scene.systems))
    {
        return std::unexpected(LevelError::MalformedBlob);
    }

    const auto nameCount = static_cast<std::uint32_t>(scene.names.size());

    const std::uint32_t entityCount = reader.ReadVarUInt32();
    if (reader.Failed() || entityCount > reader.BitsRemaining())
    {
        return std::unexpected(LevelError::MalformedBlob);
    }
    scene.entities.reserve(entityCount);
    for (std::uint32_t i = 0; i < entityCount; ++i)
    {
        CookedEntity entity;
        entity.nameIndex = reader.ReadVarUInt32();
        if (reader.Failed() || entity.nameIndex >= nameCount)
        {
            return std::unexpected(LevelError::MalformedBlob);
        }

        const std::uint32_t componentCount = reader.ReadVarUInt32();
        if (reader.Failed() || componentCount > reader.BitsRemaining())
        {
            return std::unexpected(LevelError::MalformedBlob);
        }
        entity.components.reserve(componentCount);
        for (std::uint32_t c = 0; c < componentCount; ++c)
        {
            CookedComponent component;
            if (!ReadBlock(reader, component.block))
            {
                return std::unexpected(LevelError::MalformedBlob);
            }
            // The id stays at the front of the block as well, so a reader hands
            // the whole buffer to ReadComponentId and then ReadComponent; this
            // copy is so a caller can dispatch without opening it twice.
            Core::BitReader peek{component.block};
            component.id = Core::Reflect::ReadComponentId(peek);
            if (peek.Failed())
            {
                return std::unexpected(LevelError::MalformedBlob);
            }
            entity.components.push_back(std::move(component));
        }
        scene.entities.push_back(std::move(entity));
    }

    const std::uint32_t instanceCount = reader.ReadVarUInt32();
    if (reader.Failed() || instanceCount > reader.BitsRemaining())
    {
        return std::unexpected(LevelError::MalformedBlob);
    }
    scene.instances.reserve(instanceCount);
    for (std::uint32_t i = 0; i < instanceCount; ++i)
    {
        CookedInstance instance;
        instance.name   = reader.ReadString();
        instance.source = Core::ReadAssetId(reader);
        if (reader.Failed())
        {
            return std::unexpected(LevelError::MalformedBlob);
        }
        instance.transform = ReadTransform(reader);
        if (reader.Failed() || !ReadStringList(reader, instance.removed))
        {
            return std::unexpected(LevelError::MalformedBlob);
        }

        const std::uint32_t overrideCount = reader.ReadVarUInt32();
        if (reader.Failed() || overrideCount > reader.BitsRemaining())
        {
            return std::unexpected(LevelError::MalformedBlob);
        }
        instance.overrides.reserve(overrideCount);
        for (std::uint32_t o = 0; o < overrideCount; ++o)
        {
            CookedOverride claim;
            claim.memberNameIndex    = reader.ReadVarUInt32();
            claim.componentNameIndex = reader.ReadVarUInt32();
            claim.absent             = reader.ReadBool();
            if (reader.Failed() || claim.memberNameIndex >= nameCount ||
                claim.componentNameIndex >= scene.componentNames.size())
            {
                return std::unexpected(LevelError::MalformedBlob);
            }
            if (!claim.absent && !ReadBlock(reader, claim.block))
            {
                return std::unexpected(LevelError::MalformedBlob);
            }
            instance.overrides.push_back(std::move(claim));
        }
        scene.instances.push_back(std::move(instance));
    }

    return scene;
}

} // namespace Assisi::Runtime
