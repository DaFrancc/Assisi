/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file AnimatorGraphKind.cpp
/// @brief Registers the animator kind: `.sgl` files, loaded from what the cook
///        made of them.

#include <Assisi/Core/AssetKind.hpp>
#include <Assisi/Runtime/AnimatorGraph.hpp>

#include <string>

namespace Assisi::Runtime
{

namespace
{

[[maybe_unused]] const bool kRegistered =
    Core::AssetKindRegistry::Instance().Register(Core::MakeAssetKind<AnimatorGraph>(
        std::string{kAnimatorGraphKindName}, {Core::AssetFormat{.extension = ".sgl", .preferred = true}},
        ReadAnimatorGraph));

} // namespace

} // namespace Assisi::Runtime
