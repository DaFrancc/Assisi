/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ResourceStore.hpp
/// @brief One value of each type, belonging to a world rather than to an entity.

#include <memory>
#include <typeindex>
#include <unordered_map>

namespace Assisi::App
{

/// @brief Values a world has one of, such as which network client this machine
///        is: facts about the world as a whole, which no entity carries.
///
/// Reached from a system through `ctx.world.resources`. One value per type, made
/// the first time it is asked for, so the type is the key and nothing is looked
/// up by name.
class ResourceStore
{
public:
    /// @brief The world's @p T, made with its default value the first time.
    template <typename T> T &Get()
    {
        std::unique_ptr<Holder> &slot = _values[std::type_index(typeid(T))];
        if (slot == nullptr)
        {
            slot = std::make_unique<Value<T>>();
        }
        return static_cast<Value<T> &>(*slot).value;
    }

    /// @brief The world's @p T, or null when nothing has made one.
    template <typename T> [[nodiscard]] const T *Find() const
    {
        const ValueMap::const_iterator found = _values.find(std::type_index(typeid(T)));
        if (found == _values.end())
        {
            return nullptr;
        }
        return &static_cast<const Value<T> &>(*found->second).value;
    }

    /// @brief Drops every value, so each is made afresh when next asked for.
    void Clear() { _values.clear(); }

private:
    /// What the map owns: a value of a type only its key knows.
    struct Holder
    {
        virtual ~Holder() = default;
    };

    template <typename T> struct Value final : Holder
    {
        T value{};
    };

    using ValueMap = std::unordered_map<std::type_index, std::unique_ptr<Holder>>;

    ValueMap _values;
};

} // namespace Assisi::App
