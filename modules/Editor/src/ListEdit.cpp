/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Editor/ListEdit.hpp>

#include <format>

namespace Assisi::Editor
{

namespace
{

using Core::Reflect::ContainerOps;

bool Move(const ContainerOps &ops, std::byte *container, std::size_t from, std::size_t to)
{
    if (ops.move == nullptr)
    {
        return false;
    }
    ops.move(container, from, to);
    return true;
}

} // namespace

bool ApplyListEdit(const ContainerOps &ops, std::byte *container, ListEdit edit)
{
    const std::size_t size = ops.size(container);
    switch (edit.kind)
    {
    case ListEditKind::Add:
        if (ops.pushDefault == nullptr)
        {
            return false;
        }
        ops.pushDefault(container);
        return true;
    case ListEditKind::Remove:
        if (ops.erase == nullptr || edit.row >= size)
        {
            return false;
        }
        ops.erase(container, edit.row);
        return true;
    case ListEditKind::MoveUp:
        if (edit.row == 0 || edit.row >= size)
        {
            return false;
        }
        return Move(ops, container, edit.row, edit.row - 1);
    case ListEditKind::MoveDown:
        if (edit.row + 1 >= size)
        {
            return false;
        }
        return Move(ops, container, edit.row, edit.row + 1);
    default:
        return false;
    }
}

Core::Reflect::FieldMeta ElementFieldMeta(const Core::Reflect::FieldMeta &list, std::size_t row)
{
    Core::Reflect::FieldMeta element;
    element.name = std::format("[{}]", row);
    element.type = list.container->elementType;
    element.container = list.container->element;
    element.structSpec = list.structSpec;
    element.enumConstants = list.enumConstants;
    element.enumType = list.enumType;
    element.enumSize = list.enumSize;
    element.enumSigned = list.enumSigned;
    return element;
}

std::byte *ResolveFieldPath(std::byte *object, std::span<const FieldStep> path)
{
    std::byte *at = object;
    for (const FieldStep &step : path)
    {
        at += step.offset;
        if (step.list == nullptr)
        {
            continue;
        }
        if (step.row >= step.list->size(at))
        {
            return nullptr;
        }
        at = step.list->at(at, step.row);
    }
    return at;
}

} // namespace Assisi::Editor
