/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file EditorSplines.cpp
/// @brief Every spline in the level, drawn where it lies, and shaped by hand.
///
/// Found by reflection (SplineFields.hpp), so a component that gains a Spline
/// field is drawn and edited with no code here. Each curve is drawn as the
/// chain Geometry::FlattenSpline gives, from the same math anything that
/// follows the curve uses, so the drawing cannot disagree with it.
///
/// Shaping works on one point at a time, a node or a Bezier handle, chosen by
/// clicking its marker. The gizmo then moves that point instead of the entity,
/// the same way it turns a directional light's aim, and each drag, insert and
/// removal is one undo step on the component holding the spline.

#include <Assisi/Editor/EditorApp.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/WorldMatrix.hpp>
#include <Assisi/Editor/EditHistory.hpp>
#include <Assisi/Editor/ListEdit.hpp>
#include <Assisi/Editor/Overlay/LinePass.hpp>
#include <Assisi/Editor/Overlay/OverlayRenderer.hpp>
#include <Assisi/Editor/SplineEdit.hpp>
#include <Assisi/Editor/SplineFields.hpp>
#include <Assisi/Editor/WireShapes.hpp>
#include <Assisi/Geometry/Spline.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Runtime/Camera.hpp>

#include <imgui.h> // must precede ImGuizmo.h — it uses ImVec2/ImDrawList/ImU32 unguarded

#include <ImGuizmo.h>

