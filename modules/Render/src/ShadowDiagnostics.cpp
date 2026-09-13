/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/ShadowDiagnostics.hpp>

#include <Assisi/Render/ShadowAtlas.hpp>

#include <algorithm>

namespace Assisi::Render
{

namespace
{
/// @brief Where @p kind / @p lightIndex sits in @p requests, or the size of the
/// span when the selection never kept it.
///
/// A walk rather than a map: the requests are the lights that won tiles, which
/// the caps hold to a couple of dozen, and a map built per frame would cost more
/// than the walk it replaced.
std::size_t FindRequest(std::span<const LocalShadowRequest> requests, LocalLightKind kind, std::uint32_t lightIndex)
{
    for (std::size_t index = 0; index < requests.size(); ++index)
    {
        if (requests[index].kind == kind && requests[index].lightIndex == lightIndex)
        {
            return index;
        }
    }
    return requests.size();
}
} // namespace

void ShadowDiagnostics::Clear()
{
    lights.clear();
    shadowedLights = 0;
    castingLights = 0;
    deferredFaces = 0;
    budgetFaces = 0;
    cascadeCasters.fill(0);
    cascadeCount = 0;
    cascadeAgeFrames.fill(0);
    cascadesRedrawn = 0;
}

const LocalShadowLightReport *ShadowDiagnostics::Find(LocalLightKind kind, std::uint32_t lightIndex) const
{
    const auto found = std::find_if(lights.begin(), lights.end(),
                                    [&](const LocalShadowLightReport &report)
                                    { return report.kind == kind && report.lightIndex == lightIndex; });
    return found == lights.end() ? nullptr : &*found;
}

void BuildLocalShadowDiagnostics(const LocalShadowDiagnosticsFrame &frame, ShadowDiagnostics &out)
{
    out.lights.clear();
    out.shadowedLights = 0;
    out.castingLights = static_cast<std::uint32_t>(frame.candidates.size());
    out.deferredFaces = frame.deferredFaces;
    out.budgetFaces = frame.budgetFaces;

    for (const LocalShadowCandidate &candidate : frame.candidates)
    {
        LocalShadowLightReport &report = out.lights.emplace_back();
        report.kind = candidate.kind;
        report.lightIndex = candidate.lightIndex;
        report.score = LocalShadowScore(candidate);


        if (!frame.active)
        {
            report.state = LocalShadowState::Disabled;
            continue;
        }

        const std::size_t request = FindRequest(frame.requests, candidate.kind, candidate.lightIndex);
        if (request == frame.requests.size())
        {
            // Absent from the selection entirely, which only the cap does. The
            // atlas never sees a light it turned away, so nothing further down
            // could have an opinion about this one.
            report.state = LocalShadowState::CappedOut;
            continue;
        }

        report.requestedResolution = ShadowSizeClassResolution(frame.requests[request].sizeClass);

        const auto served = std::find_if(frame.served.begin(), frame.served.end(),
                                         [&](const LocalShadowServedTile &tile)
                                         { return tile.requestIndex == request; });
        if (served == frame.served.end())
        {
            // Two ways to be selected and still go dark, and they are two
            // different settings. The plan is the only thing that knows which:
            // with caching off there are no plans and nothing could have been
            // deferred, so the atlas is the answer by construction.
            const bool deferred = request < frame.plans.size() && frame.plans[request].deferred;
            report.state = deferred ? LocalShadowState::Deferred : LocalShadowState::AtlasFull;
            continue;
        }

        report.resolution = served->resolution;
        ++out.shadowedLights;
        // Demotion is the atlas refusing the class, not the selector choosing a
        // smaller one: a light far enough away to want less is working exactly
        // as intended and has not left the fast path.
        report.state = served->resolution < report.requestedResolution ? LocalShadowState::Demoted
                                                                       : LocalShadowState::Shadowed;
    }
}

const char *LocalShadowStateName(LocalShadowState state)
{
    switch (state)
    {
    case LocalShadowState::Shadowed:
        return "shadowed";
    case LocalShadowState::Demoted:
        return "demoted";
    case LocalShadowState::Deferred:
        return "deferred";
    case LocalShadowState::AtlasFull:
        return "atlas full";
    case LocalShadowState::CappedOut:
        return "capped out";
    case LocalShadowState::Disabled:
        return "shadows off";
    }
    return "unshadowed";
}

const char *DescribeLocalShadowState(LocalShadowState state)
{
    switch (state)
    {
    case LocalShadowState::Shadowed:
        return "Shadowed at full size.";
    case LocalShadowState::Demoted:
        return "Shadowed, but at a smaller tile than it asked for — the atlas had no rectangle of that class. "
               "A larger atlas, or fewer shadowed lights, gives it back.";
    case LocalShadowState::Deferred:
        return "Unshadowed this frame: the redraw budget has not reached it yet. Expected on the frame a room "
               "opens; if it stays, raise the budget or move casters off it.";
    case LocalShadowState::AtlasFull:
        return "Unshadowed: the atlas had no room for it even at the smallest tile. Raise the atlas resolution "
               "or lower the shadowed-light caps.";
    case LocalShadowState::CappedOut:
        return "Unshadowed: the importance cap ranked it out. Raise the cap for its type, bias it with "
               "shadowPriority, or pin it always-shadowed.";
    case LocalShadowState::Disabled:
        return "Local-light shadows are switched off, so nothing decided this light in particular. Turn Cast "
               "Shadows back on to see what it would get.";
    }
    return "Unshadowed.";
}

} // namespace Assisi::Render
