/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file WireShapes.hpp
/// @brief Line-list primitives the editor draws its overlays out of.
///
/// Every shape appends pairs of vertices to a caller-owned batch, in the local
/// space of a model matrix it transforms them by. That is the whole interface:
/// the batch goes to Render::LinePass through SceneRenderer::SubmitOverlayLines,
/// and nothing here knows what the shape means.
///
/// Shared rather than per-caller because a collider's sphere and a point light's
/// reach are the same drawing, and two of them would drift in tessellation and in
/// how they read on screen. What differs between overlays is the colour and what
/// the radius stands for, which is the caller's business.

#include <cmath>
#include <cstdint>
#include <vector>

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/LinePass.hpp>

namespace Assisi::Editor
{

/// @brief Tessellation of curved shapes, in segments per full circle. Smooth
/// enough for an overlay without flooding the line batch.
inline constexpr std::int32_t kCircleSegments = 24;

inline constexpr glm::vec3 kAxisX{1.f, 0.f, 0.f};
inline constexpr glm::vec3 kAxisY{0.f, 1.f, 0.f};
inline constexpr glm::vec3 kAxisZ{0.f, 0.f, 1.f};

/// @brief Append one segment, transforming both local endpoints by @p model.
void AddSegment(std::vector<Render::LineVertex> &out, const glm::mat4 &model, const glm::vec4 &color,
                const glm::vec3 &a, const glm::vec3 &b);

/// @brief Append a poly-line arc of @p segments in the plane spanned by unit axes
/// @p u and @p v, centred at @p center with radius @p radius, sweeping
/// [@p a0, @p a1]. A full circle is a0 = 0, a1 = 2*pi.
void AddArc(std::vector<Render::LineVertex> &out, const glm::mat4 &model, const glm::vec4 &color,
            const glm::vec3 &center, const glm::vec3 &u, const glm::vec3 &v, float radius, float a0, float a1,
            std::int32_t segments);

/// @brief A box's twelve edges, centred on the origin.
void AddBoxWireframe(std::vector<Render::LineVertex> &out, const glm::mat4 &model, const glm::vec4 &color,
                     const glm::vec3 &halfExtents);

/// @brief Three orthogonal great circles — a sphere, as far as an overlay goes.
void AddSphereWireframe(std::vector<Render::LineVertex> &out, const glm::mat4 &model, const glm::vec4 &color,
                        float radius);

/// @brief Two rings at +/- @p halfHeight along Y plus four vertical connectors:
/// the body a cylinder and a capsule share.
void AddCylinderBody(std::vector<Render::LineVertex> &out, const glm::mat4 &model, const glm::vec4 &color,
                     float radius, float halfHeight);

/// @brief A cylinder body capped by two hemispherical profiles.
void AddCapsuleWireframe(std::vector<Render::LineVertex> &out, const glm::mat4 &model, const glm::vec4 &color,
                         float radius, float halfHeight);

/// @brief A cone from the origin opening along -Y: the rim circle at @p height,
/// four ribs down to the apex, and the axis itself.
///
/// Along -Y because that is a spot light's own convention for "forward", so a
/// caller aiming one has no basis to build and no sign to get wrong — the model
/// matrix carries the aim.
void AddConeWireframe(std::vector<Render::LineVertex> &out, const glm::mat4 &model, const glm::vec4 &color,
                      float halfAngleDegrees, float height, bool drawAxis);

/// @brief An arrow from the origin along -Y of @p length, with a head of four
/// barbs.
///
/// The same -Y convention as the cone, and for the same reason: a light's aim is
/// a direction, and the matrix that orients the shape is the one place that
/// direction has to be turned into a basis.
void AddArrowWireframe(std::vector<Render::LineVertex> &out, const glm::mat4 &model, const glm::vec4 &color,
                       float length);

/// @brief A rotation carrying -Y onto @p direction.
///
/// The basis a light's gizmo is drawn in. @p direction need not be unit length;
/// a degenerate one falls back to the identity rather than producing NaN, which
/// would take every vertex of the overlay with it.
[[nodiscard]] glm::mat4 AimAlong(const glm::vec3 &direction);

} // namespace Assisi::Editor
