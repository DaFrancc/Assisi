/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SkeletonOverlay.hpp
/// @brief The lines a skinned mesh's skeleton is drawn with, and which joint
///        the cursor is over.
///
/// Kept out of EditorApp so both can be tested without a window or a device.

#include <cstdint>
#include <span>
#include <vector>

#include <Assisi/Editor/Overlay/LinePass.hpp>
#include <Assisi/Editor/ScenePick.hpp>
#include <Assisi/Geometry/MeshData.hpp>
#include <Assisi/Math/GLM.hpp>

namespace Assisi::Editor
{

/// @brief Half the length of each arm of the cross drawn at a joint, in the
///        mesh's units: big enough to see a fingertip, small enough not to hide
///        the bones beside it.
inline constexpr float kJointMarkerHalfSize = 0.015f;

/// @brief How near the cursor must be to a joint to name it, in pixels.
inline constexpr float kJointHoverPixels = 8.f;

/// @brief Appends a line from each joint to its parent, and a small cross at
///        every joint, placed by @p world.
///
/// @p jointModel holds each joint's transform in the mesh's model space, one per
/// joint of @p skeleton.
void AppendSkeletonLines(std::vector<LineVertex> &out, const Geometry::Skeleton &skeleton,
                         std::span<const glm::mat4> jointModel, const glm::mat4 &world, const glm::vec4 &color);

/// @brief The joint drawn nearest @p cursor, if one is within
///        kJointHoverPixels; otherwise Geometry::kNoJoint.
///
/// @p pixelsOut is that joint's screen distance from the cursor, so the nearest
/// across several skeletons can win. A joint behind the eye is never picked.
[[nodiscard]] int32_t JointUnderCursor(const PickRay &ray, glm::vec2 cursor, std::span<const glm::mat4> jointModel,
                                       const glm::mat4 &world, float &pixelsOut);

} // namespace Assisi::Editor
