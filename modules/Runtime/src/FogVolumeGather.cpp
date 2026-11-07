/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Runtime/FogVolumeGather.hpp>

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/WorldMatrix.hpp>
#include <Assisi/Geometry/Spline.hpp>
#include <Assisi/Runtime/FogVolume.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <vector>

namespace Assisi::Runtime
{
namespace
{

// An outline is flattened to lines no further from the curve than this share
// of its edge softness: the soft band hides corners that much smaller than it.
constexpr float kOutlineToleranceOfSoftness = 0.1f;
// And never finer than this, in metres, however hard its edge.
constexpr float kMinOutlineTolerance = 0.02f;
// The most edges one outline is flattened to. Every pixel behind it tests each
// one, so a curve that would take more is flattened more coarsely instead.
constexpr std::size_t kMaxEdgesPerOutline = 256u;
// How many times the tolerance doubles looking for few enough edges.
constexpr int32_t kMaxCoarsenings = 8;
// An axis scaled to less than this has no size to fill.
constexpr float kMinAxisScale = 1.0e-6f;

/// @brief A world matrix as a frame with no scale, and the scale it had.
struct ScaledFrame
{
    glm::mat4 frame{1.f};
    glm::vec3 scale{1.f};
};

std::optional<ScaledFrame> SplitScale(const glm::mat4 &world)
{
    ScaledFrame split{.frame = world};
    for (glm::length_t axis = 0; axis < 3; ++axis)
    {
        const float length = glm::length(glm::vec3(world[axis]));
        if (!(length >= kMinAxisScale))
        {
            return std::nullopt;
        }
        split.scale[axis] = length;
        split.frame[axis] = world[axis] / length;
    }
    return split;
}

// The outline's points seen from above, scaled across as its entity is.
std::vector<glm::vec2> OutlinePoints(const Geometry::Spline &outline, float softness, const glm::vec3 &scale)
{
    float tolerance = std::max(kMinOutlineTolerance, softness * kOutlineToleranceOfSoftness);
    Geometry::SplinePolyline chain = Geometry::FlattenSpline(outline, tolerance);
    for (int32_t attempt = 0; attempt < kMaxCoarsenings && chain.points.size() > kMaxEdgesPerOutline + 1u; ++attempt)
    {
        tolerance *= 2.f;
        chain = Geometry::FlattenSpline(outline, tolerance);
    }
    std::vector<glm::vec2> points;
    points.reserve(chain.points.size());
    for (const glm::vec3 &point : chain.points)
    {
        points.emplace_back(point.x * scale.x, point.z * scale.z);
    }
    return points;
}

/// @brief The volumes gathered so far, and the screen squares each covers.
class Gathering
{
public:
    Gathering(const FogVolumeView &view, Render::FogVolumeFrame &frame) : _view(view), _frame(frame)
    {
        _frame.Clear();
        _frame.grid = Render::FogTileGridFor(view.width, view.height);
    }

    /// @brief The world matrix @p world measured from the frame's origin, split
    /// into a frame and a scale; none for one with an axis of no size.
    [[nodiscard]] std::optional<ScaledFrame> Place(const ECS::WorldMatrix &world) const
    {
        return SplitScale(ECS::MatrixFrom(world, _view.origin));
    }

    /// @brief How the fog of a volume component looks, before its shape.
    template <typename Volume>
    [[nodiscard]] static Render::FogVolumeDesc LookOf(const Volume &volume, const glm::mat4 &frame)
    {
        return Render::FogVolumeDesc{.frame = frame,
                                     .color = volume.color,
                                     .visibility = volume.visibility,
                                     .edgeSoftness = volume.edgeSoftness,
                                     .falloff = volume.falloff,
                                     .litBySky = volume.litBySky};
    }

    /// @brief Room in the frame's outline edges for @p count more.
    [[nodiscard]] bool EdgesFit(std::size_t count)
    {
        if (_frame.edges.size() + count > Render::kMaxFogOutlineEdges)
        {
            ++_result.dropped;
            return false;
        }
        return true;
    }

    [[nodiscard]] std::vector<Render::FogOutlineEdgeGpu> &Edges() { return _frame.edges; }