namespace Assisi::Editor
{
namespace
{

constexpr glm::vec4 kSelectedColor{kSelectionOutline, 1.0f};
constexpr glm::vec4 kActiveSelectedColor{kActiveSelectionOutline, 1.0f};

/// The node or handle being shaped: white, and this much larger than the rest.
constexpr glm::vec4 kShapingColor{1.0f, 1.0f, 1.0f, 1.0f};
constexpr float kShapingScale = 1.5f;

/// An arrowhead's half-angle: narrow enough that its rim stays inside the cube
/// a click on it is tested against (MarkerHalfExtent).
constexpr float kArrowheadHalfAngleDegrees = 25.f;

/// Which way an arrowhead points where the curve gives no direction: a lone
/// node, or two nodes in one place.
constexpr glm::vec3 kNoDirection = kAxisY;

/// Shorter than this, a direction has no length to point along.
constexpr float kMinDirectionLength = 1e-6f;

/// What a node moves by with Ctrl held: half a metre, as an entity does under
/// the transform gizmo.
constexpr float kNodeSnapStep = 0.5f;

/// Which batches an entity's splines go in, in what colour, and whether their
/// nodes show. Curves and markers are apart because only curves are drawn at
/// the preferred thickness.
struct SplineStyle
{
    std::vector<LineVertex> *curves = nullptr;
    std::vector<LineVertex> *markers = nullptr;
    glm::vec4 color{0.f};
    bool detailed = false;
};

using EntitySplineVisitor =
    std::function<void (Core::Reflect::ComponentId, Geometry::Spline &, std::span<const FieldStep>)>;

/// Where @p local, in the holder's space, lands in the overlay's.
glm::vec3 Placed(const glm::mat4 &model, const glm::vec3 &local)
{
    return glm::vec3(model * glm::vec4(local, 1.f));
}

/// A flat disc square to @p normal: a ring and two diameters across it, so it
/// reads as a plate from the side rather than as a hoop.
void AddDisc(std::vector<LineVertex> &out, const glm::vec4 &color, const glm::vec3 &centre, const glm::vec3 &normal,
             float radius)
{
    // AimAlong carries -Y onto the normal, so X and Z span the disc.
    const glm::mat4 basis = glm::translate(glm::mat4(1.f), centre) * AimAlong(normal);
    AddArc(out, basis, color, glm::vec3(0.f), kAxisX, kAxisZ, radius, 0.f, glm::two_pi<float>(), kCircleSegments);
    AddSegment(out, basis, color, kAxisX * radius, -kAxisX * radius);
    AddSegment(out, basis, color, kAxisZ * radius, -kAxisZ * radius);
}

/// A node's arrowhead at @p at, pointing along @p direction, with the disc an
/// end has: at the base for the start, at the tip for the end.
void AddArrowhead(const SplineStyle &style, const glm::vec3 &at, const glm::vec3 &direction, SplineMarker marker,
                  bool shaping)
{
    const glm::vec4 &color = shaping ? kShapingColor : style.color;
    const float scale = shaping ? kShapingScale : 1.f;
    const float half = kArrowheadHalfLength * scale;
    const glm::vec3 tip = at + direction * half;

    // The cone opens along -Y from its apex; aimed backwards, it opens from the
    // tip back over the node.
    AddConeWireframe(*style.markers, glm::translate(glm::mat4(1.f), tip) * AimAlong(-direction), color,
                     kArrowheadHalfAngleDegrees, 2.f * half, /*drawAxis=*/ false);
    if (marker == SplineMarker::Start)
    {
        AddDisc(*style.markers, color, at - direction * half, direction, kEndDiscRadius * scale);
    }
    else if (marker == SplineMarker::End)
    {
        AddDisc(*style.markers, color, tip, direction, kEndDiscRadius * scale);
    }
}

/// The way the curve travels at node @p index, in the overlay's space, or
/// kNoDirection where it does not move.
glm::vec3 DirectionAt(const glm::mat4 &model, const Geometry::Spline &spline, std::size_t index)
{
    const glm::vec3 local = Geometry::SplineDirection(spline, static_cast<float>(index));
    const glm::vec3 placed = glm::vec3(model * glm::vec4(local, 0.f));
    const float length = glm::length(placed);
    return length < kMinDirectionLength ? kNoDirection : placed / length;
}

/// A handle's box, in the shaping colour and size when it is the point being
/// shaped.
void AddHandleBox(const SplineStyle &style, const glm::vec3 &at, bool shaping)
{
    const glm::vec4 &color = shaping ? kShapingColor : style.color;
    const float half = shaping ? kHandleHalfExtent * kShapingScale : kHandleHalfExtent;
    AddBoxWireframe(*style.markers, glm::translate(glm::mat4(1.f), at), color, glm::vec3(half));
}

/// An arrowhead on each node, and the handles of each Bezier node. Placed
/// through @p model but not scaled by it, so a node reads the same size on any
/// entity. @p shaping is the point being shaped, when it is on this spline.
void AddNodes(const SplineStyle &style, const glm::mat4 &model, const Geometry::Spline &spline,
              std::optional<SplinePart> shaping)
{
    const glm::mat4 identity(1.f);
    for (std::size_t i = 0; i < spline.nodes.size(); ++i)
    {
        const Geometry::SplineNode &node = spline.nodes[i];
        const SplinePart self{.node = i, .kind = SplinePartKind::Node};
        const glm::vec3 at = Placed(model, node.position);
        AddArrowhead(style, at, DirectionAt(model, spline, i), MarkerOf(spline, self), shaping == self);
        if (node.mode != Geometry::SplineNodeMode::Bezier)
        {
            continue;
        }
        for (const SplinePartKind kind : {SplinePartKind::HandleIn, SplinePartKind::HandleOut})
        {
            const SplinePart handle{.node = i, .kind = kind};
            const glm::vec3 tip = Placed(model, PartPosition(spline, handle));
            AddSegment(*style.markers, identity, style.color, at, tip);
            AddHandleBox(style, tip, shaping == handle);
        }
    }
}

/// One spline's curve, and its nodes when the style shows them.
void AddSpline(const SplineStyle &style, const glm::mat4 &model, const Geometry::Spline &spline,
               std::optional<SplinePart> shaping)
{
    const Geometry::SplinePolyline chain = Geometry::FlattenSpline(spline, kSplineDrawTolerance);
    for (std::size_t i = 1; i < chain.points.size(); ++i)
    {
        AddSegment(*style.curves, model, style.color, chain.points[i - 1], chain.points[i]);
    }
    if (style.detailed)
    {
        AddNodes(style, model, spline, shaping);
    }
}

/// Where @p entity's splines are placed from: its world matrix measured from
/// @p origin, or the world itself for an entity with no Transform.
glm::mat4 ModelFor(ECS::Scene &scene, ECS::Entity entity, const glm::dvec3 &origin)
{
    const ECS::WorldMatrix *world = scene.Get<ECS::WorldMatrix>(entity);
    return world != nullptr ? ECS::MatrixFrom(*world, origin) : glm::translate(glm::mat4(1.f), glm::vec3(-origin));
}

/// Every spline on @p entity, with the component holding it and the path to it.
///
/// Read through getByEntity, which does not stamp a change: the storage is the
/// scene's own and writable, and every caller that writes reports its change.
void ForEachEntitySpline(ECS::Scene &scene, ECS::Entity entity, const EntitySplineVisitor &visit)
{
    for (const Core::Reflect::ComponentMeta &meta : Core::Reflect::ComponentRegistry::Instance().All())
    {
        if (!meta.getByEntity || !HoldsSpline(meta.fields))
        {
            continue;
        }
        const void *component = meta.getByEntity(&scene, entity.index, entity.generation);
        if (component == nullptr)
        {
            continue;
        }
        ForEachSplinePath(const_cast<void *>(component), meta.fields,
                          [&](Geometry::Spline &spline, std::span<const FieldStep> path)
                          { visit(meta.id, spline, path); });
    }
}

/// What one click finds among one entity's splines: the nearest node or handle
/// on screen, the nearest stretch of curve to the eye, and the first spline with
/// no nodes at all.
class SplineClick
{
public:
    SplineClick(const PickRay &ray, glm::vec2 cursor, const glm::mat4 &model, ECS::Entity entity)
        : _ray(ray), _model(model), _cursor(cursor), _entity(entity)
    {
    }

