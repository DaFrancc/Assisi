/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#pragma once

#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/ECS/Entity.hpp>
#include <Assisi/NetSync/NetProtocol.hpp>

#include <cstdint>

/// Key-packing shared by the Replication translation units.
///
/// These were one anonymous-namespace block while ReplicationServer and
/// ReplicationClient shared a single file. They are `inline` rather than
/// `static` so the split does not hand each TU its own copy.
namespace Assisi::NetSync
{

/// Entity handles are (index, generation); the maps here want one integer key.
///
/// **The layout matches `BinaryCodec`'s exactly — index low, generation high.**
/// That is not tidiness: the codec hands `entityToWire`/`entityFromWire` a
/// handle it packed itself, so a second convention here silently swaps the two
/// halves of every entity reference that crosses the wire. It read correctly
/// only for handles whose index happens to equal their generation — which
/// includes `{0, 0}`, the first entity in any scene, and is exactly why nothing
/// noticed until a message referenced a second one.
inline std::uint64_t PackEntity(ECS::Entity entity)
{
    return static_cast<std::uint64_t>(entity.index) | (static_cast<std::uint64_t>(entity.generation) << 32);
}

/// `(netId, componentId)` as one sortable integer. The component-set diff that
/// finds removals is a set_difference over these, so they must order by entity
/// first and component second — which the shift gives for free.
inline std::uint64_t PackComponentRef(NetId netId, Core::Reflect::ComponentId componentId)
{
    // .value on both: packing into a sortable integer, not a NetId/ComponentId
    // operation.
    return (static_cast<std::uint64_t>(netId.value) << 32) | static_cast<std::uint64_t>(componentId.value);
}

/// The component half of a packed ref. There is deliberately no NetId half: the
/// only consumer is the removal diff, which already knows the entity it is
/// diffing and needs the component out of each pair.
inline Core::Reflect::ComponentId ComponentIdOfRef(std::uint64_t packed)
{
    // packed key boundary: unpacking a sortable integer back into an id.
    return Core::Reflect::ComponentId{static_cast<std::uint32_t>(packed & 0xFFFFFFFFull)};
}

inline ECS::Entity UnpackEntity(std::uint64_t packed)
{
    return ECS::Entity{static_cast<std::uint32_t>(packed & 0xFFFFFFFFull), static_cast<std::uint32_t>(packed >> 32)};
}

} // namespace Assisi::NetSync
