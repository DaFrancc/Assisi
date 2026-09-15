/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Reflect/ContainerOps.hpp
/// @brief How non-template code reaches the storage of a reflected container.
///
/// A codec walks a `FieldMeta` and a raw byte address. That works for a scalar,
/// whose bytes *are* the value, and it does not work for a `std::vector` or a
/// map, where the bytes are a handle onto an allocation only the concrete type
/// knows how to resize or iterate. Enumerating one concrete container per
/// `FieldType` is how `AssetPathVector` and `AssetIdVector` solved it, and it
/// does not scale: a parameterised container would need one enumerator per
/// element type, and a map one per key-and-element *pair*.
///
/// So the concrete type is captured once, at the point where it is still known —
/// reflectgen's generated code, which can name `decltype(Comp::member)` — into a
/// table of function pointers. Everything downstream works through that table
/// and never names a container type again.
///
/// A `ContainerSpec` chains: its `element` is non-null when the element is
/// itself a container, so a reader walks the chain instead of branching on a
/// depth. The depth *limit* is a check in one place rather than a shape anything
/// else assumes.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include <Assisi/Core/Reflect/FieldMeta.hpp>
#include <Assisi/Core/ShortString.hpp>
#include <Assisi/Core/TrivialString.hpp>

namespace Assisi::Core::Reflect
{

/// @brief Receives one entry during a traversal. @p key is null for a vector.
using ContainerVisitFn = void (*)(void *context, const std::byte *key, const std::byte *value);

/// @brief Fills the key buffer a map insert is about to use.
/// @return false to abandon the insert (a key that could not be decoded).
using ContainerReadKeyFn = bool (*)(void *context, std::byte *key);

/// @brief The operations a container supports, erased of its concrete type.
///
/// Every pointer takes the container's own address as raw bytes. `pushDefault`
/// is null for a map and `insert` is null for a vector; nothing else is ever
/// null on a spec that names a real container.
struct ContainerOps
{
    /// Entry count.
    std::size_t (*size)(const std::byte *container);

    /// Drops every entry, leaving the container empty and its storage reusable.
    void (*clear)(std::byte *container);

    /// Calls @p visitor once per entry. A vector visits in index order; a map
    /// visits **in sorted key order**, whatever order it iterates in itself,
    /// because an unordered_map's own order follows memory layout and would make
    /// the same contents encode differently from one run to the next.
    void (*visit)(const std::byte *container, void *context, ContainerVisitFn visitor);

    /// Appends a value-initialized element and returns its address. Vector only.
    std::byte *(*pushDefault)(std::byte *container);

