/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file EditorFogVolumes.cpp
/// @brief Where a fog volume's shape is while it is being placed.
///
/// A fog volume's edges are soft by design, so the fog alone never says where
/// its box, sphere or outline ends. Each is drawn as its own fields: the box at
/// its size, the sphere at its radius, both turned and scaled with the entity,
/// and an outline as its floor, its top and an upright at each node.

#include <Assisi/Editor/EditorApp.hpp>

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
        AddBoxWireframe(*style.batch, Assisi::ECS::MatrixFrom(world, origin), style.color, 0.5f * box.size);
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
