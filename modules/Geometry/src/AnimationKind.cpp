/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file AnimationKind.cpp
/// @brief Registers the animation kind: the formats a clip file comes in, and
///        loading a cooked one.

#include <Assisi/Core/AssetKind.hpp>
#include <Assisi/Geometry/AnimationClip.hpp>
#include <Assisi/Geometry/AnimationFile.hpp>

#include <string>
#include <utility>
#include <vector>

namespace Assisi::Geometry
{

namespace
{

bool Register()
{
    // Not preferred: a new .glb is a model far more often than a clip, so it
    // stays a mesh. Clip files are written with their kind already named.
    std::vector<Core::AssetFormat> formats{Core::AssetFormat{.extension = ".glb"},
                                           Core::AssetFormat{.extension = ".gltf"}};
    Core::AssetKind kind =
        Core::MakeAssetKind<AnimationClip>(std::string{kAnimationKindName}, std::move(formats), LoadAnimation);
    return Core::AssetKindRegistry::Instance().Register(std::move(kind));
}

[[maybe_unused]] const bool kRegistered = Register();

} // namespace

} // namespace Assisi::Geometry
