/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file AnimationCook.cpp
/// @brief Registers the animation kind's cook step: a clip file that would not
///        play fails the cook, and one that would ships as its keys.

#include <Assisi/Core/AssetKind.hpp>
#include <Assisi/Geometry/AnimationClip.hpp>
#include <Assisi/Geometry/AnimationImport.hpp>

namespace Assisi::Geometry
{

namespace
{

[[maybe_unused]] const bool kRegistered = Core::AssetKindRegistry::Instance().RegisterCookStep(
    Core::MakeAssetCookStep(kAnimationKind, kAnimationPayloadVersion, CookAnimation));

} // namespace

} // namespace Assisi::Geometry