    void Consider(Core::Reflect::ComponentId component, const Geometry::Spline &spline,
                  std::span<const FieldStep> path)
    {
        const SplineTarget where{
            .path = {path.begin(), path.end()}, .part = {}, .entity = _entity, .component = component};
        if (const std::optional<SplinePartHit> hit = PickSplinePart(_ray, _cursor, _model, spline);
            hit && hit->pixels < _partPixels)
        {
            _partPixels = hit->pixels;
            _part = where;
            _part->part = hit->part;
        }
        if (const std::optional<SplineCurveHit> hit = PickSplineCurve(_ray, _cursor, _model, spline);
            hit && hit->distance < _curveDistance)
        {
            _curveDistance = hit->distance;
            _curveParameter = hit->parameter;
            _curve = where;
        }
        if (spline.nodes.empty() && !_empty)
        {
            _empty = where;
        }
    }

    [[nodiscard]] const std::optional<SplineTarget> &Part() const { return _part; }
    [[nodiscard]] const std::optional<SplineTarget> &Curve() const { return _curve; }
    [[nodiscard]] float CurveParameter() const { return _curveParameter; }
    [[nodiscard]] const std::optional<SplineTarget> &Empty() const { return _empty; }

private:
    std::optional<SplineTarget> _part;
    std::optional<SplineTarget> _curve;
    std::optional<SplineTarget> _empty;
    const PickRay &_ray;
    glm::mat4 _model;
    glm::vec2 _cursor;
    ECS::Entity _entity;
    float _partPixels = std::numeric_limits<float>::max();
    float _curveDistance = std::numeric_limits<float>::max();
    float _curveParameter = 0.f;
};

} // namespace

void EditorApp::SubmitSplineGizmos()
{
    if (_scene == nullptr)
    {
        return;
    }

    _splineCurvesDepthTested.clear();
    _splineCurvesOnTop.clear();
    _splineMarkers.clear();

    // The selection on top, so a spline being shaped is never lost behind the
    // ground it lies on. The rest take the preferred colour.
    const SplineStyle behind{.curves = &_splineCurvesDepthTested,
                             .markers = &_splineMarkers,
                             .color = _preferences.splines.color,
                             .detailed = false};
    const SplineStyle selected{
        .curves = &_splineCurvesOnTop, .markers = &_splineMarkers, .color = kSelectedColor, .detailed = true};
    const SplineStyle active{
        .curves = &_splineCurvesOnTop, .markers = &_splineMarkers, .color = kActiveSelectedColor, .detailed = true};

    // Told apart by address: the target's path leads to exactly one spline.
    const Geometry::Spline *shaped = _splineTarget ? SplineOf(*_splineTarget) : nullptr;

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
                              [&](const Geometry::Spline &spline)
                {
                    AddSpline(style, model, spline,
                              &spline == shaped ? std::optional(_splineTarget->part) : std::nullopt);
                });
            });
    }

    const float thickness = _preferences.splines.thickness;
    _overlays.SubmitThickOverlayLines(_splineCurvesDepthTested, /*onTop=*/ false, thickness);
    _overlays.SubmitThickOverlayLines(_splineCurvesOnTop, /*onTop=*/ true, thickness);
    _overlays.SubmitOverlayLines(_splineMarkers, /*onTop=*/ true);
}

