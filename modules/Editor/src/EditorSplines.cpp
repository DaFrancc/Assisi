/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file EditorSplines.cpp
/// @brief Every spline in the level, drawn where it lies.
///
/// Found by reflection (SplineFields.hpp), so a component that gains a Spline
/// field is drawn with no code here. Each curve is drawn as the chain
/// Geometry::FlattenSpline gives, from the same math anything that follows the
/// curve uses, so the drawing cannot disagree with it.

#include <Assisi/Editor/EditorApp.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/WorldMatrix.hpp>
#include <Assisi/Editor/Overlay/LinePass.hpp>
#include <Assisi/Editor/Overlay/OverlayRenderer.hpp>
#include <Assisi/Editor/SplineFields.hpp>
#include <Assisi/Editor/WireShapes.hpp>
#include <Assisi/Geometry/Spline.hpp>
#include <Assisi/Math/GLM.hpp>

namespace Assisi::Editor
{
namespace
{

/// Magenta, which an outdoor level's greens and browns rarely are, and apart
/// from the cyan of a camera route and the orange of a selection.
constexpr glm::vec4 kSplineColor{1.0f, 0.35f, 0.85f, 0.9f};
constexpr glm::vec4 kSelectedColor{kSelectionOutline, 1.0f};
constexpr glm::vec4 kActiveSelectedColor{kActiveSelectionOutline, 1.0f};

/// How far the drawn chain may stray from the curve, in the holder's units. A
/// centimetre is finer than a line on screen from anywhere a spline is shaped.
constexpr float kDrawTolerance = 0.01f;

/// Half the side of the box on each node of a selected spline.
constexpr float kNodeHalfExtent = 0.12f;

/// The first node's box is this much larger, so which end a route starts from
/// shows.
constexpr float kFirstNodeScale = 2.f;

/// Half the side of the box at the tip of each handle of a Bezier node.
constexpr float kHandleHalfExtent = 0.06f;

/// Which batch an entity's splines go in, in what colour, and whether their
/// nodes show.
struct SplineStyle
{
    std::vector<LineVertex> *batch = nullptr;
    glm::vec4 color{0.f};
    bool detailed = false;
};

/// Where @p local, in the holder's space, lands in the overlay's.
glm::vec3 Placed(const glm::mat4 &model, const glm::vec3 &local)
{
    return glm::vec3(model * glm::vec4(local, 1.f));
}

/// A box on each node, and the handles of each Bezier node. Placed through
/// @p model but not scaled by it, so a node reads the same size on any entity.
void AddNodes(const SplineStyle &style, const glm::mat4 &model, const Geometry::Spline &spline)
{
    const glm::mat4 identity(1.f);
    for (std::size_t i = 0; i < spline.nodes.size(); ++i)
    {
        const Geometry::SplineNode &node = spline.nodes[i];
        const glm::vec3 at = Placed(model, node.position);
        const float half = i == 0 ? kNodeHalfExtent * kFirstNodeScale : kNodeHalfExtent;
        AddBoxWireframe(*style.batch, glm::translate(identity, at), style.color, glm::vec3(half));
        if (node.mode != Geometry::SplineNodeMode::Bezier)
        {
            continue;
        }
        for (const glm::vec3 &handle : {node.handleIn, node.handleOut})
        {
            const glm::vec3 tip = Placed(model, node.position + handle);
            AddSegment(*style.batch, identity, style.color, at, tip);
            AddBoxWireframe(*style.batch, glm::translate(identity, tip), style.color, glm::vec3(kHandleHalfExtent));
        }
    }
}

/// One spline's curve, and its nodes when the style shows them.
void AddSpline(const SplineStyle &style, const glm::mat4 &model, const Geometry::Spline &spline)
{
    const Geometry::SplinePolyline chain = Geometry::FlattenSpline(spline, kDrawTolerance);
    for (std::size_t i = 1; i < chain.points.size(); ++i)
    {
        AddSegment(*style.batch, model, style.color, chain.points[i - 1], chain.points[i]);
    }
    if (style.detailed)
    {
        AddNodes(style, model, spline);
    }
}

/// Where @p entity's splines are placed from: its world matrix measured from
/// @p origin, or the world itself for an entity with no Transform.
glm::mat4 ModelFor(ECS::Scene &scene, ECS::Entity entity, const glm::dvec3 &origin)
{
    const ECS::WorldMatrix *world = scene.Get<ECS::WorldMatrix>(entity);
    return world != nullptr ? ECS::MatrixFrom(*world, origin) : glm::translate(glm::mat4(1.f), glm::vec3(-origin));
}

} // namespace

void EditorApp::SubmitSplineGizmos()
{
    if (_scene == nullptr)
    {
        return;
    }

    _splineLinesDepthTested.clear();
    _splineLinesOnTop.clear();

    // The selection on top, so a spline being shaped is never lost behind the
    // ground it lies on.
    const SplineStyle behind{.batch = &_splineLinesDepthTested, .color = kSplineColor, .detailed = false};
    const SplineStyle selected{.batch = &_splineLinesOnTop, .color = kSelectedColor, .detailed = true};
    const SplineStyle active{.batch = &_splineLinesOnTop, .color = kActiveSelectedColor, .detailed = true};

    // Measured from the camera, as the overlay draws them.
    const glm::dvec3 origin = ViewOrigin();
    for (const Core::Reflect::ComponentMeta &meta : Core::Reflect::ComponentRegistry::Instance().All())
    {
        if (!meta.iterateEntities || !HoldsSpline(meta.fields))
        {
            continue;
        }
        meta.iterateEntities(_scene,
                             [&](std::uint32_t index, std::uint32_t generation, const void *component)
            {
                const ECS::Entity entity{.index = index, .generation = generation};
                const SplineStyle &style =
                    entity == _selectedEntity ? active : (IsSelected(entity) ? selected : behind);
                const glm::mat4 model = ModelFor(*_scene, entity, origin);
                ForEachSpline(component, meta.fields,
                              [&](const Geometry::Spline &spline) { AddSpline(style, model, spline); });
            });
    }

    _overlays.SubmitOverlayLines(_splineLinesDepthTested, /*onTop=*/ false);
    _overlays.SubmitOverlayLines(_splineLinesOnTop, /*onTop=*/ true);
}

} // namespace Assisi::Editor
