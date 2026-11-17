/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file EditorFogVolumes.cpp
/// @brief Where a fog volume's shape is while it is being placed.
///
/// A fog volume's edges are soft by design, so the fog alone never says where
/// its box, sphere or outline ends. Each is drawn as its own fields: the box at
/// its size, the sphere at its radius, both turned and scaled with the entity,
/// and an outline as its floor, its top and an upright at each node. A piled box
/// or outline adds its dome's two arcs through the middle.

#include <Assisi/Editor/EditorApp.hpp>

#include <array>
#include <cstdint>
#include <vector>

#include <Assisi/ECS/WorldMatrix.hpp>
#include <Assisi/Editor/Overlay/OverlayRenderer.hpp>
#include <Assisi/Editor/WireShapes.hpp>
#include <Assisi/Geometry/Spline.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Runtime/FogVolume.hpp>

namespace Assisi::Editor
{
namespace
{
namespace Rt = Assisi::Runtime;

// The volume's own colour, faint enough that overlapping volumes stay legible.
constexpr float kUnselectedAlpha = 0.35f;
constexpr glm::vec4 kSelectedColor{kSelectionOutline, 1.0f};
constexpr glm::vec4 kActiveSelectedColor{kActiveSelectionOutline, 1.0f};

// How closely an outline's drawn floor and top follow its curve, in the
// entity's units.
constexpr float kOutlineDrawTolerance = 0.05f;

// Straight pieces in each arc a piled top is drawn with.
constexpr int32_t kDomeArcSegments = 24;

/// @brief A piled top, as drawn: over a footprint centred above @p floor, in
/// the entity's space.
struct DomeSketch
{
    /// The middle of the footprint, at the floor's height.
    glm::vec3 floor{0.0f};
    /// The footprint's half extents across x and z.
    glm::vec2 footprint{1.0f};
    /// How high the top stands over the middle.
    float height = 0.0f;
    float pile = 0.0f;
};

/// @brief A piled top's two arcs through its middle, along x and along z, from
/// the top over the middle down toward the floor at the footprint's edges.
void AddDomeArcs(std::vector<LineVertex> &out, const glm::mat4 &model, const glm::vec4 &color,
                 const DomeSketch &dome)
{
    if (dome.pile <= 0.0f)
    {
        return;
    }
    const std::array<glm::vec3, 2> axes{glm::vec3(dome.footprint.x, 0.0f, 0.0f),
                                        glm::vec3(0.0f, 0.0f, dome.footprint.y)};
    for (const glm::vec3 &axis : axes)
    {
        glm::vec3 previous(0.0f);
        for (int32_t i = 0; i <= kDomeArcSegments; ++i)
        {
            const float across = -1.0f + 2.0f * static_cast<float>(i) / static_cast<float>(kDomeArcSegments);
            const glm::vec3 local =
                dome.floor + axis * across + glm::vec3(0.0f, dome.height * (1.0f - dome.pile * across * across), 0.0f);
            const glm::vec3 point(model * glm::vec4(local, 1.0f));
            if (i > 0)
            {
                out.push_back(LineVertex{.position = previous, .color = color});
                out.push_back(LineVertex{.position = point, .color = color});
            }
            previous = point;
        }
    }
}

/// @brief Which batch a volume draws into, and in what colour.
struct FogVolumeStyle
{
    std::vector<LineVertex> *batch = nullptr;
    glm::vec4 color{1.0f};
};

/// @brief The two batches a volume may draw into: behind the scene's geometry,
/// or over it for the selection, which is often inside a hill or a room.
struct FogVolumeBatches
{
    std::vector<LineVertex> *depthTested = nullptr;
    std::vector<LineVertex> *onTop = nullptr;
};

FogVolumeStyle StyleFor(const FogVolumeBatches &batches, bool selected, bool active, const glm::vec3 &fogColor)
{
    if (!selected)
    {
        return FogVolumeStyle{.batch = batches.depthTested, .color = glm::vec4(fogColor, kUnselectedAlpha)};
    }
    return FogVolumeStyle{.batch = batches.onTop, .color = active ? kActiveSelectedColor : kSelectedColor};
}

/// @brief Adds the loop through @p points at @p height, in @p model's space.
void AddOutlineLoop(std::vector<LineVertex> &out, const glm::mat4 &model, const glm::vec4 &color,
                    const std::vector<glm::vec3> &points, float height)
{
    for (std::size_t i = 0; i < points.size(); ++i)
    {
        const glm::vec3 &from = points[i];
        const glm::vec3 &to = points[(i + 1) % points.size()];
        out.push_back(LineVertex{.position = glm::vec3(model * glm::vec4(from.x, height, from.z, 1.0f)), .color = color});
        out.push_back(LineVertex{.position = glm::vec3(model * glm::vec4(to.x, height, to.z, 1.0f)), .color = color});
    }
}

/// @brief An outline volume: its floor at the entity's origin, its top at its
/// height, and an upright joining them at each node.
void AddFogOutlineWireframe(std::vector<LineVertex> &out, const glm::mat4 &model, const glm::vec4 &color,
                            const Rt::FogOutline &outline)
{
    const Geometry::SplinePolyline chain = Geometry::FlattenSpline(outline.outline, kOutlineDrawTolerance);
    if (chain.points.size() < 2)
    {
        return;
    }
    AddOutlineLoop(out, model, color, chain.points, 0.0f);
    AddOutlineLoop(out, model, color, chain.points, outline.height);
    for (const Geometry::SplineNode &node : outline.outline.nodes)
    {
        const glm::vec3 &p = node.position;
        out.push_back(LineVertex{.position = glm::vec3(model * glm::vec4(p.x, 0.0f, p.z, 1.0f)), .color = color});
        out.push_back(LineVertex{.position = glm::vec3(model * glm::vec4(p.x, outline.height, p.z, 1.0f)), .color = color});
    }

    // The pile peaks over the middle of the outline's bounds.
    glm::vec2 low(chain.points.front().x, chain.points.front().z);
    glm::vec2 high = low;
    for (const glm::vec3 &point : chain.points)
    {
        low = glm::min(low, glm::vec2(point.x, point.z));
        high = glm::max(high, glm::vec2(point.x, point.z));
    }
    const glm::vec2 middle = 0.5f * (low + high);
    AddDomeArcs(out, model, color,
                DomeSketch{.floor = glm::vec3(middle.x, 0.0f, middle.y),
                           .footprint = 0.5f * (high - low),
                           .height = outline.height,
                           .pile = outline.pile});
}

} // namespace

void EditorApp::SubmitFogVolumeGizmos()
{
    if (_scene == nullptr)
    {
        return;
    }
    _fogVolumeLinesDepthTested.clear();
    _fogVolumeLinesOnTop.clear();
    const glm::dvec3 origin = ViewOrigin();
    const FogVolumeBatches batches{.depthTested = &_fogVolumeLinesDepthTested, .onTop = &_fogVolumeLinesOnTop};

    for (auto [entity, world, box] : _scene->Query<Assisi::ECS::WorldMatrix, Rt::FogBox>())
    {
        const FogVolumeStyle style =
            StyleFor(batches, IsSelected(entity), entity == _selectedEntity, glm::vec3(box.color));
        const glm::mat4 model = Assisi::ECS::MatrixFrom(world, origin);
        const glm::vec3 half = 0.5f * box.size;
        AddBoxWireframe(*style.batch, model, style.color, half);
        AddDomeArcs(*style.batch, model, style.color,
                    DomeSketch{.floor = glm::vec3(0.0f, -half.y, 0.0f),
                               .footprint = glm::vec2(half.x, half.z),
                               .height = box.size.y,
                               .pile = box.pile});
    }
    for (auto [entity, world, sphere] : _scene->Query<Assisi::ECS::WorldMatrix, Rt::FogSphere>())
    {
        const FogVolumeStyle style =
            StyleFor(batches, IsSelected(entity), entity == _selectedEntity, glm::vec3(sphere.color));
        AddSphereWireframe(*style.batch, Assisi::ECS::MatrixFrom(world, origin), style.color, sphere.radius);
    }
    for (auto [entity, world, outline] : _scene->Query<Assisi::ECS::WorldMatrix, Rt::FogOutline>())
    {
        const FogVolumeStyle style =
            StyleFor(batches, IsSelected(entity), entity == _selectedEntity, glm::vec3(outline.color));
        AddFogOutlineWireframe(*style.batch, Assisi::ECS::MatrixFrom(world, origin), style.color, outline);
    }

    _overlays.SubmitOverlayLines(_fogVolumeLinesDepthTested, /*onTop=*/ false);
    _overlays.SubmitOverlayLines(_fogVolumeLinesOnTop, /*onTop=*/ true);
}

} // namespace Assisi::Editor
