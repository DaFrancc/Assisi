/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ShaderModule.hpp
/// @brief Loads compiled SPIR-V into an NVRHI shader handle.

#include <string>

#include <nvrhi/nvrhi.h>

namespace Assisi::Render
{
/// @brief Reads a compiled `.spv` file and creates an NVRHI shader from it.
///
/// @param path Virtual asset path to the compiled SPIR-V, resolved through
/// `Core::AssetSystem` (e.g. "shaders/cube_min.vert.spv"). The build compiles
/// GLSL and places the `.spv` under the asset root's `shaders/` directory (see
/// `apps/sandbox/CMakeLists.txt`), so shaders resolve exactly like every other
/// asset — no CWD dependency.
///
/// @return nullptr if the file couldn't be read or shader creation failed
/// (logged either way).
nvrhi::ShaderHandle LoadSpirvShader(nvrhi::IDevice *device, const std::string &path, nvrhi::ShaderType stage);

} /* namespace Assisi::Render */