    /// Builds a key through @p readKey, inserts it, and returns the address of
    /// the mapped value. Map only. Returns null if @p readKey failed or the key
    /// was already present — a duplicate key means the stream disagrees with
    /// itself, and silently keeping one of the two would hide that.
    std::byte *(*insert)(std::byte *container, void *context, ContainerReadKeyFn readKey);
};

/// @brief What a container field holds, and — when its element is itself a
/// container — what that holds, and so on down the chain.
struct ContainerSpec
{
    const ContainerOps *ops      = nullptr;
    /// Non-null when the element is itself a container. The chain ends at the
    /// first null, where `elementType` names a primitive.
    const ContainerSpec *element = nullptr;
    /// `FieldType::Unknown` for a vector, which has no key.
    FieldType keyType     = FieldType::Unknown;
    FieldType elementType = FieldType::Unknown;
};

/// @brief How many container levels a type has: 0 for a primitive, 1 for
///        `vector<int32_t>`, 2 for `map<ShortString, vector<int32_t>>`.
template <typename T> struct ContainerDepth
{
    static constexpr std::size_t value = 0;
};

/// @brief Deepest nesting a reflected field may declare.
///
/// Two levels, because an action bound to several keys is a list per name and
/// has no flatter spelling. Not more, because each level costs a recursion in
/// the codec and a row in the editor, and nothing in the engine wants a third.
/// reflectgen refuses a deeper field with a diagnostic; this backstops it for
/// anything that builds a spec by hand.
inline constexpr std::size_t kMaxContainerDepth = 2;

/// @brief The FieldType a C++ type reflects as. Unknown for anything unsupported,
///        which is what the static_asserts below turn into a readable failure.
template <typename T> constexpr FieldType FieldTypeOf()
{
    if constexpr (std::is_enum_v<T>)
    {
        return FieldType::Enum;
    }
    else if constexpr (std::is_same_v<T, bool>)
    {
        return FieldType::Bool;
    }
    else if constexpr (std::is_same_v<T, float>)
    {
        return FieldType::Float;
    }
    else if constexpr (std::is_same_v<T, double>)
    {
        return FieldType::Double;
    }
    else if constexpr (std::is_same_v<T, std::int8_t>)
    {
        return FieldType::Int8;
    }
    else if constexpr (std::is_same_v<T, std::uint8_t>)
    {
        return FieldType::UInt8;
    }
    else if constexpr (std::is_same_v<T, std::int16_t>)
    {
        return FieldType::Int16;
    }
    else if constexpr (std::is_same_v<T, std::uint16_t>)
    {
        return FieldType::UInt16;
    }
    else if constexpr (std::is_same_v<T, std::int32_t>)
    {
        return FieldType::Int32;
    }
    else if constexpr (std::is_same_v<T, std::uint32_t>)
    {
        return FieldType::UInt32;
    }
    else if constexpr (std::is_same_v<T, std::int64_t>)
    {
        return FieldType::Int64;
    }
    else if constexpr (std::is_same_v<T, std::uint64_t>)
    {
        return FieldType::UInt64;
    }
    else if constexpr (std::is_same_v<T, ShortString>)
    {
        return FieldType::String;
    }
    else if constexpr (std::is_same_v<T, EntityName>)
    {
        return FieldType::EntityName;
    }
    else if constexpr (ContainerDepth<T>::value > 0)
    {
        return ContainerDepth<T>::kind;
    }
    else
    {
        return FieldType::Unknown;
    }
}

/// @brief Whether a type may key a map: an integer width, or one of the inline
///        strings. Not a float (no stable text round-trip for a JSON object key),
///        not a bool, and not an enum (the one enum-metadata slot on a FieldMeta
///        describes the element).
template <typename T> constexpr bool IsValidMapKey()
{
    constexpr FieldType type = FieldTypeOf<T>();
    return type == FieldType::Int8 || type == FieldType::UInt8 || type == FieldType::Int16
           || type == FieldType::UInt16 || type == FieldType::Int32 || type == FieldType::UInt32
           || type == FieldType::Int64 || type == FieldType::UInt64 || type == FieldType::String
           || type == FieldType::EntityName;
}

template <typename T> const ContainerSpec *ContainerSpecFor();

namespace Detail
{

/// @brief The spec an element contributes: null for a primitive, its own chain
///        for a nested container.
template <typename Elem> const ContainerSpec *ElementSpec()
{
    if constexpr (ContainerDepth<Elem>::value > 0)
    {
        return ContainerSpecFor<Elem>();
    }
    else
    {
        return nullptr;
    }
}

/// @brief The type-erased operations for one concrete container type.
///
/// Specialized per container shape below. Each member is a plain static function
/// so its address is a function pointer with no capture and no indirection.
template <typename C> struct ContainerAccess;

template <typename T, typename A> struct ContainerAccess<std::vector<T, A>>
{
    using Container = std::vector<T, A>;

    // std::vector<bool> packs its elements into bits, so an element has no
    // address to hand out and none of this works. A bool list spells itself
    // std::vector<std::uint8_t>.
    static_assert(!std::is_same_v<T, bool>,
                  "std::vector<bool> has no addressable elements; use std::vector<std::uint8_t>");

    static std::size_t Size(const std::byte *container)
    {
        return reinterpret_cast<const Container *>(container)->size();
    }

    static void Clear(std::byte *container) { reinterpret_cast<Container *>(container)->clear(); }

    static void Visit(const std::byte *container, void *context, ContainerVisitFn visitor)
    {
        for (const T &element : *reinterpret_cast<const Container *>(container))
        {
            visitor(context, nullptr, reinterpret_cast<const std::byte *>(&element));
        }
    }

    static std::byte *PushDefault(std::byte *container)
    {
        Container &values = *reinterpret_cast<Container *>(container);
        values.emplace_back();
        return reinterpret_cast<std::byte *>(&values.back());
    }

    static const ContainerOps *Ops()
    {
        static constexpr ContainerOps ops{.size        = &Size,
                                          .clear       = &Clear,
                                          .visit       = &Visit,
                                          .pushDefault = &PushDefault,
                                          .insert      = nullptr};
        return &ops;
    }

    static constexpr FieldType kKeyType     = FieldType::Unknown;
    static constexpr FieldType kElementType = FieldTypeOf<T>();
    using Element                           = T;
};

/// @brief Shared body for both map flavours — they differ only in their template
///        parameter list, and nothing here depends on how they store entries.
template <typename M> struct MapAccess
{
    using Key     = typename M::key_type;
    using Mapped  = typename M::mapped_type;
    using Element = Mapped;

    static_assert(IsValidMapKey<Key>(),
                  "a reflected map key must be an integer width, ShortString or EntityName");

    static std::size_t Size(const std::byte *container)
    {
        return reinterpret_cast<const M *>(container)->size();
    }

    static void Clear(std::byte *container) { reinterpret_cast<M *>(container)->clear(); }

    static void Visit(const std::byte *container, void *context, ContainerVisitFn visitor)
    {
        const M &entries = *reinterpret_cast<const M *>(container);

        // Sorted, not native order. An unordered_map iterates in whatever order
        // its buckets happen to sit in, so the same contents would encode
        // differently between two runs of the same binary — which breaks delta
        // replication and makes a cooked asset's bytes unreproducible.
        std::vector<const typename M::value_type *> ordered;
        ordered.reserve(entries.size());
        for (const typename M::value_type &entry : entries)
        {
            ordered.push_back(&entry);
        }
        std::sort(ordered.begin(), ordered.end(),
                  [](const typename M::value_type *left, const typename M::value_type *right)
                  { return left->first < right->first; });

        for (const typename M::value_type *entry : ordered)
        {
            visitor(context, reinterpret_cast<const std::byte *>(&entry->first),
                    reinterpret_cast<const std::byte *>(&entry->second));
        }
    }

    static std::byte *Insert(std::byte *container, void *context, ContainerReadKeyFn readKey)
    {
        M &entries = *reinterpret_cast<M *>(container);

        Key key{};
        if (!readKey(context, reinterpret_cast<std::byte *>(&key)))
        {
            return nullptr;
        }

        const auto [position, inserted] = entries.try_emplace(key);
        if (!inserted)
        {
            return nullptr;
        }
        return reinterpret_cast<std::byte *>(&position->second);
    }

    static const ContainerOps *Ops()
    {
        static constexpr ContainerOps ops{.size        = &Size,
                                          .clear       = &Clear,
                                          .visit       = &Visit,
                                          .pushDefault = nullptr,
                                          .insert      = &Insert};
        return &ops;
    }

    static constexpr FieldType kKeyType     = FieldTypeOf<Key>();
    static constexpr FieldType kElementType = FieldTypeOf<Mapped>();
};

template <typename K, typename V, typename C, typename A>
struct ContainerAccess<std::map<K, V, C, A>> : MapAccess<std::map<K, V, C, A>>
{
};

template <typename K, typename V, typename H, typename E, typename A>
struct ContainerAccess<std::unordered_map<K, V, H, E, A>>
    : MapAccess<std::unordered_map<K, V, H, E, A>>
{
};

} // namespace Detail

template <typename T, typename A> struct ContainerDepth<std::vector<T, A>>
{
    static constexpr std::size_t value = 1 + ContainerDepth<T>::value;
    static constexpr FieldType kind    = FieldType::Vector;
};

template <typename K, typename V, typename C, typename A> struct ContainerDepth<std::map<K, V, C, A>>
{
    static constexpr std::size_t value = 1 + ContainerDepth<V>::value;
    static constexpr FieldType kind    = FieldType::Map;
};

template <typename K, typename V, typename H, typename E, typename A>
struct ContainerDepth<std::unordered_map<K, V, H, E, A>>
{
    static constexpr std::size_t value = 1 + ContainerDepth<V>::value;
    static constexpr FieldType kind    = FieldType::Map;
};

/// @brief A container's contents as one line of text, for a reader that has only
///        a FieldMeta and an address — the editor's inspector.
///
/// Recursive over the spec chain, so a nested element reads as a nested list:
/// `{ Jump: [Green], MoveForward: [Red, Blue] }`. An enum leaf is shown by
/// enumerator name when @p field carries the table, and by value otherwise.
///
/// Type-erased on purpose. The JSON path formats the same values, but it is
/// templated on the concrete container, and the inspector never has that type.
[[nodiscard]] std::string DescribeContainer(const FieldMeta &field, const void *address);

/// @brief The static-storage descriptor for one container type.
///
/// The returned pointer outlives every FieldMeta that copies it, which is what
/// lets a FieldMeta stay copyable while naming a shape it cannot own.
template <typename T> const ContainerSpec *ContainerSpecFor()
{
    using Access = Detail::ContainerAccess<T>;

    static_assert(ContainerDepth<T>::value > 0, "ContainerSpecFor names a container type");
    static_assert(ContainerDepth<T>::value <= kMaxContainerDepth,
                  "a reflected container nests at most one level deep");
    static_assert(Access::kElementType != FieldType::Unknown,
                  "a reflected container's element must be a primitive, an AENUM enum, or one "
                  "container of those");

    static const ContainerSpec spec{.ops         = Access::Ops(),
                                    .element     = Detail::ElementSpec<typename Access::Element>(),
                                    .keyType     = Access::kKeyType,
                                    .elementType = Access::kElementType};
    return &spec;
}

} // namespace Assisi::Core::Reflect
