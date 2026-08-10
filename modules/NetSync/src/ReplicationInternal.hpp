/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#pragma once

#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/ECS/Entity.hpp>
#include <Assisi/NetSync/NetProtocol.hpp>

#include <cstdint>

/// Key-packing shared by the Replication translation units. Private to them —
/// this header is in src/, not include/.
namespace Assisi::NetSync
{

/// An entity handle as one integer key, for the maps here.
///
/// **The layout must match `BinaryCodec`'s — index low, generation high.** The
/// codec hands the entity hooks a handle it packed itself, so a second
/// convention silently swaps both halves of every entity reference on the wire,
/// and reads correctly only where index happens to equal generation.
inline std::uint64_t PackEntity(ECS::Entity entity)
{
    return static_cast<std::uint64_t>(entity.index) | (static_cast<std::uint64_t>(entity.generation) << 32);
}

/// `(netId, componentId)` as one sortable integer.
///
/// The removal diff is a set_difference over these, so the order has to be
/// entity first, component second — which the shift gives for free.
inline std::uint64_t PackComponentRef(NetId netId, Core::Reflect::ComponentId componentId)
{
    return (static_cast<std::uint64_t>(netId.value) << 32) | static_cast<std::uint64_t>(componentId.value);
}

/// The component half of a packed ref. No NetId half exists because the removal
/// diff, its only caller, already knows the entity.
inline Core::Reflect::ComponentId ComponentIdOfRef(std::uint64_t packed)
{
    return Core::Reflect::ComponentId{static_cast<std::uint32_t>(packed & 0xFFFFFFFFull)};
}

inline ECS::Entity UnpackEntity(std::uint64_t packed)
{
    return ECS::Entity{static_cast<std::uint32_t>(packed & 0xFFFFFFFFull), static_cast<std::uint32_t>(packed >> 32)};
}

} // namespace Assisi::NetSync
