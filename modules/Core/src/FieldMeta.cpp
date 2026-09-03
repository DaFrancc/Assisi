/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Core/Reflect/FieldMeta.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace Assisi::Core::Reflect
{

namespace
{

const std::byte *FieldAddress(const void *object, std::size_t offset)
{
    return static_cast<const std::byte *>(object) + offset;
}

/// The sibling named @p name, or nullptr. Linear because a component has a
/// handful of fields and this runs on an inspector frame or a decoded packet, not
/// in a loop over entities.
const FieldMeta *FindField(std::span<const FieldMeta> fields, const std::string &name)
{
    const auto it = std::find_if(fields.begin(), fields.end(),
                                 [&](const FieldMeta &f) { return f.name == name; });
    return it == fields.end() ? nullptr : &*it;
}

} // namespace

bool ReadNumericField(const FieldMeta &field, const void *object, double &out)
{
    const void *address = FieldAddress(object, field.offset);
    // Every widening is written out, because -Wdouble-promotion reads an implicit
    // one as an accident. Here the whole comparison is meant to run in double.
    switch (field.type)
    {
    case FieldType::Float:  out = static_cast<double>(*static_cast<const float *>(address)); return true;
    case FieldType::Double: out = *static_cast<const double *>(address); return true;
    case FieldType::Int32:  out = *static_cast<const std::int32_t *>(address); return true;
    case FieldType::UInt32: out = *static_cast<const std::uint32_t *>(address); return true;
    case FieldType::Int64:  out = static_cast<double>(*static_cast<const std::int64_t *>(address)); return true;
    case FieldType::UInt64: out = static_cast<double>(*static_cast<const std::uint64_t *>(address)); return true;
    default: return false;
    }
}

bool WriteNumericField(const FieldMeta &field, void *object, double value)
{
    void *address = static_cast<std::byte *>(object) + field.offset;
    switch (field.type)
    {
    case FieldType::Float:  *static_cast<float *>(address)         = static_cast<float>(value); return true;
    case FieldType::Double: *static_cast<double *>(address)        = value; return true;
    case FieldType::Int32:  *static_cast<std::int32_t *>(address)  = static_cast<std::int32_t>(value); return true;
    case FieldType::UInt32: *static_cast<std::uint32_t *>(address) = static_cast<std::uint32_t>(value); return true;
    case FieldType::Int64:  *static_cast<std::int64_t *>(address)  = static_cast<std::int64_t>(value); return true;
    case FieldType::UInt64: *static_cast<std::uint64_t *>(address) = static_cast<std::uint64_t>(value); return true;
    default: return false;
    }
}

FieldBounds ResolveFieldBounds(const FieldMeta &field, std::span<const FieldMeta> siblings, const void *object)
{
    FieldBounds bounds;

    // A named bound that cannot be read is dropped, not defaulted. Zero is a
    // legal bound, so substituting it would clamp a value to zero and look like
    // the author asked for that.
    const auto resolve = [&](bool declared, const std::string &named, float literal, bool &has, double &value)
                         {
                             if (!declared)
                             {
                                 return;
                             }
                             if (named.empty())
                             {
                                 has   = true;
                                 value = static_cast<double>(literal);
                                 return;
                             }
                             const FieldMeta *source = FindField(siblings, named);
                             has = source != nullptr && object != nullptr &&
                                   ReadNumericField(*source, object, value);
                         };

    resolve(field.hasMin, field.minField, field.minValue, bounds.hasMin, bounds.minValue);
    resolve(field.hasMax, field.maxField, field.maxValue, bounds.hasMax, bounds.maxValue);
    return bounds;
}

bool SettleDependentBounds(std::span<const FieldMeta> fields, void *object)
{
    if (object == nullptr)
    {
        return false;
    }

    bool anyMoved = false;
    // One pass per field is enough for any chain that settles at all, and is the
    // stop for metadata that would otherwise not settle.
    for (std::size_t pass = 0; pass < fields.size(); ++pass)
    {
        bool movedThisPass = false;
        for (const FieldMeta &field : fields)
        {
            if (field.minField.empty() && field.maxField.empty())
            {
                continue;
            }

            double value = 0.0;
            if (!ReadNumericField(field, object, value))
            {
                continue;
            }

            const FieldBounds bounds = ResolveFieldBounds(field, fields, object);
            double settled = value;
            if (bounds.hasMin)
            {
                settled = std::max(settled, bounds.minValue);
            }
            if (bounds.hasMax)
            {
                settled = std::min(settled, bounds.maxValue);
            }

            // The finite test is what stops a NaN from being written back over
            // and over: it compares unequal to itself, so "did it move" would be
            // true on every pass.
            if (settled != value && std::isfinite(settled) && WriteNumericField(field, object, settled))
            {
                movedThisPass = true;
            }
        }
        if (!movedThisPass)
        {
            break;
        }
        anyMoved = true;
    }
    return anyMoved;
}

} // namespace Assisi::Core::Reflect
