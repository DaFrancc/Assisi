/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file ContainerOps.cpp
/// @brief The one part of the container machinery that is not a template: a
///        readable description of a container reached through type-erased ops.

#include <Assisi/Core/Reflect/ContainerOps.hpp>

#include <cstring>
#include <string>

#include <Assisi/Core/ShortString.hpp>

namespace Assisi::Core::Reflect
{
namespace
{

/// How many entries are spelled out before the rest become a count. An inspector
/// row is one line, and a hundred values on it are less legible than ten and a
/// remainder.
constexpr std::size_t kMaxDescribedEntries = 10;

template <typename T> T LoadPod(const std::byte *address)
{
    T value{};
    std::memcpy(&value, address, sizeof(T));
    return value;
}

/// The enumerator's name when the field's table has one for @p value, else the
/// number — a table describes the leaf, so it is absent for a non-enum element
/// and for an enum whose metadata did not travel.
std::string DescribeEnum(const FieldMeta &field, std::int64_t value)
{
    for (const EnumConstant &constant : field.enumConstants)
    {
        if (constant.value == value)
        {
            return constant.name;
        }
    }
    return std::to_string(value);
}

std::int64_t LoadEnumValue(const FieldMeta &field, const std::byte *address)
{
    switch (field.enumSize)
    {
    case 1:
        return field.enumSigned ? LoadPod<std::int8_t>(address) : LoadPod<std::uint8_t>(address);
    case 2:
        return field.enumSigned ? LoadPod<std::int16_t>(address) : LoadPod<std::uint16_t>(address);
    case 8:
        return LoadPod<std::int64_t>(address);
    default:
        return field.enumSigned ? LoadPod<std::int32_t>(address)
                                : static_cast<std::int64_t>(LoadPod<std::uint32_t>(address));
    }
}

std::string DescribeValue(const FieldMeta &field, FieldType type, const std::byte *address);

/// Walks one container level. @p field carries the leaf's enum table, which is
/// why it travels all the way down rather than being resolved at the top.
std::string DescribeLevel(const FieldMeta &field, const ContainerSpec &spec, const std::byte *address)
{
    const bool isMap = spec.keyType != FieldType::Unknown;

    struct Collector
    {
        const FieldMeta *field;
        const ContainerSpec *spec;
        std::string text;
        std::size_t seen;
        bool isMap;
    };

    Collector state{.field = &field, .spec = &spec, .text = {}, .seen = 0, .isMap = isMap};

    spec.ops->visit(address, &state,
                    [](void *context, const std::byte *key, const std::byte *value)
                    {
                        auto &collector = *static_cast<Collector *>(context);
                        if (collector.seen >= kMaxDescribedEntries)
                        {
                            ++collector.seen;
                            return;
                        }
                        if (collector.seen > 0)
                        {
                            collector.text += ", ";
                        }
                        if (collector.isMap && key != nullptr)
                        {
                            collector.text += DescribeValue(*collector.field, collector.spec->keyType, key);
                            collector.text += ": ";
                        }
                        if (collector.spec->element != nullptr)
                        {
                            collector.text += DescribeLevel(*collector.field, *collector.spec->element, value);
                        }
                        else
                        {
                            collector.text +=
                                DescribeValue(*collector.field, collector.spec->elementType, value);
                        }
                        ++collector.seen;
                    });

    if (state.seen > kMaxDescribedEntries)
    {
        state.text += ", … " + std::to_string(state.seen - kMaxDescribedEntries) + " more";
    }

    return isMap ? "{ " + state.text + " }" : "[" + state.text + "]";
}

std::string DescribeValue(const FieldMeta &field, FieldType type, const std::byte *address)
{
    switch (type)
    {
    case FieldType::Bool: return LoadPod<bool>(address) ? "true" : "false";
    case FieldType::Float: return std::to_string(LoadPod<float>(address));
    case FieldType::Double: return std::to_string(LoadPod<double>(address));
    case FieldType::Int8: return std::to_string(LoadPod<std::int8_t>(address));
    case FieldType::UInt8: return std::to_string(LoadPod<std::uint8_t>(address));
    case FieldType::Int16: return std::to_string(LoadPod<std::int16_t>(address));
    case FieldType::UInt16: return std::to_string(LoadPod<std::uint16_t>(address));
    case FieldType::Int32: return std::to_string(LoadPod<std::int32_t>(address));
    case FieldType::UInt32: return std::to_string(LoadPod<std::uint32_t>(address));
    case FieldType::Int64: return std::to_string(LoadPod<std::int64_t>(address));
    case FieldType::UInt64: return std::to_string(LoadPod<std::uint64_t>(address));
    case FieldType::String:
        return std::string(reinterpret_cast<const ShortString *>(address)->View());
    case FieldType::EntityName:
        return std::string(reinterpret_cast<const EntityName *>(address)->View());
    case FieldType::Enum: return DescribeEnum(field, LoadEnumValue(field, address));
    default: return "?";
    }
}

} // namespace

std::string DescribeContainer(const FieldMeta &field, const void *address)
{
    if (field.container == nullptr || field.container->ops == nullptr)
    {
        return "[]";
    }
    return DescribeLevel(field, *field.container, static_cast<const std::byte *>(address));
}

} // namespace Assisi::Core::Reflect
