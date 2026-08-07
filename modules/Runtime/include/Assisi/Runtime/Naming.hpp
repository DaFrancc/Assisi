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
/// This file is the polite half: it hands out names that satisfy the rules. The
/// refusals are elsewhere and stay there, because a rule that only holds when
/// callers remember it is the bug S17 actually was — see
/// `SceneSerializer::PlaceInstance` and load's pass 1.

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

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
    TooLong,           ///< Past `Core::kShortStringMax`; refused, never truncated.
    ContainsSeparator, ///< Holds a `/`, which belongs to the addressing scheme.
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
template <typename TakenFn>
[[nodiscard]] std::string UniqueName(std::string_view stem, TakenFn &&taken)
{
    // Walks until it finds a free one rather than counting what exists: `car_2`
    // may be free while three cars are live, and naming the fourth `car_3`
    // because there are three of them would collide with the one already called
    // that.
    std::string candidate{stem};
    for (std::uint32_t suffix = 1; taken(std::string_view{candidate}); ++suffix)
        candidate = std::string{stem} + "_" + std::to_string(suffix);
    return candidate;
}

/// @brief Whether an entity other than @p except already answers to @p name.
///
/// @p except is what makes renaming an entity to the name it already has a
/// no-op rather than a conflict with itself.
[[nodiscard]] bool EntityNameTaken(ECS::Scene &scene, std::string_view name,
                                   ECS::Entity except = ECS::NullEntity);

/// @brief @p stem, or the first `stem_N` no entity in @p scene is using.
[[nodiscard]] std::string UniqueEntityName(ECS::Scene &scene, std::string_view stem);

} // namespace Assisi::Runtime
