/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file ColliderPose.cpp
/// @brief Implements ColliderBodyModel — see the header for what it answers.
///
/// A free function in its own unit rather than an EditorApp member: the pose is
/// the whole of the bug this fixes, and the Editor test binary compiles this file
/// directly (Runtime alone, no ImGui/GLFW/Jolt) to hold it to it.

#include <Assisi/Editor/ColliderPose.hpp>

#include <Assisi/ECS/TransformPose.hpp>
#include <Assisi/Runtime/Hierarchy.hpp>

namespace Assisi::Editor
{

namespace
{

/// The parent's propagated world matrix, or null for a root — the same question
/// App::ParentWorldResolver answers on Physics' behalf.
const glm::mat4 *ParentWorld(const ECS::Scene &scene, ECS::Entity entity)
{
    const Runtime::Parent *parent = scene.Get<Runtime::Parent>(entity);
    if (parent == nullptr || parent->parent == ECS::NullEntity)
        return nullptr;

    const ECS::Transform *parentTransform = scene.Get<ECS::Transform>(parent->parent);
    return parentTransform != nullptr ? &parentTransform->worldMatrix : nullptr;
}

} // namespace

glm::mat4 ColliderBodyModel(const ECS::Scene &scene, ECS::Entity entity, const ECS::Transform &local)
{
    const glm::mat4 *parent = ParentWorld(scene, entity);
    const ECS::Transform pose   = parent != nullptr ? ECS::PoseUnderParent(local, *parent) : local;

    glm::mat4 model = glm::mat4_cast(pose.rotation);
    model[3]        = glm::vec4(pose.position, 1.f);
    return model;
}

} // namespace Assisi::Editor
