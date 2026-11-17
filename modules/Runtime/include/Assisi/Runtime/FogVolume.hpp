/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file FogVolume.hpp
/// @brief Mist a designer puts down by hand: over one swamp, in a hollow, in a
/// smoky room.
///
/// The level's Fog covers the whole world. A fog volume fills one shape with fog
/// of its own colour and thickness, thinning toward its edges, and nothing
/// outside it. There is a component per shape — FogBox, FogSphere, FogOutline —
/// each placed, turned and scaled with its entity, and each with the same six
/// fields for how the fog looks.
///
/// A volume is drawn from its shape alone, in front of whatever each pixel
/// shows: no shadows fall in it, so it carries no light shafts.
///
/// Not replicable: a fog volume is level data, which every machine reads from
/// the same file.

#include <Assisi/Core/Reflect/Annotations.hpp>
#include <Assisi/Geometry/Spline.hpp>
#include <Assisi/Math/Color.hpp>
#include <Assisi/Math/GLM.hpp>

#include <initializer_list>

namespace Assisi::Runtime
{

/// @name What every fog volume starts with
/// @{
/// How far you can see through it where it is thickest, in metres.
inline constexpr float kDefaultFogVolumeVisibility = 20.f;
/// How deep it thins toward its edges, in metres.
inline constexpr float kDefaultFogVolumeEdgeSoftness = 2.f;
/// As thick at its top as at its floor.
inline constexpr float kDefaultFogVolumeFalloff = 0.f;
/// A flat top.
inline constexpr float kDefaultFogVolumePile = 0.f;
/// @}

/// @brief A box of fog, centred on its entity.
ACOMP()
struct FogBox
{
    /// Width, height and depth, in metres. A box with any of them zero or less
    /// holds no fog.
    AFIELD() glm::vec3 size{10.f, 4.f, 10.f};

    /// How far you can see through it where it is thickest, in metres.
    AFIELD(min = 0) float visibility = kDefaultFogVolumeVisibility;
    /// How deep, in metres, it thins toward its faces, so it has no hard edge.
    AFIELD(min = 0) float edgeSoftness = kDefaultFogVolumeEdgeSoftness;
    /// How many metres above its floor it takes to thin to about a third, so
    /// it lies thickest at the bottom. Zero keeps it the same at every height.
    AFIELD(min = 0) float falloff = kDefaultFogVolumeFalloff;
    /// How much it heaps up in the middle: zero keeps its top flat, and one
    /// domes the top down to the floor at the edges, so it stands tall in the
    /// middle and lies low and thin toward the sides.
    AFIELD(min = 0, max = 1) float pile = kDefaultFogVolumePile;
    /// With @ref litBySky, a tint over the light the level's haze takes from the
    /// sky: white matches the haze and follows the time of day. Without, the
    /// colour drawn at every hour.
    AFIELD() Assisi::Math::Color3<Assisi::Math::ColorSpace::Linear> color{1.f, 1.f, 1.f};
    /// Off for caves and rooms the sky does not reach.
    AFIELD() bool litBySky = true;
};

/// @brief A sphere of fog, centred on its entity. Scaling the entity stretches
/// it into an ellipsoid.
ACOMP()
struct FogSphere
{
    /// In metres.
    AFIELD(min = 0) float radius = 5.f;

    /// The same six as FogBox's. The floor is the sphere's lowest point.
    AFIELD(min = 0) float visibility = kDefaultFogVolumeVisibility;
    AFIELD(min = 0) float edgeSoftness = kDefaultFogVolumeEdgeSoftness;
    AFIELD(min = 0) float falloff = kDefaultFogVolumeFalloff;
    AFIELD(min = 0, max = 1) float pile = kDefaultFogVolumePile;
    AFIELD() Assisi::Math::Color3<Assisi::Math::ColorSpace::Linear> color{1.f, 1.f, 1.f};
    AFIELD() bool litBySky = true;
};

/// @brief The outline a new FogOutline starts with: a round patch ten metres
/// across.
[[nodiscard]] inline Geometry::Spline DefaultFogOutline()
{
    constexpr float kHalf = 5.f;
    Geometry::Spline outline;
    outline.closed = true;
    for (const glm::vec2 corner : {glm::vec2(-kHalf, -kHalf), glm::vec2(kHalf, -kHalf), glm::vec2(kHalf, kHalf),
                                   glm::vec2(-kHalf, kHalf)})
    {
        outline.nodes.push_back(Geometry::SplineNode{.position = glm::vec3(corner.x, 0.f, corner.y)});
    }
    return outline;
}

/// @brief Fog filling a closed curve, seen from above, from its entity's origin
/// up to a height: mist lying in a hollow or over a swamp.
ACOMP()
struct FogOutline
{
    /// The shape seen from above. Only where its nodes are across matters, not
    /// their height, and it always closes, from the last node back to the first.
    AFIELD() Geometry::Spline outline = DefaultFogOutline();
    /// How high above the entity's origin it is filled, in metres.
    AFIELD(min = 0) float height = 4.f;

    /// The same six as FogBox's. The floor is the entity's origin, and the pile
    /// peaks over the middle of the outline's bounds.
    AFIELD(min = 0) float visibility = kDefaultFogVolumeVisibility;
    AFIELD(min = 0) float edgeSoftness = kDefaultFogVolumeEdgeSoftness;
    AFIELD(min = 0) float falloff = kDefaultFogVolumeFalloff;
    AFIELD(min = 0, max = 1) float pile = kDefaultFogVolumePile;
    AFIELD() Assisi::Math::Color3<Assisi::Math::ColorSpace::Linear> color{1.f, 1.f, 1.f};
    AFIELD() bool litBySky = true;
};

} // namespace Assisi::Runtime
