/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file InstanceRecord.hpp
/// @brief A replicated blueprint instance as the client receives it, and the expander that builds it.
///
/// The two halves of this protocol are one design and must be read together:
/// a change to either one's wire handling is a change to both.

#include <Assisi/ECS/Entity.hpp>
#include <Assisi/ECS/InstanceId.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Core/Reflect/ComponentMask.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/Net/NetTransport.hpp>
#include <Assisi/NetSync/BodyState.hpp>
#include <Assisi/NetSync/InputCommand.hpp>
#include <Assisi/NetSync/MessageDispatch.hpp>
#include <Assisi/NetSync/NetClock.hpp>
#include <Assisi/NetSync/NetProtocol.hpp>
#include <Assisi/Physics/PhysicsWorld.hpp>

#include <typeindex>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace Assisi::NetSync
{

/// @brief One instance the server has named, as the client received it.
///
/// Keyed by `base` because that is the only handle the wire carries — member
/// blocks arrive as `base + memberIndex` and are attributed by range.
struct InstanceRecord
{
    std::uint32_t  blueprintIndex = 0;
    NetId          base;
    std::uint32_t  memberCount = 0;
    ECS::Transform placement;

    /// One entry per member, non-zero where the host actually has an entity for
    /// it. **Empty means "all of them"**, which is the ordinary case and costs a
    /// single bit on the wire.
    ///
    /// The block's width and the instance's *contents* are two different facts,
    /// and conflating them is B8: `memberCount` is the definition's count, fixed
    /// when the ids were handed out so a member destroyed later cannot shift its
    /// siblings' ids — while a member pruned on the host, or one the level
    /// removed from this instance, has no entity behind it at all. Without this
    /// nothing on the wire carried the difference, so every client expanded the
    /// full definition and held a live phantom at `base + prunedIndex` that no
    /// despawn named and no delta ever touched.
    std::vector<std::uint8_t> memberPresent;

    /// @brief Does the host have an entity for member @p index?
    [[nodiscard]] bool HasMember(std::uint32_t index) const
    {
        return memberPresent.empty() ||
               (index < memberPresent.size() && memberPresent[index] != 0u);
    }
};

/// @brief Turns a record into local entities, client-side.
///
/// Installed by App, which owns the blueprint cache and the manifest the
/// `blueprintIndex` indexes into. NetSync cannot do this itself — expansion is
/// Runtime's job and NetSync does not depend on Runtime.
///
/// This is what makes blueprint replication cheaper than sending entities: the
/// client builds every member from the blueprint, so the server never has to
/// send what the file already says.
class InstanceExpander
{
  public:
    virtual ~InstanceExpander() = default;

    InstanceExpander()                                    = default;
    InstanceExpander(const InstanceExpander &)            = delete;
    InstanceExpander &operator=(const InstanceExpander &) = delete;

    /// @brief Expand @p record locally, appending its members to @p out **in
    /// member order** — the order `baseNetId + i` indexes — and reporting the
    /// local instance id it created in @p outInstance.
    ///
    /// The id is what closes the tag's translation. A `BlueprintMember` arriving
    /// over the wire carries the *server's* instance id, which names nothing
    /// here; the codec rewrites it through this, so the tag means the same thing
    /// on both machines. An expander that has no instance table may leave
    /// @p outInstance default and the tag will simply stay unresolved.
    ///
    /// Returning false, or the wrong number of members, is fatal to the session
    /// rather than survivable: the client would otherwise bind member ids to the
    /// wrong entities and every later delta would land on the wrong one. A
    /// blueprint that fails to expand here has already passed the content-set
    /// hash, so this is a real disagreement about the file, not a missing asset.
    [[nodiscard]] virtual bool Expand(const InstanceRecord &record, std::vector<ECS::Entity> &out,
                                      ECS::InstanceId &outInstance) = 0;
};

} // namespace Assisi::NetSync
