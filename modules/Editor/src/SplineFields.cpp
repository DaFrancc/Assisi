/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Editor/SplineFields.hpp>

#include <cstddef>

#include <Assisi/Core/Reflect/ContainerOps.hpp>
#include <Assisi/Core/Reflect/StructMeta.hpp>
#include <Assisi/Editor/ListEdit.hpp>

ASSISI_REFLECTED_STRUCT(Assisi::Geometry::Spline);

namespace Assisi::Editor
{
namespace
{

using Core::Reflect::FieldMeta;
using Core::Reflect::FieldType;
using SplineVisitor = std::function<void (Geometry::Spline &)>;

const Core::Reflect::StructSpec *SplineSpec()
{
    return Core::Reflect::StructTraits<Geometry::Spline>::Spec();
}

void Walk(std::byte *value, const FieldMeta &field, const SplineVisitor &visit);

void WalkFields(std::byte *object, std::span<const FieldMeta> fields, const SplineVisitor &visit)
{
    for (const FieldMeta &field : fields)
    {
        Walk(object + field.offset, field, visit);
    }
}

/// The spline or splines at @p value, which @p field describes.
void Walk(std::byte *value, const FieldMeta &field, const SplineVisitor &visit)
{
    if (field.structSpec == nullptr)
    {
        return;
    }
    if (field.container != nullptr)
    {
        const Core::Reflect::ContainerOps &ops = *field.container->ops;
        if (ops.at == nullptr)
        {
            return;
        }
        const std::size_t count = ops.size(value);
        for (std::size_t row = 0; row < count; ++row)
        {
            Walk(ops.at(value, row), ElementFieldMeta(field, row), visit);
        }
        return;
    }
    if (field.type != FieldType::Struct)
    {
        return;
    }
    if (field.structSpec == SplineSpec())
    {
        visit(*static_cast<Geometry::Spline *>(static_cast<void *>(value)));
        return;
    }
    WalkFields(value, field.structSpec->fields, visit);
}

} // namespace

bool HoldsSpline(std::span<const FieldMeta> fields)
{
    for (const FieldMeta &field : fields)
    {
        // A container's structSpec describes the struct at its innermost
        // level, so one check covers a struct field and every list of one. A
        // struct cannot hold itself, so the descent ends.
        if (field.structSpec == nullptr || field.type == FieldType::Map)
        {
            continue;
        }
        if (field.structSpec == SplineSpec() || HoldsSpline(field.structSpec->fields))
        {
            return true;
        }
    }
    return false;
}

void ForEachSpline(const void *object, std::span<const FieldMeta> fields,
                   const std::function<void(const Geometry::Spline &)> &visit)
{
    // Walked through the writable path, which never writes: what it hands on is
    // const again before the caller sees it.
    std::byte *bytes = static_cast<std::byte *>(const_cast<void *>(object));
    WalkFields(bytes, fields, [&visit](Geometry::Spline &spline) { visit(spline); });
}

void SettleSplines(void *object, std::span<const FieldMeta> fields)
{
    WalkFields(static_cast<std::byte *>(object), fields, [](Geometry::Spline &spline) { Geometry::SettleHandles(spline); });
}

} // namespace Assisi::Editor
