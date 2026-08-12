/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file StrongId.hpp
/// @brief The opt-in marker for id wrappers that encode as a single varint.
///
/// Deliberately its own header rather than part of BitStream.hpp: the types that
/// opt in — ComponentId, MessageId, ECS::InstanceId, NetSync's NetId and
/// ClientId — are small, widely included, and have no other reason to know the
/// bit codec exists.

#include <concepts>
#include <type_traits>

namespace Assisi::Core
{

/// @brief Declares @p T an id whose wire form is its `value` as a varint.
///
/// Opt-in **by name**, not by shape. A structural test — "any aggregate with an
/// unsigned `value`" — would have admitted every unrelated wrapper that happens
/// to look like one, so `struct Health { std::uint32_t value; }` would silently
/// have become wire-encodable as an id. Nothing would break loudly; it would
/// just quietly be a thing nobody decided.
///
/// Each id specializes this in its own header, next to its definition, so the
/// declaration is visible where the type is read.
template <typename T> struct IsStrongId : std::false_type
{
};

/// @brief An id that has opted in *and* has the shape the encoding needs.
///
/// Both halves are required, and they fail in different places on purpose: a
/// type that never opted in fails at the call site that tried to encode it,
/// while a type that opted in and then drifted — `value` renamed, or a
/// constructor added that costs it aggregate initialization — fails at the
/// `static_assert` beside its own specialization.
///
/// `is_aggregate_v` is load-bearing rather than tidiness: `ReadVarId` builds its
/// result with `T{...}`.
template <typename T>
concept StrongId = IsStrongId<T>::value && std::unsigned_integral<decltype(T::value)> && std::is_aggregate_v<T>;

} // namespace Assisi::Core