Geometry::Spline *EditorApp::SplineOf(const SplineTarget &target)
{
    if (_scene == nullptr || !_scene->IsAlive(target.entity))
    {
        return nullptr;
    }
    const Core::Reflect::ComponentMeta *meta = Core::Reflect::ComponentRegistry::Instance().ById(target.component);
    if (meta == nullptr || !meta->getByEntity)
    {
        return nullptr;
    }
    const void *component = meta->getByEntity(_scene, target.entity.index, target.entity.generation);
    if (component == nullptr)
    {
        return nullptr;
    }
    std::byte *found = ResolveFieldPath(static_cast<std::byte *>(const_cast<void *>(component)), target.path);
    return found == nullptr ? nullptr : static_cast<Geometry::Spline *>(static_cast<void *>(found));
}

void EditorApp::EditSplineOnce(const SplineTarget &target, std::string_view action,
                               const std::function<std::optional<std::size_t>(Geometry::Spline &)> &edit)
{
    Geometry::Spline *spline = SplineOf(target);
    if (spline == nullptr)
    {
        return;
    }

    // An edit outside the inspector, so nothing else records it: a gesture opened
    // and closed around it, as the eyedropper does.
    EditHistory *history = ActiveHistory();
    if (history != nullptr)
    {
        history->RecordBefore(target.entity, target.component, EditLabel(action, target.entity), target.entity);
    }
    const std::optional<std::size_t> node = edit(*spline);
    if (history != nullptr)
    {
        history->CommitGesture(target.entity, target.component);
    }
    _scene->MarkChanged(target.entity, target.component);

    _splineTarget.reset();
    if (node)
    {
        SplineTarget shaped = target;
        shaped.part = SplinePart{.node = *node, .kind = SplinePartKind::Node};
        _splineTarget = std::move(shaped);
    }
}

