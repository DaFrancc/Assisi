/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file EditorEvents.hpp
/// @brief The events the editor pushes for itself, and declaring which queue
/// they belong in.

#include <Assisi/ECS/Entity.hpp>

namespace Assisi::Editor
{

/// @brief Emitted when the user clicks to select (or deselect) an entity.
/// NullEntity means the click landed on empty space. Universal, as
/// DeclareEditorEvents records: the selection is the editor's, not any one
/// world's.
struct EntitySelectionChangedEvent
{
    Assisi::ECS::Entity entity;
    /// Ctrl or Shift was held: fold the entity into the selection instead of
    /// replacing it. Both modifiers do the same thing in the viewport — a range
    /// needs an order to walk through, and the 3D view has none.
    bool additive = false;
};

/// @brief Records which of the editor's events belong to the whole program,
/// as AEVENT(universal) does for an event in a reflected header. The editor's
/// headers are not reflected: every generated registration is linked into every
/// executable, the shipped game included, which must not hold editor code.
/// EditorApp calls it as it is made, before anything is pushed.
void DeclareEditorEvents();

} // namespace Assisi::Editor
