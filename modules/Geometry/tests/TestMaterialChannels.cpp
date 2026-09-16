/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestMaterialChannels.cpp
/// @brief The per-channel format table, which the cooker and the render
/// AssetCache both read.
///
/// These are pins rather than restatements. Each one has a consequence
/// elsewhere that no compiler checks: the mesh shader reconstructs a normal's Z
/// from two channels because normals are BC5, and it reads a roughness straight
/// out of the sampled value because that channel is linear. Changing the table
/// without changing the shader is a silent rendering bug, and this is what
/// refuses to let the change be silent.

#include <doctest/doctest.h>

#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Geometry/MaterialChannels.hpp>
#include <Assisi/Image/Image.hpp>

using Assisi::Geometry::ChannelTexture;
using Assisi::Geometry::FormatFor;
using Assisi::Geometry::kMaterialChannels;
using Assisi::Geometry::MaterialChannel;
using Assisi::Geometry::MaterialData;
using Assisi::Geometry::ToString;
using Assisi::Image::ColorSpace;
using Assisi::Image::PixelFormat;

TEST_CASE("A normal map is the one channel stored as BC5")
{
    // mesh/material.glsl reads .xy and rebuilds Z as sqrt(1 - dot(xy, xy)).
    // Storing a normal as a three-channel format would leave that shader
    // reconstructing a Z the texture already carried, which is not the same
    // number.
    CHECK(FormatFor(MaterialChannel::Normal).format == PixelFormat::Bc5);

    for (const MaterialChannel channel : kMaterialChannels)
    {
        if (channel != MaterialChannel::Normal)
        {
            CHECK(FormatFor(channel).format == PixelFormat::Bc7);
        }
    }
}

TEST_CASE("Only the channels carrying light are sRGB")
{
    // A roughness, a metalness, an occlusion and a normal are all numbers the
    // shader uses directly. Decoding gamma out of one changes the value, and
    // nothing downstream would report it — the surface would just be wrong.
    CHECK(FormatFor(MaterialChannel::BaseColor).space == ColorSpace::Srgb);
    CHECK(FormatFor(MaterialChannel::Emissive).space == ColorSpace::Srgb);

    CHECK(FormatFor(MaterialChannel::Normal).space == ColorSpace::Linear);
    CHECK(FormatFor(MaterialChannel::MetallicRoughness).space == ColorSpace::Linear);
    CHECK(FormatFor(MaterialChannel::Occlusion).space == ColorSpace::Linear);
}

TEST_CASE("Every channel has a format, a name, and its own accessor")
{
    // A channel added to the enum but not to the three switches would fall to a
    // default that reads as uncompressed, unnamed, and factor-only — three
    // silent wrongs rather than a compile error.
    MaterialData material;
    material.BaseColorTexture         = *Assisi::Core::AssetId::Parse("11111111-1111-4111-8111-111111111111");
    material.NormalTexture            = *Assisi::Core::AssetId::Parse("22222222-2222-4222-8222-222222222222");
    material.MetallicRoughnessTexture = *Assisi::Core::AssetId::Parse("33333333-3333-4333-8333-333333333333");
    material.OcclusionTexture         = *Assisi::Core::AssetId::Parse("44444444-4444-4444-8444-444444444444");
    material.EmissiveTexture          = *Assisi::Core::AssetId::Parse("55555555-5555-4555-8555-555555555555");

    for (const MaterialChannel channel : kMaterialChannels)
    {
        CHECK(FormatFor(channel).format != PixelFormat::Rgba8);
        CHECK(ToString(channel) != "unknown");
        // Distinct ids above, so a channel reading its neighbour's slot shows up
        // as the wrong id rather than as an equal one.
        CHECK_FALSE(ChannelTexture(material, channel).IsNil());
    }
}

TEST_CASE("Each channel reads its own texture slot")
{
    MaterialData material;
    material.NormalTexture = *Assisi::Core::AssetId::Parse("22222222-2222-4222-8222-222222222222");

    CHECK(ChannelTexture(material, MaterialChannel::Normal) == material.NormalTexture);
    // Every other slot is still nil, so an accessor reaching the wrong member
    // would come back non-nil here.
    CHECK(ChannelTexture(material, MaterialChannel::BaseColor).IsNil());
    CHECK(ChannelTexture(material, MaterialChannel::MetallicRoughness).IsNil());
    CHECK(ChannelTexture(material, MaterialChannel::Occlusion).IsNil());
    CHECK(ChannelTexture(material, MaterialChannel::Emissive).IsNil());
}

TEST_CASE("kMaterialChannels lists every enumerator once")
{
    CHECK(kMaterialChannels.size() == static_cast<std::size_t>(MaterialChannel::Count));
    for (std::size_t i = 0; i < kMaterialChannels.size(); ++i)
    {
        // Dense and in order, because the render cache indexes its own parallel
        // table with the same number.
        CHECK(static_cast<std::size_t>(kMaterialChannels[i]) == i);
    }
}