    /// @brief Adds @p desc when it has a size and is on screen. Takes back the
    /// outline edges added for it, from @p edgesBefore on, when it does not.
    void Add(const Render::FogVolumeDesc &desc, std::size_t edgesBefore)
    {
        const bool hasSize = desc.visibility > 0.f && glm::all(glm::greaterThan(desc.halfExtents, glm::vec3(0.f)));
        const std::optional<Render::FogTileRect> rect =
            hasSize ? Render::FogVolumeTileRect(_view.viewProjection, desc.frame, desc.halfExtents, _frame.grid)
                    : std::nullopt;
        if (!rect.has_value() || _frame.volumes.size() >= Render::kMaxFogVolumes)
        {
            if (rect.has_value())
            {
                ++_result.dropped;
            }
            _frame.edges.resize(edgesBefore);
            return;
        }
        _frame.volumes.push_back(Render::MakeFogVolume(desc));
        _rects.push_back(*rect);
    }

    [[nodiscard]] FogVolumeGatherResult Finish()
    {
        if (!_frame.volumes.empty())
        {
            Render::BuildFogTileLists(_frame.grid, _rects, _frame.tiles);
        }
        return _result;
    }

private:
    const FogVolumeView &_view;
    Render::FogVolumeFrame &_frame;
    std::vector<Render::FogTileRect> _rects;
    FogVolumeGatherResult _result;
};

void GatherBoxes(const ECS::Scene &scene, Gathering &gathering)
{
    for (auto [entity, world, box] : scene.Query<ECS::WorldMatrix, FogBox>())
    {
        const std::optional<ScaledFrame> placed = gathering.Place(world);
        if (!placed.has_value())
        {
            continue;
        }
        Render::FogVolumeDesc desc = Gathering::LookOf(box, placed->frame);
        desc.halfExtents = 0.5f * box.size * placed->scale;
        desc.kind = Render::FogVolumeKind::Box;
        gathering.Add(desc, gathering.Edges().size());
    }
}

void GatherSpheres(const ECS::Scene &scene, Gathering &gathering)
{
    for (auto [entity, world, sphere] : scene.Query<ECS::WorldMatrix, FogSphere>())
    {
        const std::optional<ScaledFrame> placed = gathering.Place(world);
        if (!placed.has_value())
        {
            continue;
        }
        Render::FogVolumeDesc desc = Gathering::LookOf(sphere, placed->frame);
        desc.halfExtents = sphere.radius * placed->scale;
        desc.kind = Render::FogVolumeKind::Ellipsoid;
        gathering.Add(desc, gathering.Edges().size());
    }
}

void GatherOutlines(const ECS::Scene &scene, Gathering &gathering)
{
    for (auto [entity, world, outline] : scene.Query<ECS::WorldMatrix, FogOutline>())
    {
        const std::optional<ScaledFrame> placed = gathering.Place(world);
        if (!placed.has_value() || !(outline.visibility > 0.f))
        {
            continue;
        }
        const std::vector<glm::vec2> points = OutlinePoints(outline.outline, outline.edgeSoftness, placed->scale);
        if (!gathering.EdgesFit(points.size()))
        {
            continue;
        }
        const std::size_t edgesBefore = gathering.Edges().size();
        const std::optional<Render::FogOutlinePlacement> placement =
            Render::AppendFogOutline(gathering.Edges(), points);
        if (!placement.has_value())
        {
            continue;
        }
        // From the entity's origin up, so the box's middle is half the height
        // above it.
        const float halfHeight = 0.5f * outline.height * placed->scale.y;
        const glm::vec3 middle(placement->centre.x, halfHeight, placement->centre.y);
        Render::FogVolumeDesc desc = Gathering::LookOf(outline, placed->frame * glm::translate(glm::mat4(1.f), middle));
        desc.halfExtents = glm::vec3(placement->halfExtents.x, halfHeight, placement->halfExtents.y);
        desc.firstEdge = placement->firstEdge;
        desc.edgeCount = placement->edgeCount;
        desc.kind = Render::FogVolumeKind::Outline;
        gathering.Add(desc, edgesBefore);
    }
}

} // namespace

FogVolumeGatherResult GatherFogVolumes(const ECS::Scene &scene, const FogVolumeView &view,
                                       Render::FogVolumeFrame &frame)
{
    Gathering gathering(view, frame);
    GatherBoxes(scene, gathering);
    GatherSpheres(scene, gathering);
    GatherOutlines(scene, gathering);
    return gathering.Finish();
}

} // namespace Assisi::Runtime
