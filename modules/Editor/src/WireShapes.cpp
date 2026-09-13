/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Editor/WireShapes.hpp>

#include <Assisi/Math/Angles.hpp>

#include <algorithm>

namespace Assisi::Editor
{
namespace
{
using Render::LineVertex;
} // namespace

void AddSegment(std::vector<LineVertex> &out, const glm::mat4 &model, const glm::vec4 &color, const glm::vec3 &a,
                const glm::vec3 &b)
{
    out.push_back({glm::vec3(model * glm::vec4(a, 1.f)), color});
    out.push_back({glm::vec3(model * glm::vec4(b, 1.f)), color});
}

void AddArc(std::vector<LineVertex> &out, const glm::mat4 &model, const glm::vec4 &color, const glm::vec3 &center,
            const glm::vec3 &u, const glm::vec3 &v, float radius, float a0, float a1, std::int32_t segments)
{
    glm::vec3 prev = center + radius * (std::cos(a0) * u + std::sin(a0) * v);
    for (std::int32_t i = 1; i <= segments; ++i)
    {
        const float t = a0 + (a1 - a0) * (static_cast<float>(i) / static_cast<float>(segments));
        const glm::vec3 cur = center + radius * (std::cos(t) * u + std::sin(t) * v);
        AddSegment(out, model, color, prev, cur);
        prev = cur;
    }
}

void AddBoxWireframe(std::vector<LineVertex> &out, const glm::mat4 &model, const glm::vec4 &color,
                     const glm::vec3 &halfExtents)
{
    const glm::vec3 &h = halfExtents;
    // Eight corners, indexed by sign bits (x = bit0, y = bit1, z = bit2).
    glm::vec3 c[8];
    for (std::int32_t i = 0; i < 8; ++i)
    {
        c[i] = {(i & 1) ? h.x : -h.x, (i & 2) ? h.y : -h.y, (i & 4) ? h.z : -h.z};
    }
    // 12 edges: pairs of corners differing in exactly one axis bit.
    constexpr std::int32_t edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7},  // along X
        {0, 2}, {1, 3}, {4, 6}, {5, 7},                                     // along Y
        {0, 4}, {1, 5}, {2, 6}, {3, 7}};                                    // along Z
    for (const auto &e : edges)
    {
        AddSegment(out, model, color, c[e[0]], c[e[1]]);
    }
}

void AddSphereWireframe(std::vector<LineVertex> &out, const glm::mat4 &model, const glm::vec4 &color, float radius)
{
    const float full = glm::two_pi<float>();
    AddArc(out, model, color, glm::vec3(0.f), kAxisX, kAxisY, radius, 0.f, full, kCircleSegments);
    AddArc(out, model, color, glm::vec3(0.f), kAxisX, kAxisZ, radius, 0.f, full, kCircleSegments);
    AddArc(out, model, color, glm::vec3(0.f), kAxisY, kAxisZ, radius, 0.f, full, kCircleSegments);
}

void AddCylinderBody(std::vector<LineVertex> &out, const glm::mat4 &model, const glm::vec4 &color, float radius,
                     float halfHeight)
{
    const glm::vec3 top(0.f, halfHeight, 0.f);
    const glm::vec3 bot(0.f, -halfHeight, 0.f);
    const float full = glm::two_pi<float>();
    AddArc(out, model, color, top, kAxisX, kAxisZ, radius, 0.f, full, kCircleSegments);
    AddArc(out, model, color, bot, kAxisX, kAxisZ, radius, 0.f, full, kCircleSegments);
    for (std::int32_t k = 0; k < 4; ++k)
    {
        const float angle = static_cast<float>(k) * glm::half_pi<float>();
        const glm::vec3 offset = radius * (std::cos(angle) * kAxisX + std::sin(angle) * kAxisZ);
        AddSegment(out, model, color, top + offset, bot + offset);
    }
}

