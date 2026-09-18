/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/ShadowImportance.hpp>

#include <Assisi/Render/ShadowAtlas.hpp>

#include <algorithm>
#include <cmath>

namespace Assisi::Render
{

void LocalShadowSelection::Clear()
{
    lights.clear();
    spotCount = 0;
    pointCount = 0;
    droppedByCap = 0;
}

float LocalShadowScore(const LocalShadowCandidate &candidate)
{
    if (!candidate.visible)
    {
        return 0.f;
    }
    const float coverage = candidate.screenCoverage;
    const float intensity = std::abs(candidate.intensity);
    const float priority = candidate.priority;
    if (!std::isfinite(coverage) || !std::isfinite(intensity) || !std::isfinite(priority))
    {
        return 0.f;
    }
    const float clamped = std::clamp(coverage, 0.f, 1.f);
    return intensity * clamped * clamped * std::exp2(std::clamp(priority, kMinShadowPriority, kMaxShadowPriority));
}

std::uint32_t StableSizeClass(std::uint32_t baseClass, float coverage, std::uint32_t previousClass, float hysteresis)
{
    baseClass = std::min(baseClass, kShadowSizeClassCount - 1u);
    if (!std::isfinite(coverage))
    {
        coverage = 0.f;
    }
    coverage = std::clamp(coverage, 0.f, 1.f);
    hysteresis = std::isfinite(hysteresis) ? std::clamp(hysteresis, 0.f, 1.f) : 0.f;

    // The demand, before anything resists it: every halving of coverage below
    // the reference drops one class. Floored rather than rounded, so a light
    // gets the class it can actually fill rather than the one it nearly can.
    std::uint32_t demand = 0;
    if (coverage > 0.f)
    {
        const float octaves = std::floor(std::log2(coverage / kFaceCoverageReference));
        const float below = std::max(-octaves, 0.f); // demand never promotes past baseClass
        demand = below >= static_cast<float>(baseClass) ? 0u : baseClass - static_cast<std::uint32_t>(below);
    }

    if (previousClass == kNoPreviousSizeClass || previousClass >= kShadowSizeClassCount)
    {
        return demand;
    }
    const std::uint32_t held = std::min(previousClass, baseClass);
    if (demand == held)
    {
        return held;
    }

    // The coverage at which the held class starts, and the one at which the next
    // one up starts. A change is taken only once the coverage has cleared the
    // relevant boundary by the margin — which is what stops a light sitting on a
    // boundary from resizing every other frame.
    const auto boundary = [&](std::uint32_t sizeClass)
                          {
                              return kFaceCoverageReference * std::exp2(static_cast<float>(sizeClass) - static_cast<float>(baseClass));
                          };

    if (demand > held)
    {
        return coverage >= boundary(held + 1u) * (1.f + hysteresis) ? demand : held;
    }
    return coverage <= boundary(held) / (1.f + hysteresis) ? demand : held;
}

void LocalShadowSelector::Forget()
{
    _previous.clear();
}

std::uint32_t LocalShadowSelector::PreviousClass(LocalLightKind kind, std::uint32_t index) const
{
    for (const LocalShadowAssignment &held : _previous)
    {
        if (held.kind == kind && held.lightIndex == index)
        {
            return held.sizeClass;
        }
    }
    return kNoPreviousSizeClass;
}

void LocalShadowSelector::Select(std::span<const LocalShadowCandidate> candidates, const LocalShadowSettings &local,
                                 const LocalShadowSelectionSettings &selection, LocalShadowSelection &out)
{
    out.Clear();
    _ranked.clear();

    const LocalShadowSettings safeLocal = Sanitized(local);
    const LocalShadowSelectionSettings safeSelection = Sanitized(selection);
    if (!safeLocal.enabled)
    {
        _previous.clear();
        return;
    }

    const std::uint32_t baseClass = ShadowSizeClassOf(safeLocal.faceResolution);

    _ranked.reserve(candidates.size());
    for (const LocalShadowCandidate &candidate : candidates)
    {
        if (!candidate.visible)
        {
            continue;
        }
        const std::uint32_t previousClass = PreviousClass(candidate.kind, candidate.lightIndex);
        const float score = LocalShadowScore(candidate);
        // A holder carries its margin into its own score rather than the
        // comparison carrying it, which is what makes the ordering independent
        // of the order the candidates arrive in: a margin stated between pairs
        // decides differently depending on which pair is compared first.
        const bool held = previousClass != kNoPreviousSizeClass;
        _ranked.push_back(Ranked{
                .assignment = LocalShadowAssignment{.kind = candidate.kind,
                                                    .lightIndex = candidate.lightIndex,
                                                    .score = score,
                                                    .sizeClass = StableSizeClass(baseClass, candidate.screenCoverage,
                                                                                 previousClass,
                                                                                 safeSelection.classHysteresis)},
                .effectiveScore = held ? score * (1.f + safeSelection.capHysteresis) : score,
                .pinned = candidate.pinned});
    }

    if (!safeSelection.capEnabled)
    {
        // No ordering at all: the atlas is still a fixed texture, so the
        // allocator fills it and whoever is left over goes unshadowed in arrival
        // order. Worse than the ordering under pressure, and the author's call.
        for (const Ranked &light : _ranked)
        {
            out.lights.push_back(light.assignment);
            (light.assignment.kind == LocalLightKind::Point ? out.pointCount : out.spotCount) += 1u;
        }
        _previous = out.lights;
        return;
    }

    // Pinned lights sort ahead of everything, because the pin means the cap does
    // not apply to them. The rest by score, ties broken by index so a frame's
    // answer is the same every time it is asked.
    std::stable_sort(_ranked.begin(), _ranked.end(),
                     [](const Ranked &lhs, const Ranked &rhs)
        {
            if (lhs.pinned != rhs.pinned)
            {
                return lhs.pinned;
            }
            if (lhs.effectiveScore != rhs.effectiveScore)
            {
                return lhs.effectiveScore > rhs.effectiveScore;
            }
            if (lhs.assignment.kind != rhs.assignment.kind)
            {
                return lhs.assignment.kind < rhs.assignment.kind;
            }
            return lhs.assignment.lightIndex < rhs.assignment.lightIndex;
        });

    std::uint32_t spots = 0;
    std::uint32_t points = 0;
    for (const Ranked &light : _ranked)
    {
        const bool isPoint = light.assignment.kind == LocalLightKind::Point;
        std::uint32_t &count = isPoint ? points : spots;
        const std::uint32_t cap = isPoint ? safeSelection.capPoint : safeSelection.capSpot;
        // A pin outranks the cap outright. That is what the knob is for: a bias
        // large enough to always win depends on what else the level places,
        // which is not something an author can know.
        if (!light.pinned && count >= cap)
        {
            ++out.droppedByCap;
            continue;
        }
        ++count;
        out.lights.push_back(light.assignment);
    }

    out.spotCount = spots;
    out.pointCount = points;
    _previous = out.lights;
}

} // namespace Assisi::Render
