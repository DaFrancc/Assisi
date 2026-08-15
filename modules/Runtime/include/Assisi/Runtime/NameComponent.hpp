/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file NameComponent.hpp
/// @brief A human-readable name for an entity, shown in the editor.

#include <Assisi/Prelude.hpp>
#include <Assisi/Core/TrivialString.hpp>

namespace Assisi::Runtime
{

/// @brief An optional, editor-visible label for an entity.
///
/// Entities without a Name fall back to their `[index:generation]` id in the
/// entity list; adding a Name (via the inspector's rename field) makes the scene
/// readable. The label is a fixed-capacity EntityName, so the component stays
/// trivially copyable and heap-free. Serialized with the level like any other
/// reflected component, so names persist across saves.
///
/// Never written directly: every path that names an entity goes through a door
/// in Runtime/Naming.hpp, which is what keeps two entities from answering to one
/// name. See that file for which door and why there are two.
///
/// Replicable so a joining client's entity list reads the same as the host's —
/// debugging a session against `[14:1]` on one screen and `Crate` on the other
/// is needless friction. Cheap: a name changes about never, so after the spawn
/// it costs nothing until it does.
ACOMP(replicable)
struct Name
{
    AFIELD() Assisi::Core::EntityName value;
};

} // namespace Assisi::Runtime
