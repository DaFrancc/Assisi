/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Target.hpp
/// @brief Where a screen draws: the window, or a target the engine shows
/// somewhere else, such as a texture on a monitor in the world.
///
/// Screens sharing a target are ordered, hidden and drawn among themselves and
/// never against the screens of another, so a pause menu over the window does
/// not stop a monitor's screen being drawn.

#include <Assisi/Mondrian/DrawList.hpp>

#include <cstdint>
#include <functional>
#include <optional>

namespace Assisi::Mondrian
{

/// @brief Names a target. The window is kWindowTarget; Ui::AddTarget makes the
/// others.
struct TargetId
{
    uint32_t value = 0;

    bool operator==(const TargetId &) const = default;
};

/// @brief The window: what every screen draws into until given another target.
inline constexpr TargetId kWindowTarget{};

/// @brief What the engine made to show a target, such as the texture slot a
/// material reads it from. The UI only keeps it for whoever asks.
struct TargetTexture
{
    uint32_t value = 0;

    bool operator==(const TargetTexture &) const = default;
};

/// @brief Makes what shows a target of @p size, named @p target; nullopt, logged
/// by the engine, when it cannot.
using TargetMaker = std::function<std::optional<TargetTexture>(TargetId target, Extent size)>;

/// @brief Lets go of what TargetMaker made for @p target.
using TargetReleaser = std::function<void (TargetId target)>;

/// @brief Where the window pointer at @p pointer lands on a target, in that
/// target's pixels, or nullopt where it is not over it.
using PointerMap = std::function<std::optional<Point>(Point pointer)>;

} // namespace Assisi::Mondrian