void AddCapsuleWireframe(std::vector<LineVertex> &out, const glm::mat4 &model, const glm::vec4 &color, float radius,
                         float halfHeight)
{
    AddCylinderBody(out, model, color, radius, halfHeight);

    const glm::vec3 top(0.f, halfHeight, 0.f);
    const glm::vec3 bot(0.f, -halfHeight, 0.f);
    const float pi = glm::pi<float>();
    const std::int32_t capSegs = kCircleSegments / 2;
    // Two orthogonal profile half-arcs per hemisphere: rim point, over the pole, to
    // the opposite rim point. Top domes up (+Y), bottom domes down (-Y).
    AddArc(out, model, color, top, kAxisX, kAxisY, radius, 0.f, pi, capSegs);
    AddArc(out, model, color, top, kAxisZ, kAxisY, radius, 0.f, pi, capSegs);
    AddArc(out, model, color, bot, kAxisX, kAxisY, radius, 0.f, -pi, capSegs);
    AddArc(out, model, color, bot, kAxisZ, kAxisY, radius, 0.f, -pi, capSegs);
}

void AddConeWireframe(std::vector<LineVertex> &out, const glm::mat4 &model, const glm::vec4 &color,
                      float halfAngleDegrees, float height, bool drawAxis)
{
    if (!(height > 0.f))
    {
        return;
    }
    // The same ceiling a spot light's cone is clamped to where its shadow map is
    // built, and it has to be the same one: this wireframe is what an author
    // aims the cone by, so a gizmo drawn wider than the map can be built for is
    // a cone whose shadow stops at a different angle than the shape promised.
    const float halfAngle = glm::radians(std::clamp(halfAngleDegrees, 0.f, Math::kMaxConeHalfAngleDegrees));
    const float radius = std::tan(halfAngle) * height;
    const glm::vec3 rim(0.f, -height, 0.f);

    AddArc(out, model, color, rim, kAxisX, kAxisZ, radius, 0.f, glm::two_pi<float>(), kCircleSegments);
    // Four ribs rather than one per rim segment: enough to read as a cone from
    // any angle, few enough that two nested cones do not become a solid.
    for (std::int32_t k = 0; k < 4; ++k)
    {
        const float angle = static_cast<float>(k) * glm::half_pi<float>();
        AddSegment(out, model, color, glm::vec3(0.f),
                   rim + radius * (std::cos(angle) * kAxisX + std::sin(angle) * kAxisZ));
    }
    if (drawAxis)
    {
        AddSegment(out, model, color, glm::vec3(0.f), rim);
    }
}

void AddArrowWireframe(std::vector<LineVertex> &out, const glm::mat4 &model, const glm::vec4 &color, float length)
{
    if (!(length > 0.f))
    {
        return;
    }
    const glm::vec3 tip(0.f, -length, 0.f);
    AddSegment(out, model, color, glm::vec3(0.f), tip);

    // A head proportional to the shaft, so the arrow reads the same however it
    // is scaled, and barbs swept back rather than a flat cap — a cap seen
    // end-on is a dot, and end-on is exactly how a light pointing at the camera
    // is seen.
    const float headLength = length * 0.25f;
    const float headRadius = headLength * 0.4f;
    const glm::vec3 barbRing(0.f, -length + headLength, 0.f);
    for (std::int32_t k = 0; k < 4; ++k)
    {
        const float angle = static_cast<float>(k) * glm::half_pi<float>();
        AddSegment(out, model, color, tip,
                   barbRing + headRadius * (std::cos(angle) * kAxisX + std::sin(angle) * kAxisZ));
    }
    AddArc(out, model, color, barbRing, kAxisX, kAxisZ, headRadius, 0.f, glm::two_pi<float>(), kCircleSegments / 2);
}

glm::mat4 AimAlong(const glm::vec3 &direction)
{
    const float lengthSq = glm::dot(direction, direction);
    if (!(lengthSq > 0.f))
    {
        return glm::mat4(1.f);
    }
    const glm::vec3 forward = direction / std::sqrt(lengthSq);
    // -Y is the shapes' own forward, so this is the rotation taking that onto the
    // aim. Straight up is the degenerate case for the usual cross product, and it
    // is the ordinary case for a light — a sun points down.
    const glm::vec3 from(0.f, -1.f, 0.f);
    const float alignment = glm::dot(from, forward);
    if (alignment > 0.9999f)
    {
        return glm::mat4(1.f);
    }
    if (alignment < -0.9999f)
    {
        // Antiparallel: any axis perpendicular to Y turns it around, and there is
        // no shortest arc to prefer between them.
        return glm::rotate(glm::mat4(1.f), glm::pi<float>(), kAxisX);
    }
    const glm::vec3 axis = glm::normalize(glm::cross(from, forward));
    return glm::rotate(glm::mat4(1.f), std::acos(std::clamp(alignment, -1.f, 1.f)), axis);
}

} // namespace Assisi::Editor
