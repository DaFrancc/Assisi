/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ListEdit.hpp
/// @brief What the Inspector's list rows ask for, kept apart from drawing them.
///
/// A row's buttons only record what was clicked. The change is applied once,
/// after every row is drawn, because adding or removing a row moves the list's
/// storage and any row address still in hand would point at freed memory.

#include <Assisi/Core/Reflect/ContainerOps.hpp>
#include <Assisi/Core/Reflect/FieldMeta.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace Assisi::Editor
{

enum class ListEditKind : uint8_t
{
    None,
    Add,      ///< A default row at the end.
    Remove,   ///< The row at `row`.
    MoveUp,   ///< The row at `row` swaps with the one before it.
    MoveDown, ///< The row at `row` swaps with the one after it.
    Count_,
};

struct ListEdit
{
    std::size_t row = 0;
    ListEditKind kind = ListEditKind::None;
};

/// @brief Applies @p edit to the container at @p container through @p ops.
///        Whether it changed anything: a row past the end, a move off either
///        end, or an edit the container can't take (an array has a fixed
///        length) leaves it as it was and reports so.
bool ApplyListEdit(const Core::Reflect::ContainerOps &ops, std::byte *container, ListEdit edit);

/// @brief A field describing one row of the list @p list describes, for the
///        widgets that edit a field: named for row @p row, of the element's
///        type, with the element's own container shape when it is a list too.
///        The struct and enum descriptions carry over, since they describe the
///        innermost element whatever the depth.
[[nodiscard]] Core::Reflect::FieldMeta ElementFieldMeta(const Core::Reflect::FieldMeta &list, std::size_t row);

/// @brief One step from an object to a field inside it: the bytes from the
///        object to the field, then, when the field is a list, the row.
struct FieldStep
{
    std::size_t offset = 0;
    const Core::Reflect::ContainerOps *list = nullptr; ///< The field's list, or null when it is no list.
    std::size_t row = 0;
};

/// @brief The address @p path leads to from @p object, or null when a row it
///        names is past its list's end.
///
/// How something that keeps a field in view across frames, such as the asset
/// browser, finds it again: a list's rows move when it grows, so an address
/// kept from when the path was taken may no longer be the field's.
[[nodiscard]] std::byte *ResolveFieldPath(std::byte *object, std::span<const FieldStep> path);

} // namespace Assisi::Editor
