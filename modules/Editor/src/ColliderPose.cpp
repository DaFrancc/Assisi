/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file ColliderPose.cpp
/// @brief Implements ColliderBodyModel — see the header for what it answers.
///
/// A free function in its own unit rather than an EditorApp member, so the Editor
/// test binary can compile it directly — Runtime alone, no ImGui/GLFW/Jolt.

#include <Assisi/Editor/ColliderPose.hpp>

#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/TransformPose.hpp>
#include <Assisi/ECS/WorldMatrix.hpp>
#include <Assisi/Math/Matrix.hpp>

namespace Assisi::Editor
{

glm::mat4 ColliderBodyModel(const ECS::Scene &scene, ECS::Entity entity, const ECS::Transform &local,
                            const glm::dvec3 &origin)
{
    // A parented pose comes out measured from where its parent's matrix is,
    // which is @p origin; a root's is in world space and is moved there. The
    // parent's own matrix, without the entity's ParentFrame: Physics places a
    // body from its parent's pose alone.
    const ECS::Parent *link = scene.Get<ECS::Parent>(entity);
    const ECS::WorldMatrix *parent =
        link != nullptr && link->parent != ECS::NullEntity ? scene.Get<ECS::WorldMatrix>(link->parent) : nullptr;
    const ECS::Transform pose =
        parent != nullptr ? ECS::PoseUnderParent(local, ECS::MatrixFrom(*parent, origin)) : local;

    glm::mat4 model = glm::mat4_cast(pose.rotation);
    Math::SetTranslation(model, glm::vec3(parent != nullptr ? pose.position : pose.position - origin));
    return model;
}

} // namespace Assisi::Editor
