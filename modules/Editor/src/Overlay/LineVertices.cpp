/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Editor/Overlay/LineVertices.hpp>

#include <cstddef>
#include <cstdint>

namespace Assisi::Editor
{
namespace
{

// The corners that draw one segment as two triangles, as (which end, which
// side) pairs: end 0 is the segment's start. A corner at the far end pushes the
// opposite way to reach the same side, because its "along" runs backwards.
struct ThickCorner
{
    int32_t end;
    float side;
};
constexpr ThickCorner kThickCorners[] = {{0, -1.f}, {0, 1.f}, {1, -1.f}, {0, -1.f}, {1, -1.f}, {1, 1.f}};

} // namespace

void AppendThickLines(std::vector<ThickLineVertex> &out, std::span<const LineVertex> lines, float widthPixels)
{
    const float halfWidth = widthPixels * 0.5f;
    for (std::size_t i = 0; i + 1 < lines.size(); i += 2)
    {
        const LineVertex *ends[] = {&lines[i], &lines[i + 1]};
        for (const ThickCorner &corner : kThickCorners)
        {
            const LineVertex &here = *ends[corner.end];
            const LineVertex &there = *ends[1 - corner.end];
            out.push_back(ThickLineVertex{.color = here.color,
                                          .position = here.position,
                                          .other = there.position,
                                          .side = corner.side,
                                          .halfWidth = halfWidth});
        }
    }
}

} // namespace Assisi::Editor
