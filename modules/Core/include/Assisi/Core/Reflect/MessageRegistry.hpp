/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Reflect/MessageRegistry.hpp
/// @brief Singleton registry of all reflected message types.
///
/// The exact shape of ComponentRegistry, for the same reasons: generated code
/// registers from static initializers before main(), ids are dense and assigned
/// alphabetically at finalize, and the table is immutable once startup is over.
///
/// Two things are different, and both are deliberate. Duplicate *names* are a
/// hard error rather than a last-one-wins overwrite — two message types with the
/// same name would collide on a dense id and misdispatch across the wire, which
/// is silent corruption of exactly the kind the protocol hash exists to prevent.
/// And every registered message's layout, direction, and reliability go into
/// `ProtocolLayoutDescription()`, so adding a message, reordering its fields, or
/// reclassifying it moves the hash and mismatched builds refuse to pair.

#include <span>
#include <string_view>
#include <typeindex>
#include <unordered_map>
#include <vector>

#include <Assisi/Core/Reflect/MessageMeta.hpp>

namespace Assisi::Core::Reflect
{

class MessageRegistry
{
public:
    static MessageRegistry &Instance();

    /// @brief Register a message type. Called by generated code at startup.
    ///
    /// A second registration under an existing name is fatal: the two would
    /// share a dense id, and every packet naming it would reach the wrong
    /// handler on one of the two machines.
    void Register(MessageMeta meta);

    /// @brief Find a message by name, or nullptr.
    [[nodiscard]] const MessageMeta *Find(std::string_view name) const;

    /// @brief Every registered message, in ascending id order (alphabetical).
    [[nodiscard]] std::span<const MessageMeta> All() const;

    [[nodiscard]] std::size_t Count() const;

    /// @brief The dense id of a message by its C++ type, or kInvalidMessageId.
    ///
    /// @warning Runtime only, never from a static initializer — ids finalize
    /// lazily once every startup registration is in.
    [[nodiscard]] MessageId IdOf(std::type_index type) const;

    [[nodiscard]] MessageId IdOf(std::string_view name) const;

    /// @brief The message with the given id, or nullptr. Ids are dense from
    /// one, so this is an index.
    [[nodiscard]] const MessageMeta *ById(MessageId id) const;

private:
    MessageRegistry() = default;

    void EnsureFinalized() const;

    mutable std::vector<MessageMeta>                       _metas;
    mutable std::unordered_map<std::type_index, MessageId> _idByType;
    mutable bool _finalized = false;
};

} // namespace Assisi::Core::Reflect
