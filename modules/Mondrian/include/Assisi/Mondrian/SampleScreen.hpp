/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SampleScreen.hpp
/// @brief A screen holding one of everything, for looking at rather than
/// playing.
///
/// It exercises every kind of sizing, every built-in control and both ways text
/// can be set, so a capture at two window sizes shows at a glance whether
/// layout reflows and whether anything draws wrong. The tests build it for the
/// same reason.

#include <Assisi/Mondrian/DrawList.hpp>
#include <Assisi/Mondrian/Screen.hpp>

namespace Assisi::Mondrian
{

class Ui;

/// The name the sample screen is created under, for finding it again.
inline constexpr std::string_view kSampleScreenName = "Sample";

/// @brief Builds the sample screen into @p ui and returns it, hidden until it
/// is shown. Its picture shows @p picture.
Screen *AddSampleScreen(Ui &ui, TextureId picture);

} // namespace Assisi::Mondrian
