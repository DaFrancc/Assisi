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
/// Not routed through `Core::AssetSystem`: the build places compiled `.spv`
/// output next to the executable (see e.g. `apps/sandbox/CMakeLists.txt`), not
/// under `assets/`. See docs/nvrhi-migration-todo.md section 4 for the pending
/// decision on unifying the shader-compilation pipeline.
///
/// @return nullptr if the file couldn't be read or shader creation failed
/// (logged either way).
nvrhi::ShaderHandle LoadSpirvShader(nvrhi::IDevice *device, const std::string &path, nvrhi::ShaderType stage);

} /* namespace Assisi::Render */