bool EditorApp::HandleSplineClick(glm::vec2 cursor)
{
    // The condition the nodes are drawn under: a marker that is not on screen is
    // not a click target.
    if (_scene == nullptr || !_showEditorOverlays || _playState == PlayState::Playing ||
        _selectedEntity == ECS::NullEntity || !_scene->IsAlive(_selectedEntity) || !IsEditable(_selectedEntity))
    {
        return false;
    }
    const PickRay ray = BuildPickRay(cursor);
    if (!ray.valid)
    {
        return false;
    }

    const glm::mat4 model = ModelFor(*_scene, _selectedEntity, ray.measuredFrom);
    SplineClick click{ray, cursor, model, _selectedEntity};
    ForEachEntitySpline(*_scene, _selectedEntity,
                        [&click](Core::Reflect::ComponentId component, Geometry::Spline &spline,
                                 std::span<const FieldStep> path) { click.Consider(component, spline, path); });

    if (!ImGui::GetIO().KeyAlt)
    {
        if (click.Part())
        {
            const bool already = _splineTarget == click.Part();
            const bool doubleClick = ImGui::GetIO().MouseDoubleClicked[ImGuiMouseButton_Left];

            // A double-click on the node already being shaped steps its mode. Not
            // one whose first click picked it: that first click only chose the
            // node, and a mode changing under a plain double-click to select would
            // be a surprise.
            if (already && doubleClick && !_splineTargetFresh && click.Part()->part.kind == SplinePartKind::Node)
            {
                const std::size_t node = click.Part()->part.node;
                EditSplineOnce(*click.Part(), "Change Node Mode",
                               [node](Geometry::Spline &spline)
                    {
                        CycleNodeMode(spline, node);
                        return std::optional<std::size_t>(node);
                    });
            }
            _splineTargetFresh = !already;
            _splineTarget = click.Part();
            return true;
        }
        // A click away from every marker lets go of the point being shaped, and
        // is an ordinary pick from there.
        _splineTarget.reset();
        return false;
    }

    // Alt adds a node, and every Alt+click is the splines', so a miss never
    // deselects the entity being shaped.
    if (click.Curve())
    {
        const float parameter = click.CurveParameter();
        EditSplineOnce(*click.Curve(), "Add Spline Node",
                       [parameter](Geometry::Spline &spline) { return InsertSplineNode(spline, parameter); });
        return true;
    }
    AddSplineNodeOffCurve(ray, model, click.Empty());
    return true;
}

void EditorApp::AddSplineNodeOffCurve(const PickRay &ray, const glm::mat4 &model,
                                      const std::optional<SplineTarget> &empty)
{
    std::optional<SplineTarget> where;
    bool atStart = false;
    float height = Placed(model, glm::vec3(0.f)).y;

    // Past the end node being shaped: a node in the middle, or anywhere on a
    // closed loop, has no "past" to extend into.
    if (_splineTarget && _splineTarget->part.kind == SplinePartKind::Node)
    {
        const Geometry::Spline *spline = SplineOf(*_splineTarget);
        const std::size_t node = _splineTarget->part.node;
        if (spline != nullptr && !spline->closed && node < spline->nodes.size() &&
            (node == 0 || node + 1 == spline->nodes.size()))
        {
            where = _splineTarget;
            // A lone node extends forward, so the one it gains is the second.
            atStart = node == 0 && spline->nodes.size() > 1;
            height = Placed(model, spline->nodes[node].position).y;
        }
    }
    if (!where)
    {
        where = empty;
    }
    if (!where)
    {
        return;
    }

    const std::optional<glm::vec3> onLevel = RayOnLevel(ray.origin, ray.direction, height);
    if (!onLevel)
    {
        return;
    }
    const glm::vec3 local = Placed(glm::inverse(model), *onLevel);
    EditSplineOnce(*where, "Add Spline Node",
                   [atStart, local](Geometry::Spline &spline) { return ExtendSpline(spline, atStart, local); });
}

void EditorApp::DeleteSplineNode()
{
    if (!_splineTarget)
    {
        return;
    }
    const SplineTarget target = *_splineTarget;
    EditSplineOnce(target, "Remove Spline Node",
                   [&target](Geometry::Spline &spline) -> std::optional<std::size_t>
        {
            if (target.part.node < spline.nodes.size())
            {
                RemoveSplineNode(spline, target.part.node);
            }
            return std::nullopt;
        });
}

