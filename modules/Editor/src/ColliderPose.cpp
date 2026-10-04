/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file ColliderPose.cpp
/// @brief Implements ColliderBodyModel — see the header for what it answers.
///
/// A free function in its own unit rather than an EditorApp member, so the Editor
/// test binary can compile it directly — Runtime alone, no ImGui/GLFW/Jolt.

#include <Assisi/Editor/ColliderPose.hpp>

#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/ECS/TransformPose.hpp>
#include <Assisi/Math/Matrix.hpp>

namespace Assisi::Editor
{

glm::mat4 ColliderBodyModel(const ECS::Scene &scene, ECS::Entity entity, const ECS::Transform &local)
{
    const glm::mat4 *parent = ECS::ParentWorldMatrix(scene, entity);
    const ECS::Transform pose = parent != nullptr ? ECS::PoseUnderParent(local, *parent) : local;

    glm::mat4 model = glm::mat4_cast(pose.rotation);
    Math::SetTranslation(model, pose.position);
    return model;
}

} // namespace Assisi::Editor
