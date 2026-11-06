/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Editor/SplineFields.hpp>

#include <cstddef>
#include <vector>

#include <Assisi/Core/Reflect/ContainerOps.hpp>
#include <Assisi/Core/Reflect/StructMeta.hpp>

ASSISI_REFLECTED_STRUCT(Assisi::Geometry::Spline);

namespace Assisi::Editor
{
namespace
{

using Core::Reflect::FieldMeta;
using Core::Reflect::FieldType;
using SplineVisitor = std::function<void (Geometry::Spline &, std::span<const FieldStep>)>;

const Core::Reflect::StructSpec *SplineSpec()
{
    return Core::Reflect::StructTraits<Geometry::Spline>::Spec();
}

/// Walks one level down, with @p path leading to it: a step for each field the
/// way ResolveFieldPath reads one, and a step for each list row below a field.
class SplineWalk
{
public:
    explicit SplineWalk(const SplineVisitor &visit) : _visit(visit) {}

    void Fields(std::byte *object, std::span<const FieldMeta> fields)
    {
        for (const FieldMeta &field : fields)
        {
            _path.push_back(FieldStep{.offset = field.offset});
            Value(object + field.offset, field);
            _path.pop_back();
        }
    }

private:
    /// The spline or splines at @p value, which @p field describes.
    void Value(std::byte *value, const FieldMeta &field)
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
                _path.push_back(FieldStep{.offset = 0, .list = &ops, .row = row});
                Value(ops.at(value, row), ElementFieldMeta(field, row));
                _path.pop_back();
            }
            return;
        }
        if (field.type != FieldType::Struct)
        {
            return;
        }
        if (field.structSpec == SplineSpec())
        {
            _visit(*static_cast<Geometry::Spline *>(static_cast<void *>(value)), _path);
            return;
        }
        Fields(value, field.structSpec->fields);
    }

    const SplineVisitor &_visit;
    std::vector<FieldStep> _path;
};

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

void ForEachSplinePath(void *object, std::span<const FieldMeta> fields, const SplineVisitor &visit)
{
    SplineWalk{visit}.Fields(static_cast<std::byte *>(object), fields);
}

void ForEachSpline(const void *object, std::span<const FieldMeta> fields,
                   const std::function<void(const Geometry::Spline &)> &visit)
{
    // Walked through the writable path, which never writes: what it hands on is
    // const again before the caller sees it.
    ForEachSplinePath(const_cast<void *>(object), fields,
                      [&visit](Geometry::Spline &spline, std::span<const FieldStep>) { visit(spline); });
}

void SettleSplines(void *object, std::span<const FieldMeta> fields)
{
    ForEachSplinePath(object, fields,
                      [](Geometry::Spline &spline, std::span<const FieldStep>) { Geometry::SettleHandles(spline); });
}

} // namespace Assisi::Editor