ECS::Entity EditorApp::PickSplineCurveEntity(glm::vec2 cursor, float &tOut)
{
    tOut = std::numeric_limits<float>::max();
    if (_scene == nullptr || !_showEditorOverlays || _playState == PlayState::Playing)
    {
        return ECS::NullEntity;
    }
    const PickRay ray = BuildPickRay(cursor);
    if (!ray.valid)
    {
        return ECS::NullEntity;
    }

    ECS::Entity result = ECS::NullEntity;
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
                const glm::mat4 model = ModelFor(*_scene, entity, ray.measuredFrom);
                ForEachSpline(component, meta.fields,
                              [&](const Geometry::Spline &spline)
                {
                    const std::optional<SplineCurveHit> hit = PickSplineCurve(ray, cursor, model, spline);
                    if (hit && hit->distance < tOut)
                    {
                        tOut = hit->distance;
                        result = entity;
                    }
                });
            });
    }
    return result;
}

bool EditorApp::DrawSplineGizmo()
{
    if (!_splineTarget)
    {
        return false;
    }
    // The point belongs to the entity it was picked on. Selecting another, or an
    // undo that took the node away, lets go of it.
    const SplineTarget &target = *_splineTarget;
    Geometry::Spline *spline = SplineOf(target);
    const bool handle = target.part.kind != SplinePartKind::Node;
    if (target.entity != _selectedEntity || !IsEditable(target.entity) || spline == nullptr ||
        target.part.node >= spline->nodes.size() ||
        (handle && spline->nodes[target.part.node].mode != Geometry::SplineNodeMode::Bezier))
    {
        _splineTarget.reset();
        return false;
    }

    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList());
    ImGuizmo::SetRect(viewport->Pos.x, viewport->Pos.y, viewport->Size.x, viewport->Size.y);

    // The view the scene is drawn from, measured from that camera, as the
    // transform gizmo's is.
    ECS::WorldMatrix viewWorld;
    Runtime::Camera viewCamera;
    ViewCamera(viewWorld, viewCamera);
    const glm::dvec3 origin = ECS::WorldPositionOf(viewWorld);
    const float aspect = viewport->Size.y > 0.f ? viewport->Size.x / viewport->Size.y : 1.f;
    const glm::mat4 view = Runtime::ViewMatrix(ECS::MatrixFrom(viewWorld, origin));
    const glm::mat4 proj = Runtime::ProjectionMatrix(viewCamera, aspect);

    const glm::mat4 model = ModelFor(*_scene, target.entity, origin);
    glm::mat4 point = glm::translate(glm::mat4(1.f), Placed(model, PartPosition(*spline, target.part)));

    // Every frame the handles draw, as the light's aim does: RecordBefore keeps
    // the pre-drag spline for an open gesture and only refreshes it, which is
    // what stops the end-of-frame sweep committing the drag under itself.
    if (EditHistory *history = ActiveHistory(); history != nullptr)
    {
        history->RecordBefore(target.entity, target.component, EditLabel("Shape Spline", target.entity),
                              target.entity);
    }

    const glm::vec3 snap(kNodeSnapStep);
    const bool manipulated = ImGuizmo::Manipulate(&view[0][0], &proj[0][0], ImGuizmo::TRANSLATE, ImGuizmo::WORLD,
                                                  &point[0][0], nullptr, ImGui::GetIO().KeyCtrl ? &snap[0] : nullptr);
    const bool held = ImGuizmo::IsUsing();
    if (manipulated)
    {
        // Back into the spline's own space, through whatever the entity's turn
        // and scale are, so a node lands under the handle on any entity.
        MovePart(*spline, target.part, Placed(glm::inverse(model), glm::vec3(point[3])));
        _scene->MarkChanged(target.entity, target.component);
    }

    // The release edge is read by the caller, as for the light's aim.
    _splineGizmoHeld = held;
    if (held)
    {
        _captureEditingActive = true;
        _splineDragComponent = target.component;
        _splineDrag.Hold(target.entity, {});
    }
    return true;
}

} // namespace Assisi::Editor
