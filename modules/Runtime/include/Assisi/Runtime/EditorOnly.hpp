/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file EditorOnly.hpp
/// @brief The one-word marker that says "this entity is scaffolding, not content".

#include <Assisi/Prelude.hpp>

namespace Assisi::Runtime
{

/// @brief Scaffolding the editor put in the scene to work with, which a save must
/// never write.
///
/// The blueprint editor is what this exists for: editing a crate means a sun and
/// something to stand on, neither of which is part of the crate. Without a marker,
/// the first save of `crate.abp` bakes a directional light into it and every
/// instance of that crate brings its own sun along.
///
/// **A tag rather than a flag on the light**: the rule is about the entity, not
/// about any component it carries — a helper mesh, a backdrop and a measuring stick
/// are the same case, and none wants its own opt-out.
///
/// It lives in Runtime rather than the editor library because
/// SceneSerializer::Save is what has to honour it. The cost to a shipped game is
/// one registered component id it never adds.
///
/// ACOMP(transient): id-only, so a Scene can store it. Being unserializable is not
/// the mechanism — Save skips the *entity*, asking for this tag directly, exactly
/// as it asks for ECS::BlueprintMember.
ACOMP(transient)
struct EditorOnly
{
};

} // namespace Assisi::Runtime
