/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ShaderModule.hpp
/// @brief Loads compiled SPIR-V into an NVRHI shader handle.

#include <string>

#include <nvrhi/nvrhi.h>

namespace Assisi::Render
{
/// @brief Loads a compiled shader stage and creates an NVRHI shader from it.
///
/// @param path Virtual path of the compiled SPIR-V (e.g. "shaders/mesh.vert.spv"),
/// read through the installed AssetSource — the source tree's `.spv` in the
/// editor, a cooked blob in a shipped game.
///
/// @return nullptr if the stage couldn't be loaded or shader creation failed
/// (logged either way).
nvrhi::ShaderHandle LoadSpirvShader(nvrhi::IDevice *device, const std::string &path, nvrhi::ShaderType stage);

} /* namespace Assisi::Render */
