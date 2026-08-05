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
/// The blueprint editor is what this exists for. Editing a crate means looking at
/// it, and looking at it means a sun and something to stand on — neither of which
/// is part of the crate. Without a marker, the first save of `crate.abp` bakes a
/// directional light into it, and every instance of that crate placed in a level
/// then brings its own sun along.
///
/// **A tag rather than a flag on the light**, because the rule is about the entity
/// and not about any component it happens to carry: a helper mesh, a backdrop, a
/// measuring stick are all the same case, and none of them wants its own opt-out.
///
/// It lives in Runtime rather than in the editor library because
/// SceneSerializer::Save is the thing that has to honour it, and a rule enforced
/// somewhere the enforcer cannot see is a rule that gets forgotten. The cost is one
/// registered component id in a shipped game, which registers it and never adds it.
///
/// ACOMP(transient): id-only, so a Scene can store it. It reflects no fields —
/// there is nothing to say beyond presence — and being unserializable is not the
/// mechanism. Save skips the *entity*, which it does by asking for this tag
/// directly, exactly as it asks for ECS::BlueprintMember.
ACOMP(transient)
struct EditorOnly
{
};

} // namespace Assisi::Runtime
