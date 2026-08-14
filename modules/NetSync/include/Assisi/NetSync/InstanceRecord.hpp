/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file InstanceRecord.hpp
/// @brief A replicated blueprint instance as the client receives it, and the
///        expander that turns it into entities.

#include <Assisi/ECS/Entity.hpp>
#include <Assisi/ECS/InstanceId.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/NetSync/NetProtocol.hpp>

#include <cstdint>
#include <vector>

namespace Assisi::NetSync
{

/// @brief One instance the server has named, as the client received it.
///
/// Keyed by `base`: member blocks arrive as `base + memberIndex`, so the base id
/// is the only handle the wire carries.
struct InstanceRecord
{
    std::uint32_t blueprintIndex = 0;
    NetId base;
    std::uint32_t memberCount = 0;
    ECS::Transform placement;

    /// Which members the host actually has an entity for. **Empty means all of
    /// them** — the ordinary case, and one bit on the wire.
    ///
    /// Not the same fact as `memberCount`, which is the definition's width and
    /// stays fixed so destroying a member cannot shift its siblings' ids. Without
    /// this, a client expanded every member and kept a live phantom for any the
    /// host had pruned.
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
/// Installed by App, which owns the blueprint cache and the manifest that
/// `blueprintIndex` indexes into; NetSync does not depend on Runtime and cannot
/// expand anything itself.
///
/// This is why replicating a blueprint costs less than replicating its entities:
/// the client builds each member from the file.
class InstanceExpander
{
public:
    virtual ~InstanceExpander() = default;

    InstanceExpander()                                    = default;
    InstanceExpander(const InstanceExpander &)            = delete;
    InstanceExpander &operator=(const InstanceExpander &) = delete;

    /// @brief Expand @p record locally, appending its members to @p out **in
    ///        member order** — the order `base + i` indexes — and reporting the
    ///        instance id it created in @p outInstance.
    ///
    /// Order is not cosmetic: it is what binds each id to the right entity.
    /// Returning false, or the wrong number of members, ends the session rather
    /// than degrading it, because every later delta would land on the wrong
    /// entity.
    ///
    /// @p outInstance translates member tags, which arrive carrying the
    /// *server's* instance id. An expander with no instance table may leave it
    /// default; the tag then stays unresolved.
    [[nodiscard]] virtual bool Expand(const InstanceRecord &record, std::vector<ECS::Entity> &out,
                                      ECS::InstanceId &outInstance) = 0;

    /// @brief The instance @p localInstance named is finished: drop whatever the
    ///        expansion created **beside** the entities.
    ///
    /// Called once, with the id that Expand reported in `outInstance`, when the
    /// record is retired — every member despawned, or an expansion refused. Never
    /// called for an expander that left the id default, which named nothing.
    ///
    /// **The member entities are not yours to destroy here.** This side owns the
    /// bindings and the mirror bodies and has already torn both down by the time
    /// this runs; a second pass over the members would hand the same Jolt body to
    /// RemoveBody twice. What is left is the bookkeeping only the expander knows
    /// about — the instance table row App writes, which nothing else can reach.
    ///
    /// Not optional, and deliberately not defaulted to a no-op: an expander that
    /// records nothing writes an empty body, but it has to say so. The version of
    /// this interface without it leaked a row per instance for the length of a
    /// session (round-7 S5) precisely because the question was never asked.
    virtual void Collapse(ECS::InstanceId localInstance) = 0;
};

} // namespace Assisi::NetSync
