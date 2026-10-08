/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Pose.hpp
/// @brief Turns a skeleton's pose — each joint's transform relative to its
///        parent — into the matrices skinning and joint queries read.
///
/// A pose is local so that sampling, blending and IK can work on one joint
/// without touching the rest; these functions are the one place it becomes
/// model space, once per frame.

#include <cstdint>
#include <span>
#include <string_view>

#include <Assisi/Geometry/MeshData.hpp>
#include <Assisi/Math/GLM.hpp>

namespace Assisi::Geometry
{

/// @brief What FindJoint returns for a name the skeleton does not have.
inline constexpr int32_t kNoJoint = -1;

/// @brief @p local as one matrix: scale, then rotation, then translation.
[[nodiscard]] glm::mat4 JointMatrix(const JointTransform &local);

/// @brief Each joint's transform in the mesh's model space.
///
/// A root joint is placed by the skeleton's RootTransform, every other joint by
/// its parent, in one forward pass. @p local and @p outModel hold one entry per
/// joint.
void JointModelTransforms(const Skeleton &skeleton, std::span<const JointTransform> local,
                          std::span<glm::mat4> outModel);

/// @brief The matrix each joint moves its vertices by: its model transform
///        times its inverse bind matrix. Identity for a joint at its bind pose.
///
/// @p model and @p outPalette hold one entry per joint.
void SkinningPalette(const Skeleton &skeleton, std::span<const glm::mat4> model, std::span<glm::mat4> outPalette);

/// @brief The index of the joint named @p name, or kNoJoint.
[[nodiscard]] int32_t FindJoint(const Skeleton &skeleton, std::string_view name);

} // namespace Assisi::Geometry
