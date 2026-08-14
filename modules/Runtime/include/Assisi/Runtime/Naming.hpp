/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Naming.hpp
/// @brief What may be called what, and how to pick a free name.
///
/// Names are addresses. An entity's name is how a reference reaches it, and an
/// instance's name is the prefix its members are addressed under — `car_3/body`.
/// At load both live as keys in one map, which is what lets a level entity point
/// into an instance at all, and what makes a duplicate unloadable rather than
/// merely confusing: the loader refuses a name claimed twice, so a level saved
/// with one is a level that never opens again.
///
/// **Two rules, and the second falls out of the first.**
///
/// 1. A name contains no `/`. The separator belongs to the addressing scheme,
///    not to the things being addressed.
/// 2. A name is unique among its own kind — entities among entities, instances
///    among instances.
///
/// Given (1), an entity name can never collide with a member path, because every
/// member path contains a separator and no name does. So the two namespaces are
/// disjoint *by construction* rather than by anyone remembering to cross-check
/// them — which is why an entity called `car` and an instance called `car` are
/// fine, and why round-7 S17's suggestion to forbid that would have rejected
/// levels that were never in danger.
///
/// **Every write of a Name goes through a door here.** Rule 2 holds only if it
/// holds for every path that creates an entity, so no call site spells the write
/// itself. Which door depends on who is asking:
///
///  - **Give** (`GiveEntityName`, `NameBatch::Give`) — the caller is creating an
///    entity and the name is a starting point: an expanded member's leaf, a
///    migrated entity's old name, `Entity` for a fresh one. Steps to `body_1`
///    rather than failing a spawn over a label.
///  - **Rename** (`RenameEntity`, `CheckEntityName`) — a human typed this exact
///    string. Refused with a reason, since storing `crate_1` for a typed `crate`
///    is an edit nobody made.
///
/// A *file* naming two entities the same is refused outright instead — every
/// reference and override in it would be ambiguous. That check stays with the
/// loader: `SceneSerializer::Load` pass 1, and `PlaceInstance` for instances.

#include <algorithm>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>

#include <Assisi/Core/TrivialString.hpp>
#include <Assisi/ECS/Entity.hpp>

namespace Assisi::ECS
{
struct Scene;
}

namespace Assisi::Runtime
{

/// @brief What separates an instance's name from its member's in an address.
inline constexpr char kNameSeparator = '/';

/// @brief Why a string may not be used as a name.
enum class NameError
{
    Empty,             ///< Nothing to address it by.
    TooLong,           ///< Past `Core::kEntityNameMax`; refused, never truncated.
    ContainsSeparator, ///< Holds a `/`, which belongs to the addressing scheme.
    Taken,             ///< Another entity already answers to it.
};

/// @brief One line saying what is wrong, for a log or a field hint.
[[nodiscard]] std::string_view Describe(NameError error);

/// @brief Whether @p name may be given to an entity or an instance.
///
/// The reason comes back with the refusal because both callers need it: the
/// rename box says it under the field, and the loader puts it in the error that
/// refuses the file. A bool would make each of them re-derive what it already
/// knew.
///
/// Empty is not valid here, which is not the same as saying an entity must have
/// a name: an entity with no `Name` component is ordinary, and the serializer
/// gives it a placeholder at save time. This answers "may this string be a
/// name", asked of a string somebody actually typed.
[[nodiscard]] std::expected<void, NameError> ValidateName(std::string_view name);

/// @brief @p stem, or the first `stem_N` that @p taken reports free.
///
/// **Stateless on purpose.** It remembers nothing between calls and asks the
/// world instead, so `car` then `car_1` then `car_2` comes out of the caller
/// having actually created each one — not out of a counter. A counter drifts the
/// moment a name is handed out and not used, or the object is deleted, or an
/// undo puts the world back: it would offer `car_5` in a level with no cars.
/// The world cannot drift from itself.
///
/// The result always fits `Core::kEntityNameMax`, suffix included: a name
/// truncated on the way into storage is one that was checked in a spelling it
/// does not keep, and a truncated duplicate is still a duplicate.
template <typename TakenFn>
[[nodiscard]] std::string UniqueName(std::string_view stem, TakenFn &&taken)
{
    std::string base{stem.substr(0, std::min(stem.size(), Core::kEntityNameMax))};
    if (!taken(std::string_view{base}))
        return base;

    // Walks until it finds a free one rather than counting what exists: `car_2`
    // may be free while three cars are live, and naming the fourth `car_3`
    // because there are three of them would collide with the one already called
    // that.
    for (std::uint32_t suffix = 1;; ++suffix)
    {
        // The suffix is what must survive, so the stem is what gives way: at most
        // 11 bytes of `_4294967295`, which leaves 21 of any stem.
        const std::string tail = "_" + std::to_string(suffix);
        const std::string candidate =
            base.substr(0, std::min(base.size(), Core::kEntityNameMax - tail.size())) + tail;
        if (!taken(std::string_view{candidate}))
            return candidate;
    }
}

/// @brief Whether an entity other than @p except already answers to @p name.
///
/// @p except is what makes renaming an entity to the name it already has a
/// no-op rather than a conflict with itself.
[[nodiscard]] bool EntityNameTaken(ECS::Scene &scene, std::string_view name,
                                   ECS::Entity except = ECS::NullEntity);

/// @brief The Give door for many entities at once: one pass over the scene, then
///        a name each.
///
/// For callers naming entities by the dozen — an expansion names every member of
/// an instance, a migration every entity of a subtree — where `GiveEntityName`
/// would walk the whole scene per name.
///
/// **Goes stale.** It holds the scene's names as they were plus what it has
/// handed out, so it is valid only while nothing else creates or renames
/// entities. Build it, use it, drop it.
class NameBatch
{
public:
    /// @param rebuilding entities whose current names do not count as taken,
    ///        because this same batch is about to rename them. A re-expansion
    ///        passes its adopted members: without it each one is suffixed against
    ///        its own previous name and `body_1` drifts to `body_2`.
    explicit NameBatch(ECS::Scene &scene, std::span<const ECS::Entity> rebuilding = {});

    /// @brief Names @p entity @p stem, or the first free `stem_N`, and says which.
    std::string Give(ECS::Entity entity, std::string_view stem);

private:
    ECS::Scene &_scene;
    std::unordered_set<std::string> _taken;
};

/// @brief Names @p entity @p stem, or the first `stem_N` no entity is using.
/// @return the name it was actually given.
std::string GiveEntityName(ECS::Scene &scene, ECS::Entity entity, std::string_view stem);

/// @brief Whether @p entity may be renamed to exactly @p name.
///
/// Separate from RenameEntity for the rename box, which asks every frame and
/// writes only on a keystroke.
///
/// Empty is accepted and means "no name": an entity without one is ordinary.
/// ValidateName refuses it, because it answers for a *file*, where a nameless
/// entity is one no reference can reach.
[[nodiscard]] std::expected<void, NameError> CheckEntityName(ECS::Scene &scene, ECS::Entity entity,
                                                             std::string_view name);

/// @brief Renames @p entity to exactly @p name, or refuses and changes nothing.
[[nodiscard]] std::expected<void, NameError> RenameEntity(ECS::Scene &scene, ECS::Entity entity,
                                                          std::string_view name);

} // namespace Assisi::Runtime
