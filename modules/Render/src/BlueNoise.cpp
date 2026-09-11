/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/BlueNoise.hpp>

#include <array>

namespace Assisi::Render
{

namespace
{
constexpr std::array<std::uint8_t, kBlueNoiseTileSize * kBlueNoiseTileSize> kTile = {
#include "BlueNoiseTile.inl"
};
} // namespace

std::span<const std::uint8_t> BlueNoiseTile()
{
    return kTile;
}

} // namespace Assisi::Render
